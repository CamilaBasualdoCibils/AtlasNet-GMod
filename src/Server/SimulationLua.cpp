#include "Server/SimulationLua.hpp"
#include "Gateway/Channel.hpp"

#include <charconv>
#include <cstdlib>
#include <string>

namespace AtlasNetGMod
{
namespace
{
using namespace Simulation;
using GarrysMod::Lua::ILuaBase;
Gateway::Socket engine;

void Field(ILuaBase& lua, const char* name, double value)
{
  lua.PushNumber(value);
  lua.SetField(-2, name);
}

void Field(ILuaBase& lua, const char* name, const char* value)
{
  lua.PushString(value);
  lua.SetField(-2, name);
}

double Number(ILuaBase& lua, const char* name)
{
  lua.GetField(1, name);
  const auto value = lua.GetNumber();
  lua.Pop();
  return value;
}

void PushState(ILuaBase& lua, std::uint64_t entity, const State& v)
{
  lua.CreateTable();
  Field(lua, "id", std::to_string(entity).c_str());
  Field(lua, "kind", static_cast<unsigned>(v.kind));
  Field(lua, "x", v.position.x); Field(lua, "y", v.position.y); Field(lua, "z", v.position.z);
  Field(lua, "vx", v.velocity.x); Field(lua, "vy", v.velocity.y); Field(lua, "vz", v.velocity.z);
  Field(lua, "pitch", v.pitch); Field(lua, "yaw", v.yaw);
  Field(lua, "eye", v.eye); Field(lua, "health", v.health);
  Field(lua, "armor", v.armor);
  Field(lua, "respawnDelay", v.respawnDelay);
  Field(lua, "fuse", v.fuse);
  Field(lua, "alive", v.alive ? 1.0 : 0.0);
  Field(lua, "motion", v.motion ? 1.0 : 0.0);
  Field(lua, "gait", static_cast<unsigned>(v.gait));
  Field(lua, "hold", v.hold);
  Field(lua, "revision", v.revision);
  Field(lua, "model", v.model.data());
  Field(lua, "weapon", v.weapon.data());
}

int Read(lua_State* state)
{
  LUA->SetState(state);
  Message message;
  const auto result = Gateway::ReceiveRecord(engine, message);
  if (result == Gateway::IO::Busy)
    return 0;
  if (result == Gateway::IO::Closed)
  {
    engine.Reset();
    message.action = Action::Lost;
  }
  PushState(*LUA, message.entity, message.state);
  Field(*LUA, "action", static_cast<unsigned>(message.action));
  Field(*LUA, "forward", message.input.forward);
  Field(*LUA, "side", message.input.side);
  Field(*LUA, "aimPitch", message.input.pitch);
  Field(*LUA, "aimYaw", message.input.yaw);
  Field(*LUA, "buttons", message.input.buttons);
  Field(*LUA, "distance", message.bullet.distance);
  Field(*LUA, "traceHull", message.traceHull);
  Field(*LUA, "dx", message.bullet.direction.x);
  Field(*LUA, "dy", message.bullet.direction.y);
  Field(*LUA, "dz", message.bullet.direction.z);
  Field(*LUA, "damage", message.bullet.damage);
  Field(*LUA, "damageType", message.bullet.damageType);
  Field(*LUA, "target", std::to_string(message.target).c_str());
  if (message.action == Action::Blast)
  {
    Field(*LUA, "x", message.blast.position.x);
    Field(*LUA, "y", message.blast.position.y);
    Field(*LUA, "z", message.blast.position.z);
    Field(*LUA, "damage", message.blast.damage);
    Field(*LUA, "radius", message.blast.radius);
  }
  if (message.action == Action::PhysicsTool)
  {
    Field(*LUA, "tool", static_cast<unsigned>(message.physics.tool));
    Field(*LUA, "operation", static_cast<unsigned>(message.physics.action));
    Field(*LUA, "x", message.physics.point.x);
    Field(*LUA, "y", message.physics.point.y);
    Field(*LUA, "z", message.physics.point.z);
    Field(*LUA, "dx", message.physics.direction.x);
    Field(*LUA, "dy", message.physics.direction.y);
    Field(*LUA, "dz", message.physics.direction.z);
  }
  return 1;
}

int Report(lua_State* state)
{
  LUA->SetState(state);
  LUA->CheckType(1, GarrysMod::Lua::Type::TABLE);
  Message message;
  message.action = static_cast<Action>(static_cast<unsigned>(Number(*LUA, "action")));
  auto readID = [&](const char* key, std::uint64_t& destination)
  {
    LUA->GetField(1, key);
    if (const auto* id = LUA->GetString())
    {
      const std::string_view text(id);
      std::from_chars(text.data(), text.data() + text.size(), destination);
    }
    LUA->Pop();
  };
  readID("id", message.entity);
  readID("target", message.target);
  LUA->GetField(1, "ok");
  message.ok = LUA->GetBool();
  LUA->Pop();
  const auto read = [&](const char* field) { return static_cast<float>(Number(*LUA, field)); };
  message.state.position = {read("x"), read("y"), read("z")};
  message.state.velocity = {read("vx"), read("vy"), read("vz")};
  message.state.mins = {read("minx"), read("miny"), read("minz")};
  message.state.maxs = {read("maxx"), read("maxy"), read("maxz")};
  message.state.pitch = read("pitch");
  message.state.yaw = read("yaw");
  message.state.eye = read("eye");
  message.state.health = read("health");
  message.state.armor = read("armor");
  message.state.respawnDelay = read("respawnDelay");
  message.state.fuse = read("fuse");
  message.state.alive = Number(*LUA, "alive") != 0;
  message.state.motion = Number(*LUA, "motion") != 0;
  message.state.gait = static_cast<Gait>(static_cast<unsigned>(Number(*LUA, "gait")));
  LUA->GetField(1, "model");
  if (const auto* model = LUA->GetString())
  {
    const std::string_view path(model);
    if (path.size() < message.state.model.size())
      std::copy(path.begin(), path.end(), message.state.model.begin());
  }
  LUA->Pop();
  message.bullet.distance = read("distance");
  LUA->PushBool(Gateway::SendRecord(engine, message) == Gateway::IO::Ok);
  return 1;
}
}

void PushView(ILuaBase& lua, const Simulation::View& view)
{
  PushState(lua, view.entity, view.state);
  Field(lua, "action", static_cast<unsigned>(view.action));
  Field(lua, "shard", view.shard);
  Field(lua, "region", view.region);
}

void RegisterSimulation(lua_State* state)
{
  const char* fd = std::getenv("ATLASNET_GMOD_SIM_FD");
  if (!fd)
    return;
  const std::string_view value(fd);
  int descriptor = -1;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), descriptor);
  int type{};
  socklen_t size = sizeof(type);
  if (parsed.ec != std::errc{} || descriptor < 3 ||
      getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &size) < 0 || type != SOCK_SEQPACKET)
    return;
  engine.Reset(descriptor);
  LUA->PushCFunction(Read);
  LUA->SetField(-2, "SimulationRead");
  LUA->PushCFunction(Report);
  LUA->SetField(-2, "SimulationReport");
}

void CloseSimulation() noexcept { engine.Reset(); }
}
