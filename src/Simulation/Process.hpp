#pragma once
#include "Gateway/Channel.hpp"
#include "Simulation/Protocol.hpp"
#include <chrono>
#include <deque>
#include <optional>

namespace AtlasNetGMod::Simulation
{
class Process
{
public:
  ~Process();
  State Start(const char* launcher, const char* shard);
  bool Send(const Message& message);
  std::optional<Message> Read();
  std::optional<Message> Wait(Action action, std::uint64_t entity,
                              std::chrono::milliseconds timeout);
  void Stop() noexcept;
  bool Alive() const { return static_cast<bool>(channel); }

private:
  Gateway::Socket channel;
  pid_t pid = -1;
  std::deque<Message> pending;
};
}
