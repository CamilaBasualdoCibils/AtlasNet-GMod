#include <AtlasNet/Core/Memory/Memory.hpp>
#include "Server/SimulationLua.hpp"
#include <GarrysMod/Lua/Interface.h>

#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace AtlasNetGMod
{
void RegisterGateway(lua_State* state);
void CloseGateway() noexcept;
}

namespace
{
constexpr char Version[] = "AtlasNet-GMod " ATLASNET_GMOD_VERSION;
static_assert(offsetof(lua_State, luabase) == 120);

struct Runtime
{
  GarrysMod::Lua::ILuaBase* lua;
};

std::unique_ptr<Runtime, AtlasNet::Memory::Deleter<Runtime>> runtime;

int LuaVersion(lua_State* state)
{
  LUA->SetState(state);
  LUA->PushString(Version);
  return 1;
}

int LuaIsInitialized(lua_State* state)
{
  LUA->SetState(state);
  LUA->PushBool(runtime && runtime->lua == LUA);
  return 1;
}

int LuaWorldOrigin(lua_State* state)
{
  LUA->SetState(state);
  for (const auto* axis : {"ATLASNET_GMOD_ORIGIN_X", "ATLASNET_GMOD_ORIGIN_Y", "ATLASNET_GMOD_ORIGIN_Z"})
  {
    const auto* text = std::getenv(axis);
    char* end = nullptr;
    const double value = text ? std::strtod(text, &end) : 0;
    if (text && (end == text || *end || !std::isfinite(value)))
    {
      LUA->ThrowError(axis);
      return 0;
    }
    LUA->PushNumber(value);
  }
  return 3;
}

void Log(const char* message)
{
  std::fprintf(stdout, "[AtlasNet-GMod] %s\n", message);
  std::fflush(stdout);
}
}

GMOD_MODULE_OPEN()
{
  LUA->SetState(state);
  if (runtime)
    return 0;

  try
  {
    runtime.reset(AtlasNet::Memory::New<Runtime>(LUA));
  }
  catch (const std::bad_alloc&)
  {
  }
  if (!runtime)
  {
    LUA->ThrowError("[AtlasNet-GMod] Could not allocate AtlasNet runtime state");
    return 0;
  }

  LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
  LUA->CreateTable();
  LUA->PushCFunction(LuaVersion);
  LUA->SetField(-2, "Version");
  LUA->PushCFunction(LuaIsInitialized);
  LUA->SetField(-2, "IsInitialized");
  LUA->PushCFunction(LuaWorldOrigin);
  LUA->SetField(-2, "WorldOrigin");
  AtlasNetGMod::RegisterGateway(state);
  AtlasNetGMod::RegisterSimulation(state);
  LUA->SetField(-2, "atlasnet");
  LUA->Pop();

  Log("Native module initialized");
  return 0;
}

GMOD_MODULE_CLOSE()
{
  if (runtime && runtime->lua == LUA)
  {
    AtlasNetGMod::CloseGateway();
    AtlasNetGMod::CloseSimulation();
    runtime.reset();
    Log("Native module shut down");
  }
  return 0;
}
