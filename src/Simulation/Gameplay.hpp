#pragma once

#include "Simulation/Protocol.hpp"
#include <AtlasNet/Core/CmdSig/Command.hpp>
#include <string>

namespace AtlasNetGMod::Simulation
{
using AtlasNet::CmdSig::CommandNameToID;
inline constexpr auto MoveCommand = CommandNameToID("AtlasNetGMod.Move");
inline constexpr auto FireCommand = CommandNameToID("AtlasNetGMod.Fire");
inline constexpr auto SoundCommand = CommandNameToID("AtlasNetGMod.Sound");
inline constexpr auto BlastCommand = CommandNameToID("AtlasNetGMod.Blast");
inline constexpr auto GrenadeCommand = CommandNameToID("AtlasNetGMod.Grenade");
inline constexpr auto RespawnCommand = CommandNameToID("AtlasNetGMod.Respawn");
inline constexpr auto WeaponViewCommand = CommandNameToID("AtlasNetGMod.WeaponView");
inline constexpr auto RemovePropCommand = CommandNameToID("AtlasNetGMod.RemoveProp");
inline constexpr auto SpawnCommand = CommandNameToID("AtlasNetGMod.SpawnObject");
inline constexpr auto SpawnPropCommand = CommandNameToID("AtlasNetGMod.SpawnProp");
inline constexpr auto WorldLifecycle = CommandNameToID("AtlasNetGMod.WorldLifecycle");
inline constexpr auto WorldPosition = CommandNameToID("AtlasNetGMod.WorldPosition");
inline constexpr auto WorldSound = CommandNameToID("AtlasNetGMod.WorldSound");

struct SoundUpdate
{
  AtlasNet::AtlasNetClientID source;
  SoundPlayback playback;
  template<class Archive> void serialize(Archive& ar)
  {
    ar(source, playback.sound, playback.position, playback.endPosition, playback.explosion, playback.tracer);
  }
};

struct WorldUpdate
{
  std::uint64_t entity = 0;
  ViewAction action{};
  State state{};
  std::string shard;
  std::uint32_t region = 0;
  template<class Archive> void serialize(Archive& ar) { ar(entity, action, state, shard, region); }
};
}
