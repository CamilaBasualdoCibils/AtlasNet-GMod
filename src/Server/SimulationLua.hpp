#pragma once
#include "Simulation/Protocol.hpp"
#include <GarrysMod/Lua/Interface.h>

namespace AtlasNetGMod
{
void RegisterSimulation(lua_State* state);
void CloseSimulation() noexcept;
void PushView(GarrysMod::Lua::ILuaBase& lua, const Simulation::View& view);
}
