#include "Simulation/Process.hpp"

#include <cerrno>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>

extern char** environ;

namespace AtlasNetGMod::Simulation
{
using Gateway::IO;
using namespace std::chrono_literals;

Process::~Process() { Stop(); }

State Process::Start(const char* launcher, const char* shard)
{
  int sockets[2];
  Gateway::Check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
                            0, sockets), "GMod simulation socketpair");
  Gateway::Socket master(sockets[0]), child(sockets[1]);
  constexpr int engineFD = 198;
  posix_spawn_file_actions_t actions;
  auto check = [](int error)
  {
    if (error)
      throw std::system_error(error, std::generic_category(), "Spawn GMod simulation");
  };
  check(posix_spawn_file_actions_init(&actions));
  int result = posix_spawn_file_actions_adddup2(&actions, child.Get(), engineFD);
  char fd[] = "198";
  char* argv[]{const_cast<char*>(launcher), const_cast<char*>(shard), fd, nullptr};
  if (!result)
    result = posix_spawn(&pid, launcher, &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  check(result);
  child.Reset();
  channel = std::move(master);
  const auto ready = Wait(Action::Ready, 0, 30s);
  if (!ready || !ready->ok)
    throw std::runtime_error("GMod simulation did not report readiness");
  return ready->state;
}

bool Process::Send(const Message& message)
{
  const auto result = Gateway::SendRecord(channel, message);
  if (result == IO::Closed)
    channel.Reset();
  return result == IO::Ok;
}

std::optional<Message> Process::Read()
{
  if (!pending.empty())
  {
    auto result = pending.front();
    pending.pop_front();
    return result;
  }
  Message message;
  const auto result = Gateway::ReceiveRecord(channel, message);
  if (result == IO::Closed)
    channel.Reset();
  return result == IO::Ok ? std::optional(message) : std::nullopt;
}

std::optional<Message> Process::Wait(Action action, std::uint64_t entity,
                                     std::chrono::milliseconds timeout)
{
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + timeout;
  while (channel && Clock::now() < deadline)
  {
    pollfd fd{channel.Get(), POLLIN, 0};
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    if (poll(&fd, 1, static_cast<int>(std::max(left, 0ms).count())) <= 0)
      continue;
    Message message;
    const auto result = Gateway::ReceiveRecord(channel, message);
    if (result == IO::Closed)
      channel.Reset();
    if (result != IO::Ok)
      continue;
    if (message.action == action && message.entity == entity)
      return message;
    if (message.action == Action::Sample || message.action == Action::Removed ||
        message.action == Action::Detonated)
    {
      if (pending.size() == 256)
        pending.pop_front();
      pending.push_back(message);
    }
  }
  return std::nullopt;
}

void Process::Stop() noexcept
{
  channel.Reset();
  if (pid <= 0)
    return;
  const auto process = std::exchange(pid, -1);
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    const auto status = waitpid(process, nullptr, WNOHANG);
    if (status == process || (status < 0 && errno == ECHILD))
      return;
    std::this_thread::sleep_for(20ms);
  }
  kill(process, SIGKILL);
  while (waitpid(process, nullptr, 0) < 0 && errno == EINTR) {}
}
}
