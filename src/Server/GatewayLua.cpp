#include "Gateway/Channel.hpp"
#include "Server/SimulationLua.hpp"

#include <GarrysMod/Lua/Interface.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <poll.h>

namespace AtlasNetGMod
{
namespace
{
using namespace Gateway;
using namespace std::chrono_literals;

Socket channel;
std::deque<Frame> pending;
std::uint32_t nextRequest = 0;
bool lost = false;

int Fail(GarrysMod::Lua::ILuaBase* lua, std::string_view reason)
{
  lua->PushNil();
  lua->PushString(reason.data(), static_cast<unsigned>(reason.size()));
  return 2;
}

void Lost() noexcept
{
  lost = true;
  channel.Reset();
  pending.clear();
}

int Start(lua_State* state)
{
  LUA->SetState(state);
  if (channel)
  {
    LUA->PushBool(true);
    return 1;
  }
  const auto* path = std::getenv("ATLASNET_GMOD_GATEWAY_SOCKET");
  if (!path)
  {
    LUA->PushBool(false);
    return 1;
  }
  try
  {
    const auto address = UnixAddress(path);
    Socket connected(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    Check(connected.Get(), "Gateway socket");
    Check(connect(connected.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
          "Connect to source1 gateway");
    channel = std::move(connected);
    lost = false;
  }
  catch (const std::exception& error)
  {
    std::fprintf(stderr, "[AtlasNet-GMod] %s\n", error.what());
  }
  LUA->PushBool(static_cast<bool>(channel));
  return 1;
}

std::uint16_t BackendPort(std::string_view address)
{
  constexpr std::string_view prefix = "127.0.0.2:";
  if (!address.starts_with(prefix))
    return 0;
  address.remove_prefix(prefix.size());
  std::uint16_t port = 0;
  const auto result = std::from_chars(address.data(), address.data() + address.size(), port);
  return result.ec == std::errc{} && result.ptr == address.data() + address.size() ? port : 0;
}

int Port(lua_State* state)
{
  LUA->SetState(state);
  LUA->PushNumber(BackendPort(LUA->CheckString(1)));
  return 1;
}

int Admit(lua_State* state)
{
  LUA->SetState(state);
  const auto port = BackendPort(LUA->CheckString(1));
  const std::string_view steamID = LUA->CheckString(2);
  if (!port)
    return Fail(LUA, "Connect through the AtlasNet ingress address");
  if (!channel)
    return Fail(LUA, "AtlasNet gateway is offline");

  Frame request{.event = Event::Admit, .port = port, .value = ++nextRequest};
  request.SetText(steamID);
  if (Send(channel, request) != IO::Ok)
  {
    Lost();
    return Fail(LUA, "AtlasNet gateway is unavailable");
  }

  // CheckPassword needs a decision before returning. The node evaluates and
  // spawns asynchronously, on its own threads, while GMod waits with a deadline.
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + 4s;
  while (Clock::now() < deadline)
  {
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    pollfd descriptors{channel.Get(), POLLIN, 0};
    const int ready = poll(&descriptors, 1, static_cast<int>(std::max(remaining, 0ms).count()));
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0)
      break;
    Frame response;
    const auto received = Receive(channel, response);
    if (received == IO::Closed || pending.size() >= 64)
    {
      Lost();
      return Fail(LUA, "AtlasNet gateway disconnected");
    }
    if (received == IO::Busy)
      continue;
    if (response.event != Event::AdmissionResult)
      pending.push_back(response);
    else if (response.port == port && response.value == request.value)
    {
      if (!response.token)
        return Fail(LUA, response.Text());
      LUA->PushNumber(response.token);
      LUA->PushString(response.text.data(), response.size);
      return 2;
    }
  }

  request.event = Event::Cancel;
  request.size = 0;
  if (Send(channel, request) != IO::Ok)
    Lost();
  return Fail(LUA, "AtlasNet admission timed out");
}

int SendEvent(lua_State* state)
{
  LUA->SetState(state);
  const std::string_view event = LUA->CheckString(1);
  Event type;
  if (event == "established")
    type = Event::Established;
  else if (event == "disconnect")
    type = Event::Disconnect;
  else if (event == "chat")
    type = Event::Chat;
  else if (event == "delivered")
    type = Event::Delivered;
  else if (event == "prop")
    type = Event::Prop;
  else if (event == "marker")
    type = Event::Marker;
  else if (event == "respawn")
    type = Event::Respawn;
  else if (event == "removeProp")
    type = Event::RemoveProp;
  else
  {
    LUA->PushBool(false);
    return 1;
  }
  const auto port = LUA->CheckNumber(2);
  const auto token = LUA->CheckNumber(3);
  const auto value = LUA->GetNumber(5);
  if (!(port >= 1 && port <= UINT16_MAX && token >= 1 && token <= UINT32_MAX &&
        value >= 0 && value <= UINT32_MAX))
  {
    LUA->PushBool(false);
    return 1;
  }
  Frame frame{.event = type, .port = static_cast<std::uint16_t>(port),
              .token = static_cast<std::uint32_t>(token), .value = static_cast<std::uint32_t>(value)};
  if (type == Event::Chat || type == Event::Delivered)
  {
    unsigned length = 0;
    const auto* text = LUA->GetString(4, &length);
    if (text)
      frame.SetText({text, length});
  }
  const bool sent = channel && Send(channel, frame) == IO::Ok;
  if (!sent && channel)
    Lost();
  LUA->PushBool(sent);
  return 1;
}

int SendInput(lua_State* state)
{
  LUA->SetState(state);
  const auto port = LUA->CheckNumber(1);
  const auto token = LUA->CheckNumber(2);
  const auto buttons = LUA->CheckNumber(7);
  if (!(port >= 1 && port <= UINT16_MAX && token >= 1 && token <= UINT32_MAX &&
        buttons >= 0 && buttons <= UINT32_MAX))
    return 0;
  const Simulation::Input input{
      static_cast<float>(LUA->CheckNumber(3)), static_cast<float>(LUA->CheckNumber(4)),
      static_cast<float>(LUA->CheckNumber(5)), static_cast<float>(LUA->CheckNumber(6)),
      static_cast<std::uint32_t>(buttons), static_cast<Simulation::PhysicsGun>(LUA->GetNumber(8)),
      static_cast<float>(LUA->GetNumber(9)), LUA->GetNumber(10) != 0,
      {static_cast<float>(LUA->GetNumber(11)), static_cast<float>(LUA->GetNumber(12)),
       static_cast<float>(LUA->GetNumber(13))}};
  Frame frame{.event = Event::Input, .port = static_cast<std::uint16_t>(port),
              .token = static_cast<std::uint32_t>(token)};
  frame.SetText({reinterpret_cast<const char*>(&input), sizeof(input)});
  if (channel && Send(channel, frame) == IO::Closed)
    Lost();
  return 0;
}

int SendBullet(lua_State* state)
{
  LUA->SetState(state);
  const auto port = LUA->CheckNumber(1);
  const auto token = LUA->CheckNumber(2);
  const auto damageType = LUA->CheckNumber(8);
  if (!(port >= 1 && port <= UINT16_MAX && token >= 1 && token <= UINT32_MAX &&
        damageType >= 0 && damageType <= UINT32_MAX))
    return 0;
  const auto number = [state](int index) { return static_cast<float>(LUA->CheckNumber(index)); };
  const Simulation::Bullet bullet{{number(3), number(4), number(5)}, number(6),
                                   number(7), static_cast<std::uint32_t>(damageType)};
  Frame frame{.event = Event::Fire, .port = static_cast<std::uint16_t>(port),
              .token = static_cast<std::uint32_t>(token)};
  frame.SetText({reinterpret_cast<const char*>(&bullet), sizeof(bullet)});
  if (channel && Send(channel, frame) == IO::Closed)
    Lost();
  return 0;
}

int SendSound(lua_State* state)
{
  LUA->SetState(state);
  const auto port = LUA->CheckNumber(1);
  const auto token = LUA->CheckNumber(2);
  const std::string_view name = LUA->CheckString(3);
  const auto volume = LUA->CheckNumber(4);
  const auto soundChannel = LUA->CheckNumber(5);
  const auto level = LUA->CheckNumber(6);
  const auto pitch = LUA->CheckNumber(7);
  const auto dsp = LUA->CheckNumber(8);
  Simulation::Sound sound;
  if (!(port >= 1 && port <= UINT16_MAX && token >= 1 && token <= UINT32_MAX &&
        !name.empty() && name.size() < sound.name.size() && volume > 0 && volume <= 1 &&
        soundChannel >= -1 && soundChannel <= INT16_MAX && level >= 0 && level <= 511 &&
        pitch >= 0 && pitch <= 255 && dsp >= 0 && dsp <= 255))
    return 0;
  std::copy(name.begin(), name.end(), sound.name.begin());
  sound.volume = static_cast<float>(volume);
  sound.channel = static_cast<std::int16_t>(soundChannel);
  sound.level = static_cast<std::uint16_t>(level);
  sound.pitch = static_cast<std::uint8_t>(pitch);
  sound.dsp = static_cast<std::uint8_t>(dsp);
  Frame frame{.event = Event::Sound, .port = static_cast<std::uint16_t>(port),
              .token = static_cast<std::uint32_t>(token)};
  frame.SetText({reinterpret_cast<const char*>(&sound), sizeof(sound)});
  if (channel && Send(channel, frame) == IO::Closed)
    Lost();
  return 0;
}

int SendBlast(lua_State* state)
{
  LUA->SetState(state);
  const auto port = LUA->CheckNumber(1);
  const auto token = LUA->CheckNumber(2);
  if (!(port >= 1 && port <= UINT16_MAX && token >= 1 && token <= UINT32_MAX))
    return 0;
  const auto number = [state](int index) { return static_cast<float>(LUA->CheckNumber(index)); };
  const Simulation::Blast blast{{number(3), number(4), number(5)}, number(6), number(7)};
  Frame frame{.event = Event::Blast, .port = static_cast<std::uint16_t>(port),
              .token = static_cast<std::uint32_t>(token)};
  frame.SetText({reinterpret_cast<const char*>(&blast), sizeof(blast)});
  if (channel && Send(channel, frame) == IO::Closed)
    Lost();
  return 0;
}

int SendGrenade(lua_State* state)
{
  LUA->SetState(state);
  const auto port = LUA->CheckNumber(1);
  const auto token = LUA->CheckNumber(2);
  if (!(port >= 1 && port <= UINT16_MAX && token >= 1 && token <= UINT32_MAX))
  {
    LUA->PushBool(false);
    return 1;
  }
  const auto number = [state](int index) { return static_cast<float>(LUA->CheckNumber(index)); };
  const Simulation::GrenadeLaunch launch{
      {{number(3), number(4), number(5)}, number(9), number(10)},
      {number(6), number(7), number(8)}, number(11),
      static_cast<Simulation::Kind>(static_cast<unsigned>(LUA->CheckNumber(12))),
      number(13), number(14)};
  Frame frame{.event = Event::Grenade, .port = static_cast<std::uint16_t>(port),
              .token = static_cast<std::uint32_t>(token)};
  frame.SetText({reinterpret_cast<const char*>(&launch), sizeof(launch)});
  const auto result = channel ? Send(channel, frame) : IO::Closed;
  if (channel && result == IO::Closed)
    Lost();
  LUA->PushBool(result == IO::Ok);
  return 1;
}

int SendProp(lua_State* state)
{
  LUA->SetState(state);
  const auto port = LUA->CheckNumber(1);
  const auto token = LUA->CheckNumber(2);
  const std::string_view model = LUA->CheckString(3);
  Simulation::PropPlacement prop;
  if (!(port >= 1 && port <= UINT16_MAX && token >= 1 && token <= UINT32_MAX &&
        !model.empty() && model.size() < prop.model.size()))
  {
    LUA->PushBool(false);
    return 1;
  }
  const auto number = [state](int index) { return static_cast<float>(LUA->CheckNumber(index)); };
  std::copy(model.begin(), model.end(), prop.model.begin());
  prop.position = {number(4), number(5), number(6)};
  prop.pitch = number(7);
  prop.yaw = number(8);
  Frame frame{.event = Event::SpawnProp, .port = static_cast<std::uint16_t>(port),
              .token = static_cast<std::uint32_t>(token)};
  frame.SetText({reinterpret_cast<const char*>(&prop), sizeof(prop)});
  const auto result = channel ? Send(channel, frame) : IO::Closed;
  if (channel && result == IO::Closed)
    Lost();
  LUA->PushBool(result == IO::Ok);
  return 1;
}

int SendWeaponView(lua_State* state)
{
  LUA->SetState(state);
  const auto port = LUA->CheckNumber(1);
  const auto token = LUA->CheckNumber(2);
  const std::string_view model = LUA->CheckString(3);
  const auto hold = LUA->CheckNumber(4);
  const std::string_view playerModel = LUA->CheckString(5);
  Simulation::WeaponPresentation weapon;
  if (!(port >= 1 && port <= UINT16_MAX && token >= 1 && token <= UINT32_MAX &&
        model.size() < weapon.model.size() && !playerModel.empty() &&
        playerModel.size() < weapon.playerModel.size() && hold >= 0 && hold <= UINT8_MAX))
  {
    LUA->PushBool(false);
    return 1;
  }
  std::copy(model.begin(), model.end(), weapon.model.begin());
  std::copy(playerModel.begin(), playerModel.end(), weapon.playerModel.begin());
  weapon.hold = static_cast<std::uint8_t>(hold);
  Frame frame{.event = Event::WeaponView, .port = static_cast<std::uint16_t>(port),
              .token = static_cast<std::uint32_t>(token)};
  frame.SetText({reinterpret_cast<const char*>(&weapon), sizeof(weapon)});
  const auto result = channel ? Send(channel, frame) : IO::Closed;
  if (channel && result == IO::Closed)
    Lost();
  LUA->PushBool(result == IO::Ok);
  return 1;
}

int Poll(lua_State* state)
{
  LUA->SetState(state);
  if (!channel)
  {
    if (!std::exchange(lost, false))
      return 0;
    LUA->PushString("lost");
    return 1;
  }

  Frame frame;
  if (!pending.empty())
  {
    frame = pending.front();
    pending.pop_front();
  }
  else
  {
    const auto result = Receive(channel, frame);
    if (result == IO::Closed)
    {
      Lost();
      lost = false;
      LUA->PushString("lost");
      return 1;
    }
    if (result == IO::Busy)
      return 0;
  }
  const bool world = frame.event == Event::World || frame.event == Event::WorldReliable;
  const bool sound = frame.event == Event::Sound && frame.size == sizeof(Simulation::SoundPlayback);
  if (frame.event != Event::Signal && frame.event != Event::Kick &&
      !sound && (!world || frame.size != sizeof(Simulation::View)))
    return 0;
  LUA->PushString(world ? (frame.event == Event::World ? "world" : "worldReliable") :
                  sound ? "sound" : frame.event == Event::Signal ? "signal" : "kick");
  LUA->PushNumber(frame.port);
  LUA->PushNumber(frame.token);
  if (world)
  {
    Simulation::View view;
    std::memcpy(&view, frame.text.data(), sizeof(view));
    PushView(*LUA, view);
  }
  else if (sound)
  {
    Simulation::SoundPlayback playback;
    std::memcpy(&playback, frame.text.data(), sizeof(playback));
    LUA->CreateTable();
    LUA->PushString(playback.sound.name.data());
    LUA->SetField(-2, "name");
    const auto field = [state](const char* key, double value)
    {
      LUA->PushNumber(value);
      LUA->SetField(-2, key);
    };
    field("x", playback.position.x);
    field("y", playback.position.y);
    field("z", playback.position.z);
    field("endX", playback.endPosition.x);
    field("endY", playback.endPosition.y);
    field("endZ", playback.endPosition.z);
    LUA->PushBool(playback.tracer);
    LUA->SetField(-2, "tracer");
    field("channel", playback.sound.channel);
    field("volume", playback.sound.volume);
    field("level", playback.sound.level);
    field("pitch", playback.sound.pitch);
    field("dsp", playback.sound.dsp);
    LUA->PushBool(playback.explosion);
    LUA->SetField(-2, "explosion");
  }
  else LUA->PushString(frame.text.data(), frame.size);
  LUA->PushNumber(frame.value);
  return 5;
}
}

void RegisterGateway(lua_State* state)
{
  if (!std::getenv("ATLASNET_GMOD_GATEWAY_SOCKET"))
    return;
  LUA->PushCFunction(Start);
  LUA->SetField(-2, "GatewayStart");
  LUA->PushCFunction(Port);
  LUA->SetField(-2, "GatewayPort");
  LUA->PushCFunction(Admit);
  LUA->SetField(-2, "GatewayAdmit");
  LUA->PushCFunction(SendEvent);
  LUA->SetField(-2, "GatewaySend");
  LUA->PushCFunction(Poll);
  LUA->SetField(-2, "GatewayPoll");
  if (std::getenv("ATLASNET_GMOD_DISTRIBUTED"))
  {
    LUA->PushCFunction(SendInput);
    LUA->SetField(-2, "GatewayInput");
    LUA->PushCFunction(SendBullet);
    LUA->SetField(-2, "GatewayBullet");
    LUA->PushCFunction(SendSound);
    LUA->SetField(-2, "GatewaySound");
    LUA->PushCFunction(SendBlast);
    LUA->SetField(-2, "GatewayBlast");
    LUA->PushCFunction(SendGrenade);
    LUA->SetField(-2, "GatewayProjectile");
    LUA->PushCFunction(SendProp);
    LUA->SetField(-2, "GatewayProp");
    LUA->PushCFunction(SendWeaponView);
    LUA->SetField(-2, "GatewayWeaponView");
  }
}

void CloseGateway() noexcept
{
  channel.Reset();
  pending.clear();
  lost = false;
}
} // namespace AtlasNetGMod
