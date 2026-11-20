#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace AtlasNetGMod::Simulation
{
struct Vec
{
  float x = 0, y = 0, z = 0;
  template<class Archive> void serialize(Archive& ar) { ar(x, y, z); }
};

enum class PhysicsGun : std::uint8_t { None, Physgun, Gravity };
enum class PhysicsAction : std::uint8_t { None, Grab, Hold, Release, Freeze, Punt, Unfreeze };

struct PhysicsControl
{
  Vec point, direction;
  PhysicsGun tool = PhysicsGun::None;
  PhysicsAction action = PhysicsAction::None;
  template<class Archive> void serialize(Archive& ar) { ar(point, direction, tool, action); }
};

struct Input
{
  float forward = 0, side = 0, pitch = 0, yaw = 0;
  std::uint32_t buttons = 0;
  PhysicsGun tool = PhysicsGun::None;
  float toolRange = 0;
  bool guiding = false;
  Vec laser;
  template<class Archive> void serialize(Archive& ar)
  {
    ar(forward, side, pitch, yaw, buttons, tool, toolRange, guiding, laser);
  }
};

struct Bullet
{
  Vec direction;
  float distance = 0, damage = 0;
  std::uint32_t damageType = 0;
  template<class Archive> void serialize(Archive& ar) { ar(direction, distance, damage, damageType); }
};

struct Sound
{
  std::array<char, 128> name{};
  float volume = 1;
  std::int16_t channel = 0;
  std::uint16_t level = 75;
  std::uint8_t pitch = 100, dsp = 0;
  template<class Archive> void serialize(Archive& ar) { ar(name, volume, channel, level, pitch, dsp); }
};

struct SoundPlayback
{
  Sound sound;
  Vec position, endPosition;
  bool explosion = false, tracer = false;
};

struct Blast
{
  Vec position;
  float radius = 0, damage = 0;
  template<class Archive> void serialize(Archive& ar) { ar(position, radius, damage); }
};

enum class Kind : std::uint8_t { Player, Prop, Marker, Frag, ImpactGrenade, Rocket };

struct GrenadeLaunch
{
  Blast blast;
  Vec velocity;
  float fuse = 0;
  Kind kind = Kind::Frag;
  float pitch = 0, yaw = 0;
  template<class Archive> void serialize(Archive& ar) { ar(blast, velocity, fuse, kind, pitch, yaw); }
};

using ModelPath = std::array<char, 260>;
struct WeaponPresentation
{
  ModelPath model{}, playerModel{};
  std::uint8_t hold = 0;
  template<class Archive> void serialize(Archive& ar) { ar(model, playerModel, hold); }
};

struct PropPlacement
{
  ModelPath model{};
  Vec position;
  float pitch = 0, yaw = 0;
  template<class Archive> void serialize(Archive& ar) { ar(model, position, pitch, yaw); }
};

enum class Gait : std::uint8_t { Idle, Walk, Run, Air, CrouchIdle, CrouchWalk };
struct State
{
  Vec position, velocity, mins{-16, -16, 0}, maxs{16, 16, 72};
  float pitch = 0, yaw = 0, eye = 64, health = 100, armor = 0;
  float respawnDelay = 0, fuse = 0;
  Kind kind = Kind::Player;
  Gait gait = Gait::Idle;
  bool alive = true, motion = true;
  std::uint8_t hold = 0;
  std::uint32_t revision = 0;
  ModelPath model{}, weapon{};
  template<class Archive> void serialize(Archive& ar)
  {
    ar(position, velocity, mins, maxs, pitch, yaw, eye, health, armor, respawnDelay, fuse,
       kind, gait, alive, motion, hold, revision, model, weapon);
  }
};

enum class Action : std::uint8_t
{
  Ready, Spawn, Spawned, Resume, Freeze, Frozen, Remove, Sample, Removed,
  Input, Damage, Trace, Traced, Lost, Blast, Detonated, Respawn, PlayerModel, PhysicsTool, GuideRocket
};

// Same-machine worker/game-thread channel. AtlasNet packets are serialized
// separately through the SDK and never use this memory layout on the wire.
struct Message
{
  std::uint64_t entity = 0;
  std::uint64_t target = 0;
  Action action{};
  bool ok = true;
  State state{};
  Input input{};
  Bullet bullet{};
  Blast blast{};
  PhysicsControl physics{};
  float traceHull = 0;
};
static_assert(std::is_trivially_copyable_v<Message>);

enum class ViewAction : std::uint8_t { Own, Enter, Position, Exit, Damage, Presentation, Destroy };
struct View
{
  std::uint64_t entity = 0;
  ViewAction action{};
  State state{};
  char shard[37]{};
  std::uint32_t region = 0;
};
static_assert(std::is_trivially_copyable_v<View>);
}
