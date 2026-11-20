#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace AtlasNetGMod::Gateway
{
class Socket
{
public:
  explicit Socket(int fd = -1) noexcept : fd(fd) {}
  ~Socket() { Reset(); }
  Socket(Socket&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
  Socket& operator=(Socket&& other) noexcept
  {
    Reset(std::exchange(other.fd, -1));
    return *this;
  }

  int Get() const noexcept { return fd; }
  explicit operator bool() const noexcept { return fd >= 0; }

  void Reset(int replacement = -1) noexcept
  {
    if (fd >= 0)
      close(fd);
    fd = replacement;
  }

private:
  int fd;
};

inline void Check(int result, const char* operation)
{
  if (result < 0)
    throw std::system_error(errno, std::generic_category(), operation);
}

inline sockaddr_un UnixAddress(std::string_view path)
{
  sockaddr_un address{.sun_family = AF_UNIX};
  if (path.empty() || path.front() != '/' || path.size() >= sizeof(address.sun_path))
    throw std::invalid_argument("Gateway socket requires a short, absolute filesystem path");
  std::memcpy(address.sun_path, path.data(), path.size());
  return address;
}

inline constexpr std::size_t MaxText = 240; // Fits the native ChatPrint limit.
inline constexpr std::size_t MaxFramePayload = 768;

enum class Event : std::uint16_t
{
  Admit, AdmissionResult, Cancel, Established, Disconnect, Chat, Signal, Delivered, Kick,
  Input, Fire, Prop, Marker, World, WorldReliable, Sound, Blast, Grenade, SpawnProp, Respawn, WeaponView, RemoveProp
};

// Local IPC record shared by the two build targets. Source UDP and AtlasNet V1
// framing stay on either side of this channel.
struct Frame
{
  Event event{};
  std::uint16_t port = 0;
  std::uint32_t token = 0;
  std::uint32_t value = 0;
  std::uint16_t size = 0;
  std::array<char, MaxFramePayload> text{};
  std::uint16_t reserved = 0;

  void SetText(std::string_view value)
  {
    size = static_cast<std::uint16_t>(std::min(value.size(), text.size()));
    std::copy_n(value.begin(), size, text.begin());
  }
  std::string_view Text() const { return {text.data(), size}; }
};
static_assert(sizeof(Frame) == MaxFramePayload + 16);

enum class IO { Ok, Busy, Closed };

template<class Record>
IO SendRecord(const Socket& socket, const Record& message) noexcept
{
  ssize_t sent;
  do {
    sent = send(socket.Get(), &message, sizeof(message), MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (sent < 0 && errno == EINTR);
  if (sent == sizeof(message))
    return IO::Ok;
  return sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? IO::Busy : IO::Closed;
}

template<class Record>
IO ReceiveRecord(const Socket& socket, Record& message) noexcept
{
  ssize_t received;
  do {
    received = recv(socket.Get(), &message, sizeof(message), MSG_DONTWAIT | MSG_TRUNC);
  } while (received < 0 && errno == EINTR);
  if (received == sizeof(message))
    return IO::Ok;
  return received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? IO::Busy : IO::Closed;
}
inline IO Send(const Socket& socket, const Frame& message) noexcept
{
  return SendRecord(socket, message);
}

inline IO Receive(const Socket& socket, Frame& message) noexcept
{
  const auto result = ReceiveRecord(socket, message);
  if (result == IO::Ok && (message.size > message.text.size() || message.reserved != 0))
    return IO::Closed;
  return result;
}
} // namespace AtlasNetGMod::Gateway
