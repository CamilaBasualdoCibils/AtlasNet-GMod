#include "Endpoint/Chat.hpp"
#include "Gateway/Channel.hpp"
#include "Simulation/Gameplay.hpp"
#include "Simulation/Process.hpp"

#include <AtlasNet/Core/Network/RPC/RPCMethod.hpp>
#include <AtlasNet/Module/ModuleAPI.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <spdlog/spdlog.h>

namespace AtlasNetGMod
{
namespace
{
using namespace AtlasNet;
using namespace AtlasNet::Module;
using namespace Simulation;
using namespace std::chrono_literals;

struct TraceHit
{
  std::uint64_t target = 0;
  float distance = 0;
  bool hit = false;
  template<class Archive> void serialize(Archive& ar) { ar(target, distance, hit); }
};
using Trace = Network::RPC::RPCMethod<"AtlasNetGMod.Trace", TraceHit, std::uint64_t, float>;
using Damage = Network::RPC::RPCMethod<"AtlasNetGMod.Damage", void, std::uint64_t, float, std::uint32_t>;
using BlastDamage = Network::RPC::RPCMethod<"AtlasNetGMod.BlastDamage", void, std::uint64_t, Blast>;
using RemoveProp = Network::RPC::RPCMethod<"AtlasNetGMod.RemoveProp", void, std::uint64_t>;
using ControlProp = Network::RPC::RPCMethod<"AtlasNetGMod.ControlProp", void, std::uint64_t, PhysicsControl>;
using GuideRocket = Network::RPC::RPCMethod<"AtlasNetGMod.GuideRocket", void, std::uint64_t, Vec>;
using FinishRocket = Network::RPC::RPCMethod<"AtlasNetGMod.FinishRocket", void, std::uint64_t>;
constexpr auto engineTimeout = 750ms;
constexpr auto physicsLease = 500ms;
constexpr std::uint32_t primaryAttack = 1u, secondaryAttack = 1u << 11, reload = 1u << 13;
bool IsProjectile(Kind kind)
{
  return kind == Kind::Frag || kind == Kind::ImpactGrenade || kind == Kind::Rocket;
}

const char* ProjectileName(Kind kind)
{
  if (kind == Kind::Rocket) return "rocket";
  return kind == Kind::Frag ? "frag" : "impact grenade";
}

using Clock = std::chrono::steady_clock;
double Milliseconds(Clock::time_point since)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}
struct Shot
{
  std::future<SpatialRPCResult<TraceHit>> result;
  Clock::time_point started;
  AtlasNetEntityID shooter;
  Bullet bullet;
  bool removeProp = false;
  Vec origin;
  AtlasNetClientID client;
  PhysicsControl physics;
};

Spatial::Vector AimDirection(const Input& input)
{
  const double pitch = input.pitch * std::numbers::pi / 180;
  const double yaw = input.yaw * std::numbers::pi / 180;
  return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
}

struct Actor
{
  State state;
  Input input;
  AtlasNetClientID client;
  SpatialInterestHandle interest;
  std::uint64_t creator = 0;
  Blast projectileBlast;
  AtlasNetEntityID heldProp;
  AtlasNetEntityID activeRocket;
  std::uint64_t heldBy = 0;
  float holdDistance = 0;
  PhysicsGun holdingTool = PhysicsGun::None;
  Clock::time_point nextPhysicsUpdate, toolLease, nextGuidance;
  bool selecting = false;
  bool waitPhysgunRelease = false;
  bool receivedGuidance = false;
  Clock::time_point extracting;
  Clock::time_point nextPresentation;
  bool joined = false;
  bool updateCost = false;
  template<class Archive> void serialize(Archive& ar)
  {
    ar(state, input, client, interest, joined, creator, projectileBlast,
       heldProp, activeRocket, heldBy, holdDistance, holdingTool, waitPhysgunRelease);
  }
};

bool Finite(const Vec& point)
{
  return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

class GModShard final : public IAtlasNetShardAPI
{
public:
  void OnStart(ShardID id, Network::RPC::LocalShardRPC&) override
  {
    shard = id.to_string();
    const auto* launcher = std::getenv("ATLASNET_GMOD_SHARD_LAUNCHER");
    if (!launcher)
      throw std::runtime_error("GMod shard launcher is required");
    const auto map = game.Start(launcher, shard.c_str());
    mapBounds = {{map.mins.x, map.mins.y, map.mins.z}, {map.maxs.x, map.maxs.y, map.maxs.z}};
    viewRadius = std::hypot(map.maxs.x - map.mins.x, map.maxs.y - map.mins.y, map.maxs.z - map.mins.z);
    if (!Spatial::Valid(mapBounds) || !(viewRadius > 0 && std::isfinite(viewRadius)))
      throw std::runtime_error("GMod simulation reported invalid world bounds");
    spdlog::info("GMod map visibility bounds {} {} {} to {} {} {}", map.mins.x, map.mins.y,
                 map.mins.z, map.maxs.x, map.maxs.y, map.maxs.z);
    std::printf("[AtlasNet-GMod] Simulation ready shard=%s\n", shard.c_str());
    std::fflush(stdout);

    SetRegionChangeCallback([this](const auto& shape)
    {
      region = shape ? static_cast<std::uint32_t>(shape->id) : 0;
      std::printf("[AtlasNet-GMod] Shard region shard=%s region=%u\n", shard.c_str(), region);
      std::fflush(stdout);
    });
    RegisterHandlers();
    RegisterMigration();
  }

  void OnShutdown() override { game.Stop(); }
  void PollEvents() override {}

  void Poll() override
  {
    std::erase_if(pendingRemovals, [this](const auto& entry)
    {
      if (actors.contains(entry.first) && IsEntityMigrationPending(entry.first))
        return false;
      FinishNativeRemoval(entry.first, entry.second);
      return true;
    });
    for (int count = 0; count < 128; ++count)
    {
      auto message = game.Read();
      if (!message)
        break;
      const AtlasNetEntityID id(message->entity);
      auto actor = actors.find(id);
      if (actor == actors.end())
        continue;
      if (message->action == Action::Detonated || message->action == Action::Removed)
      {
        if (IsEntityMigrationPending(id))
          pendingRemovals.try_emplace(id, *message);
        else
          FinishNativeRemoval(id, *message);
        continue;
      }
      if (IsEntityMigrationPending(id))
        continue;
      if (message->action == Action::Sample && Finite(message->state.position))
      {
        auto& value = actor->second;
        const auto previous = value.state;
        ApplySample(value, message->state);
        UpdateEntityTransform(id, TransformOf(value.state));
        if (value.joined && value.updateCost)
        {
          value.updateCost = false;
          try { UpdateEntityCost(id, 4.0f); }
          catch (const std::exception& e) { spdlog::warn("Player entity cost: {}", e.what()); }
        }
        if (value.joined && value.client.value())
        {
          const bool changed = previous.health != value.state.health || previous.armor != value.state.armor ||
                               previous.alive != value.state.alive;
          SendView(value.client, id, changed ? ViewAction::Damage : ViewAction::Own, value.state, changed, shard);
          if (changed)
            spdlog::info("GMod vitals entity={} shard={} health={} armor={} alive={}", id.value(), shard,
                         value.state.health, value.state.armor, value.state.alive);
          if (changed || previous.gait != value.state.gait || Clock::now() >= value.nextPresentation)
            SendPresentation(id, value);
        }
        else if (value.state.model.front() &&
                 Clock::now() >= value.nextPresentation)
          SendPresentation(id, value);
      }
    }

    std::erase_if(shots, [this](auto& shot)
    {
      if (shot.result.wait_for(0ms) != std::future_status::ready)
        return false;
      ResolveShot(shot.result.get(), Milliseconds(shot.started), shot);
      return true;
    });
    if ((commandCount || signalCount) && Clock::now() >= nextStats)
    {
      const auto players = std::count_if(actors.begin(), actors.end(), [](const auto& item)
      { return item.second.client.value() != 0; });
      spdlog::info("GMod shard stats shard={} players={} entities={} commands={} signals={} handler_ms={:.3f}",
                   shard, players, actors.size(), commandCount, signalCount, commandMilliseconds);
      commandCount = 0;
      signalCount = 0;
      commandMilliseconds = 0;
      nextStats = Clock::now() + 5s;
    }
    if (!game.Alive())
      throw std::runtime_error("GMod simulation process disconnected");
  }

  bool IsEntityRegistered(AtlasNetEntityID entity) const override
  {
    return actors.contains(entity);
  }

  void UnregisterEntity(AtlasNetEntityID entity) override
  {
    if (const auto found = actors.find(entity); found != actors.end())
    {
      if (found->second.heldProp.value())
        ChangeHeldProp(entity, found->second, PhysicsAction::Release);
      const WorldUpdate update{entity.value(), ViewAction::Destroy, {}, shard, region};
      NetBinaryWriter writer;
      writer(update);
      DispatchSignal(CmdSig::Audience::Broadcast{}, WorldLifecycle, writer.GetBytes(), {}, entity);
      game.Send({.entity = entity.value(), .action = Action::Remove});
      actors.erase(found);
    }
  }

  void SetEntityMigrationCallbacks(EntityMigrationCallbacks callbacks) override
  {
    IAtlasNetShardAPI::SetEntityMigrationCallbacks(std::move(callbacks));
  }

  void RegisterCommandHandler(CmdSig::CommandID id, RawCommandCallback handler) override
  {
    handlers.insert_or_assign(id, std::move(handler));
  }

  std::optional<AtlasNetEntityID> OnClientSpawn(AtlasNetClientID client, AtlasNetEntityID entity,
      const ClientSpawnPlacement& placement, std::span<const std::byte>) override
  {
    Actor actor;
    actor.client = client;
    actor.state.position = {static_cast<float>(placement.x), static_cast<float>(placement.y),
                            static_cast<float>(placement.z)};
    actor.state.yaw = placement.x < 0 ? 0.0f : 180.0f;
    actor.input.yaw = actor.state.yaw;
    actor.updateCost = true;
    if (!Spawn(entity, actor))
      return {};
    if (!Resume(entity))
    {
      game.Send({.entity = entity.value(), .action = Action::Remove});
      return {};
    }
    actors.emplace(entity, std::move(actor));
    std::printf("[AtlasNet-GMod] Spawn client=%s entity=%llu shard=%s\n",
                client.to_string().c_str(), static_cast<unsigned long long>(entity.value()), shard.c_str());
    std::fflush(stdout);
    return entity;
  }

  void HandleCommands(std::span<const ShardCommand> commands) override
  {
    const auto started = Clock::now();
    for (const auto& command : commands)
    {
      const auto actor = actors.find(command.entity);
      if (actor == actors.end() || !actor->second.client.value())
        continue;
      NetBinaryReader reader(command.payload);
      CmdSig::CommandPackage package;
      reader(package);
      if (reader->adapter().error() != bitsery::ReaderError::NoError || reader.Remaining())
        continue;
      const auto handler = handlers.find(package.commandPayload.commandID);
      if (handler != handlers.end())
        handler->second(command.entity, actor->second.client,
                        std::as_bytes(std::span(package.commandPayload.payload)));
    }
    commandCount += commands.size();
    commandMilliseconds += Milliseconds(started);
  }

private:
  void FinishNativeRemoval(AtlasNetEntityID id, const Message& message)
  {
    const auto actor = actors.find(id);
    if (actor == actors.end() || actor->second.client.value())
      return;
    if (message.action == Action::Detonated && IsProjectile(actor->second.state.kind))
    {
      auto blast = actor->second.projectileBlast;
      blast.position = Finite(message.state.position) ? message.state.position : actor->second.state.position;
      RunBlast(AtlasNetEntityID(actor->second.creator), {}, blast);
      spdlog::info("GMod {} detonated entity={} shard={} x={:.2f}",
                   ProjectileName(actor->second.state.kind), id.value(), shard, blast.position.x);
    }
    if (actor->second.state.kind == Kind::Rocket)
    {
      const std::array launchers{AtlasNetEntityID(actor->second.creator)};
      CallEntity<FinishRocket>(launchers, {id.value()});
    }
    RemoveEntity(id);
  }

  static Transform TransformOf(const State& state)
  {
    const auto& p = state.position;
    return {.x = p.x, .y = p.y, .z = p.z};
  }

  static void ApplySample(Actor& actor, State state)
  {
    state.kind = actor.state.kind;
    if (state.kind == Kind::Player || !state.model.front())
      state.model = actor.state.model;
    state.weapon = actor.state.weapon;
    state.hold = actor.state.hold;
    state.revision = actor.state.revision + 1;
    actor.state = state;
  }

  void SendPresentation(AtlasNetEntityID entity, Actor& actor)
  {
    const auto& velocity = actor.state.velocity;
    const bool moving = velocity.x || velocity.y || velocity.z;
    actor.nextPresentation = Clock::now() +
        (actor.state.kind == Kind::Player || IsProjectile(actor.state.kind) || moving ? 45ms : 250ms);
    const WorldUpdate update{entity.value(), ViewAction::Presentation, actor.state, shard, region};
    NetBinaryWriter writer;
    writer(update);
    const GameplayOptions options{.delivery = GameplayDelivery::Unreliable,
                                  .ordering = GameplayOrdering::Unordered};
    const Spatial::Query audience{.shape = mapBounds, .dimension = 2};
    DispatchSignal(audience, WorldPosition, writer.GetBytes(), options, entity);
    ++signalCount;
  }

  bool Spawn(AtlasNetEntityID id, const Actor& actor)
  {
    if (game.Send({.entity = id.value(), .target = actor.creator, .action = Action::Spawn,
                   .state = actor.state, .input = actor.input, .blast = actor.projectileBlast}))
      if (auto reply = game.Wait(Action::Spawned, id.value(), engineTimeout); reply && reply->ok)
        return true;
    game.Send({.entity = id.value(), .action = Action::Remove});
    return false;
  }

  bool Resume(AtlasNetEntityID id)
  {
    return game.Send({.entity = id.value(), .action = Action::Resume});
  }

  void SendView(AtlasNetClientID client, AtlasNetEntityID entity, ViewAction action,
                const State& state, bool reliable, const std::string& owner)
  {
    const WorldUpdate update{entity.value(), action, state, owner, region};
    NetBinaryWriter writer;
    writer(update);
    const auto bytes = writer.GetBytes();
    GameplayOptions options;
    if (!reliable)
    {
      options.delivery = GameplayDelivery::Unreliable;
      options.ordering = GameplayOrdering::Unordered;
    }
    const ClientSignal signal{.recipient = client, .signal = reliable ? WorldLifecycle : WorldPosition,
        .payload = {bytes.begin(), bytes.end()}, .options = options, .entity = entity};
    DispatchRawSignals(std::span(&signal, 1));
    ++signalCount;
  }

  void BindInterest(AtlasNetEntityID entity)
  {
    OnSpatialInterest(actors.at(entity).interest, [this, entity](const SpatialInterestEvent& event)
    {
      const auto observer = actors.find(entity);
      if (observer == actors.end())
        return;
      State state;
      state.kind = event.isClient ? Kind::Player : Kind::Prop;
      if (event.position)
      {
        const auto& p = *event.position;
        state.position = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
      }
      const auto action = event.type == SpatialInterestEventType::Exit ? ViewAction::Exit :
                          event.type == SpatialInterestEventType::Enter ? ViewAction::Enter : ViewAction::Position;
      SendView(observer->second.client, event.entity, action, state, action != ViewAction::Position,
               event.shardId.to_string());
      if (action != ViewAction::Position)
        spdlog::info("GMod interest {} observer={} target={}", action == ViewAction::Enter ? "enter" : "exit",
                     entity.value(), event.entity.value());
    });
  }

  void ChangeHeldProp(AtlasNetEntityID entity, Actor& actor, PhysicsAction action)
  {
    const auto aim = AimDirection(actor.input);
    const auto& p = actor.state.position;
    const auto d = actor.holdDistance;
    const PhysicsControl control{
        {p.x + static_cast<float>(aim[0]) * d, p.y + static_cast<float>(aim[1]) * d,
         p.z + actor.state.eye + static_cast<float>(aim[2]) * d},
        {static_cast<float>(aim[0]), static_cast<float>(aim[1]), static_cast<float>(aim[2])},
        actor.holdingTool, action};
    const std::array targets{actor.heldProp};
    CallEntity<ControlProp>(targets, {entity.value(), control});
    if (action != PhysicsAction::Grab && action != PhysicsAction::Hold)
    {
      actor.heldProp = {};
      actor.holdingTool = PhysicsGun::None;
    }
  }

  void UpdatePhysicsInput(AtlasNetEntityID entity, Actor& actor, const Input& previous)
  {
    const auto& input = actor.input;
    const auto pressed = input.buttons & ~(previous.tool == input.tool ? previous.buttons : 0u);
    const auto now = Clock::now();
    if (actor.heldProp.value())
    {
      if (!actor.state.alive || actor.holdingTool != input.tool ||
          (input.tool == PhysicsGun::Physgun && !(input.buttons & primaryAttack)) ||
          (input.tool == PhysicsGun::Gravity && (pressed & secondaryAttack)))
        ChangeHeldProp(entity, actor, PhysicsAction::Release);
      else if (input.tool == PhysicsGun::Physgun && (pressed & secondaryAttack))
      {
        ChangeHeldProp(entity, actor, PhysicsAction::Freeze);
        actor.waitPhysgunRelease = true;
      }
      else if (input.tool == PhysicsGun::Gravity && (pressed & primaryAttack))
        ChangeHeldProp(entity, actor, PhysicsAction::Punt);
      else if (now >= actor.nextPhysicsUpdate)
      {
        ChangeHeldProp(entity, actor, PhysicsAction::Hold);
        actor.nextPhysicsUpdate = now + 50ms;
      }
      return;
    }
    if (!actor.state.alive || input.tool == PhysicsGun::None || actor.selecting ||
        now < actor.nextPhysicsUpdate || input.toolRange <= 0 || shots.size() >= 128)
      return;
    auto action = PhysicsAction::None;
    if (input.tool == PhysicsGun::Physgun)
    {
      if (!(input.buttons & primaryAttack)) actor.waitPhysgunRelease = false;
      if (pressed & reload) action = PhysicsAction::Unfreeze;
      else if (!actor.waitPhysgunRelease && (input.buttons & primaryAttack)) action = PhysicsAction::Grab;
    }
    else if (pressed & primaryAttack) action = PhysicsAction::Punt;
    else if (pressed & secondaryAttack) action = PhysicsAction::Grab;
    if (action == PhysicsAction::None) return;
    const auto& p = actor.state.position;
    const Spatial::Ray ray{{p.x, p.y, p.z + actor.state.eye}, AimDirection(input), input.toolRange, 32};
    const float hull = input.tool == PhysicsGun::Gravity ? 8.0f : 4.0f;
    shots.push_back({CallSpatial<Trace>(ray, {entity.value(), hull}), now, entity,
                     {}, false, {}, {}, {.tool = input.tool, .action = action}});
    actor.selecting = true;
    actor.nextPhysicsUpdate = now + 125ms;
  }

  void RegisterHandlers()
  {
    BindEntityRPC<GuideRocket>([this](const EntityFunctionContext& context, std::uint64_t launcher, const Vec& laser)
    {
      if (!Finite(laser)) return;
      for (auto id : context.localEntities)
      {
        const auto rocket = actors.find(id);
        if (rocket == actors.end() || rocket->second.state.kind != Kind::Rocket || rocket->second.creator != launcher)
          continue;
        if (!game.Send({.entity = id.value(), .action = Action::GuideRocket, .state = {.position = laser}}))
          throw std::runtime_error("Native rocket guidance queue is unavailable");
        if (!rocket->second.receivedGuidance)
        {
          rocket->second.receivedGuidance = true;
          spdlog::info("GMod rocket guidance received entity={} shard={}", id.value(), shard);
        }
      }
    });
    BindEntityRPC<FinishRocket>([this](const EntityFunctionContext& context, std::uint64_t rocket)
    {
      for (auto id : context.localEntities)
      {
        const auto launcher = actors.find(id);
        if (launcher != actors.end() && launcher->second.activeRocket.value() == rocket)
          launcher->second.activeRocket = {};
      }
    });
    BindEntityRPC<ControlProp>([this](const EntityFunctionContext& context, std::uint64_t source,
                                     const PhysicsControl& control)
    {
      if (!Finite(control.point) || !Finite(control.direction))
        return;
      for (auto entity : context.localEntities)
      {
        const auto found = actors.find(entity);
        if (found == actors.end() || found->second.state.kind != Kind::Prop)
          continue;
        auto& prop = found->second;
        const auto now = Clock::now();
        if (prop.heldBy && prop.toolLease < now) prop.heldBy = 0;
        if (prop.heldBy && prop.heldBy != source)
          continue;
        if ((control.action == PhysicsAction::Hold || control.action == PhysicsAction::Release ||
             control.action == PhysicsAction::Freeze) && prop.heldBy != source)
          continue;
        if (!game.Send({.entity = entity.value(), .target = source, .action = Action::PhysicsTool,
                        .physics = control}))
          throw std::runtime_error("Native physics control queue is unavailable");
        if (control.action == PhysicsAction::Grab || control.action == PhysicsAction::Hold)
        {
          prop.heldBy = source;
          prop.toolLease = now + physicsLease;
        }
        else prop.heldBy = 0;
        if (control.action != PhysicsAction::Hold)
          spdlog::info("GMod physics tool={} action={} entity={} player={} shard={}",
                       static_cast<unsigned>(control.tool), static_cast<unsigned>(control.action),
                       entity.value(), source, shard);
      }
    });
    RegisterCommandHandler(RemovePropCommand, [this](auto entity, auto, std::span<const std::byte> bytes)
    {
      const auto& actor = actors.at(entity);
      if (!bytes.empty() || !actor.state.alive || shots.size() >= 128)
        return;
      const auto& p = actor.state.position;
      const Spatial::Ray ray{{p.x, p.y, p.z + actor.state.eye}, AimDirection(actor.input), 4096, 0};
      shots.push_back({CallSpatial<Trace>(ray, {entity.value(), 0}), Clock::now(), entity,
                       {}, true});
    });
    BindEntityRPC<RemoveProp>([this](const EntityFunctionContext& context, std::uint64_t creator)
    {
      for (auto entity : context.localEntities)
      {
        const auto found = actors.find(entity);
        if (found == actors.end() || found->second.state.kind != Kind::Prop ||
            found->second.creator != creator)
          continue;
        RemoveEntity(entity);
        spdlog::info("GMod remover entity={} shard={}", entity.value(), shard);
      }
    });
    RegisterCommandHandler(RespawnCommand, [this](auto entity, auto, std::span<const std::byte> bytes)
    {
      if (bytes.empty() && !actors.at(entity).state.alive)
        game.Send({.entity = entity.value(), .action = Action::Respawn});
    });
    RegisterCommandHandler(WeaponViewCommand, [this](auto entity, auto, std::span<const std::byte> bytes)
    {
      WeaponPresentation weapon;
      NetBinaryReader reader(bytes);
      reader(weapon);
      if (reader.Remaining() || reader->adapter().error() != bitsery::ReaderError::NoError || weapon.model.back())
        return;
      auto& actor = actors.at(entity);
      if (!weapon.playerModel.front() || weapon.playerModel.back())
        return;
      if (actor.state.model != weapon.playerModel &&
          !game.Send({.entity = entity.value(), .action = Action::PlayerModel,
                      .state = {.model = weapon.playerModel}}))
        return;
      actor.state.model = weapon.playerModel;
      actor.state.weapon = weapon.model;
      actor.state.hold = weapon.hold;
      ++actor.state.revision;
      if (actor.joined)
        SendPresentation(entity, actor);
    });
    RegisterCommandHandler(SoundCommand, [this](auto entity, auto client, std::span<const std::byte> bytes)
    {
      Sound sound;
      NetBinaryReader reader(bytes);
      reader(sound);
      const auto& actor = actors.at(entity);
      if (!actor.joined || reader.Remaining() || reader->adapter().error() != bitsery::ReaderError::NoError ||
          !sound.name.front() || sound.name.back() || !(sound.volume > 0 && sound.volume <= 1))
        return;
      SendSound(entity, {client, {sound, actor.state.position}});
    });
    RegisterCommandHandler(BlastCommand, [this](auto entity, auto client, std::span<const std::byte> bytes)
    {
      Blast blast;
      NetBinaryReader reader(bytes);
      reader(blast);
      if (reader.Remaining() || reader->adapter().error() != bitsery::ReaderError::NoError ||
          !Finite(blast.position) || !(blast.radius > 0 && blast.radius <= 1024) ||
          !(blast.damage >= 0 && blast.damage <= 1500))
        return;
      RunBlast(entity, client, blast);
    });
    RegisterCommandHandler(GrenadeCommand, [this](auto entity, auto, std::span<const std::byte> bytes)
    {
      GrenadeLaunch launch;
      NetBinaryReader reader(bytes);
      reader(launch);
      const auto& p = launch.blast.position;
      const auto& v = launch.velocity;
      const auto& owner = actors.at(entity).state;
      if (reader.Remaining() || reader->adapter().error() != bitsery::ReaderError::NoError ||
          !owner.alive || !Finite(p) || !Finite(v) ||
          std::hypot(p.x - owner.position.x, p.y - owner.position.y, p.z - owner.position.z) > 192 ||
          !IsProjectile(launch.kind) || std::hypot(v.x, v.y, v.z) > 4000 ||
          !std::isfinite(launch.pitch) || !std::isfinite(launch.yaw) ||
          !(launch.fuse >= 0 && launch.fuse <= 10) ||
          (launch.kind == Kind::Frag && launch.fuse == 0) ||
          !(launch.blast.damage >= 0 && launch.blast.damage <= 1500) ||
          !(launch.blast.radius > 0 && launch.blast.radius <= 1024))
        return;
      Actor grenade;
      grenade.state.kind = launch.kind;
      grenade.state.position = p;
      grenade.state.velocity = v;
      grenade.state.fuse = launch.fuse;
      grenade.state.pitch = launch.pitch;
      grenade.state.yaw = launch.yaw;
      grenade.projectileBlast = launch.blast;
      grenade.creator = entity.value();
      const auto id = RegisterEntity(TransformOf(grenade.state), 1.0f);
      if (!Spawn(id, actors.emplace(id, grenade).first->second) || !Resume(id))
      {
        RemoveEntity(id);
        return;
      }
      if (launch.kind == Kind::Rocket)
        actors.at(entity).activeRocket = id;
      spdlog::info("GMod {} spawn entity={} shard={} fuse={:.3f} x={:.2f}",
                   ProjectileName(launch.kind), id.value(), shard, launch.fuse, p.x);
    });
    RegisterCommandHandler(SpawnPropCommand, [this](auto entity, auto, std::span<const std::byte> bytes)
    {
      PropPlacement placement;
      NetBinaryReader reader(bytes);
      reader(placement);
      if (reader.Remaining() || reader->adapter().error() != bitsery::ReaderError::NoError ||
          !placement.model.front() || placement.model.back() || !Finite(placement.position) ||
          !std::isfinite(placement.pitch) || !std::isfinite(placement.yaw))
        return;
      Actor prop;
      prop.creator = entity.value();
      prop.state.kind = Kind::Prop;
      prop.state.model = placement.model;
      prop.state.position = placement.position;
      prop.state.pitch = placement.pitch;
      prop.state.yaw = placement.yaw;
      const auto id = RegisterEntity(TransformOf(prop.state), 1.5f);
      if (!Spawn(id, actors.emplace(id, prop).first->second) || !Resume(id))
      {
        RemoveEntity(id);
        return;
      }
      spdlog::info("GMod prop spawn entity={} shard={} model={}", id.value(), shard, placement.model.data());
    });
    BindEntityRPC<BlastDamage>([this](const EntityFunctionContext& context, std::uint64_t source, const Blast& blast)
    {
      for (auto entity : context.localEntities)
      {
        if (!actors.contains(entity))
          continue;
        if (!game.Send({.entity = entity.value(), .target = source, .action = Action::Blast, .blast = blast}))
          throw std::runtime_error("Native blast queue is unavailable");
        spdlog::info("GMod blast dispatched entity={} shard={}", entity.value(), shard);
      }
    });
    RegisterCommandHandler(ChatCommand,
        [this](auto entity, auto client, std::span<const std::byte> bytes)
        {
          if (bytes.empty() || bytes.size() > Gateway::MaxText)
            return;
          const ClientSignal response{.recipient = client, .signal = ChatSignal,
              .payload = {bytes.begin(), bytes.end()}, .entity = entity};
          DispatchRawSignals(std::span(&response, 1));
          ++signalCount;
        });
    RegisterCommandHandler(MoveCommand, [this](auto entity, auto, std::span<const std::byte> bytes)
    {
      Input input;
      NetBinaryReader reader(bytes);
      reader(input);
      if (reader.Remaining() || reader->adapter().error() != bitsery::ReaderError::NoError ||
          !std::isfinite(input.forward) || !std::isfinite(input.side) ||
          !std::isfinite(input.pitch) || !std::isfinite(input.yaw) ||
          !std::isfinite(input.toolRange) || input.tool > PhysicsGun::Gravity ||
          (input.guiding && !Finite(input.laser)))
        return;
      auto& actor = actors.at(entity);
      if (!actor.joined)
      {
        actor.joined = true;
        actor.interest = CreateSpatialInterest(entity, Spatial::Sphere{.radius = viewRadius},
            SpatialInterestSettings{.updateInterval = 100ms});
        BindInterest(entity);
        SendView(actor.client, entity, ViewAction::Own, actor.state, true, shard);
        return;
      }
      input.forward = std::clamp(input.forward, -450.0f, 450.0f);
      input.side = std::clamp(input.side, -450.0f, 450.0f);
      input.pitch = std::clamp(input.pitch, -89.0f, 89.0f);
      input.toolRange = std::clamp(input.toolRange, 0.0f, 32768.0f);
      const auto previous = actor.input;
      actor.input = input;
      UpdatePhysicsInput(entity, actor, previous);
      if (actor.state.alive && input.guiding && actor.activeRocket.value() && Clock::now() >= actor.nextGuidance)
      {
        const std::array targets{actor.activeRocket};
        CallEntity<GuideRocket>(targets, {entity.value(), input.laser});
        actor.nextGuidance = Clock::now() + 50ms;
      }
      game.Send({.entity = entity.value(), .action = Action::Input, .input = input});
    });
    RegisterCommandHandler(SpawnCommand, [this](auto entity, auto, std::span<const std::byte> bytes)
    {
      if (bytes.size() != 1)
        return;
      const auto kind = static_cast<Kind>(std::to_integer<std::uint8_t>(bytes.front()));
      if (kind != Kind::Prop && kind != Kind::Marker)
        return;
      Actor object;
      object.state.kind = kind;
      const auto& player = actors.at(entity);
      object.state.position = player.state.position;
      const auto direction = AimDirection(player.input);
      object.state.position.x += static_cast<float>(direction[0] * 80);
      object.state.position.y += static_cast<float>(direction[1] * 80);
      object.state.position.z += kind == Kind::Marker ?
          player.state.eye + static_cast<float>(direction[2] * 80) : 20;
      const auto id = RegisterEntity(TransformOf(object.state), kind == Kind::Prop ? 1.5f : 0.5f);
      actors.emplace(id, object);
      if (!Spawn(id, object) || !Resume(id))
        RemoveEntity(id);
      else
        spdlog::info("GMod object spawn entity={} shard={} kind={}", id.value(), shard, static_cast<int>(kind));
    });
    RegisterCommandHandler(FireCommand, [this](auto entity, auto, std::span<const std::byte> bytes)
    {
      const auto& actor = actors.at(entity);
      Bullet bullet;
      NetBinaryReader reader(bytes);
      reader(bullet);
      if (reader.Remaining() || reader->adapter().error() != bitsery::ReaderError::NoError ||
          !actor.state.alive || shots.size() >= 128 || !Finite(bullet.direction) ||
          !(bullet.distance > 0 && bullet.distance <= 65536) || !(bullet.damage > 0 && bullet.damage <= 10000))
        return;
      const auto& d = bullet.direction;
      const double length = std::hypot(d.x, d.y, d.z);
      if (length < 0.99 || length > 1.01)
        return;
      const auto& p = actor.state.position;
      const Spatial::Ray ray{{p.x, p.y, p.z + actor.state.eye},
                             {d.x / length, d.y / length, d.z / length}, bullet.distance, 32};
      const auto started = Clock::now();
      shots.push_back({CallSpatial<Trace>(ray, {entity.value(), 0}), started, entity, bullet, false,
                       {p.x, p.y, p.z + actor.state.eye}, actor.client});
    });
    BindSpatialRPC<Trace>([this](const SpatialRPCContext& context, std::uint64_t shooter, float hull) -> TraceHit
    {
      const auto& ray = std::get<Spatial::Ray>(context.query.shape);
      State state;
      state.position = {static_cast<float>(ray.origin[0]), static_cast<float>(ray.origin[1]),
                        static_cast<float>(ray.origin[2])};
      const Vec direction{static_cast<float>(ray.direction[0]), static_cast<float>(ray.direction[1]),
                          static_cast<float>(ray.direction[2])};
      const auto started = Clock::now();
      if (!game.Send({.entity = shooter, .action = Action::Trace, .state = state,
                      .bullet = {.direction = direction, .distance = static_cast<float>(ray.distance)},
                      .traceHull = std::clamp(hull, 0.0f, 8.0f)}))
        throw std::runtime_error("Native trace queue is unavailable");
      const auto answer = game.Wait(Action::Traced, shooter, engineTimeout);
      if (!answer)
        throw std::runtime_error("Native trace timed out");
      spdlog::info("GMod trace shard={} shooter={} hit={} target={} distance={} native_ms={:.3f}",
                   shard, shooter, answer->ok, answer->target, answer->bullet.distance, Milliseconds(started));
      return {answer->target, answer->bullet.distance, answer->ok};
    });
    BindEntityRPC<Damage>([this](const EntityFunctionContext& context, std::uint64_t shooter,
                                 float amount, std::uint32_t damageType)
    {
      for (const auto entity : context.localEntities)
      {
        const auto found = actors.find(entity);
        if (found == actors.end())
          continue;
        if (!game.Send({.entity = entity.value(), .target = shooter, .action = Action::Damage,
                        .bullet = {.damage = amount, .damageType = damageType}}))
          throw std::runtime_error("Native damage queue is unavailable");
      }
    });
  }

  void SendSound(AtlasNetEntityID entity, const SoundUpdate& update)
  {
    NetBinaryWriter writer;
    writer(update);
    const auto& p = update.playback.position;
    const Spatial::Query audience{.shape = update.playback.tracer ? Spatial::Volume(mapBounds) :
        Spatial::Volume(Spatial::Sphere{{p.x, p.y, p.z}, 1280}), .dimension = 2};
    const GameplayOptions options{.delivery = update.playback.explosion ?
        GameplayDelivery::Reliable : GameplayDelivery::Unreliable,
        .ordering = GameplayOrdering::Unordered};
    DispatchSignal(audience, WorldSound, writer.GetBytes(), options, entity);
    ++signalCount;
  }

  void RunBlast(AtlasNetEntityID attacker, AtlasNetClientID nativeGateway, const Blast& blast)
  {
    const auto& p = blast.position;
    const Spatial::Query audience{.shape = Spatial::Sphere{{p.x, p.y, p.z}, blast.radius + 48.0}, .dimension = 2};
    if (blast.damage > 0)
      CallEntity<BlastDamage>(audience, {attacker.value(), blast});
    SendSound(attacker, {nativeGateway, {.position = p, .explosion = true}});
    spdlog::info("GMod blast source={} shard={} radius={} damage={}", attacker.value(), shard,
                 blast.radius, blast.damage);
  }

  void ResolveShot(const SpatialRPCResult<TraceHit>& result, double duration, const Shot& shot)
  {
    const auto shooter = shot.shooter;
    auto player = actors.find(shooter);
    if (shot.physics.action != PhysicsAction::None && player != actors.end())
      player->second.selecting = false;
    if (!result.failures.empty())
    {
      for (const auto& failure : result.failures)
        spdlog::warn("GMod ray failed shard={} shooter={} assignment={} error={} elapsed_ms={:.3f}",
            shard, shooter.value(), failure.shard.to_string(), static_cast<int>(failure.error), duration);
      return;
    }
    const TraceHit* nearest = nullptr;
    for (const auto& response : result.responses)
    {
      const auto& hit = response.value;
      if (!hit.hit)
        continue;
      if (!nearest || hit.distance < nearest->distance ||
          (hit.distance == nearest->distance && hit.target < nearest->target))
        nearest = &hit;
    }
    spdlog::info("GMod ray complete shard={} results={} nearest={} elapsed_ms={:.3f} shooter={}", shard,
                 result.responses.size(), nearest ? nearest->target : 0, duration, shooter.value());
    if (shot.physics.action != PhysicsAction::None)
    {
      if (player == actors.end() || !nearest || !nearest->target || IsEntityMigrationPending(shooter))
        return;
      auto& actor = player->second;
      if (!actor.state.alive || actor.input.tool != shot.physics.tool ||
          (shot.physics.tool == PhysicsGun::Physgun && shot.physics.action == PhysicsAction::Grab &&
           !(actor.input.buttons & primaryAttack)))
        return;
      actor.heldProp = AtlasNetEntityID(nearest->target);
      actor.holdingTool = shot.physics.tool;
      actor.holdDistance = shot.physics.tool == PhysicsGun::Gravity
          ? std::min(80.0f, nearest->distance) : nearest->distance;
      ChangeHeldProp(shooter, actor, shot.physics.action);
      return;
    }
    if (!shot.removeProp)
    {
      const auto& start = shot.origin;
      const auto& d = shot.bullet.direction;
      const auto distance = nearest ? nearest->distance : shot.bullet.distance;
      SendSound(shooter, {shot.client, {.position = start,
          .endPosition = {start.x + d.x * distance, start.y + d.y * distance, start.z + d.z * distance},
          .tracer = true}});
    }
    if (nearest && nearest->target)
    {
      const std::array targets{AtlasNetEntityID(nearest->target)};
      if (shot.removeProp)
        CallEntity<RemoveProp>(targets, {shooter.value()});
      else
        CallEntity<Damage>(targets, {shooter.value(), shot.bullet.damage, shot.bullet.damageType});
    }
  }

  void RegisterMigration()
  {
    SetEntityMigrationCallbacks({
        .prepareExtract = [this](auto id, auto, auto& payload)
        {
          const auto found = actors.find(id);
          if (found == actors.end())
            return false;
          found->second.extracting = Clock::now();
          if (!game.Send({.entity = id.value(), .action = Action::Freeze}))
            return false;
          const auto sample = game.Wait(Action::Frozen, id.value(), engineTimeout);
          if (!sample || !sample->ok)
            return false;
          ApplySample(found->second, sample->state);
          NetBinaryWriter writer;
          writer(found->second);
          const auto bytes = writer.GetBytes();
          payload.assign(bytes.begin(), bytes.end());
          return true;
        },
        .prepareRestore = [this](auto id, auto transfer, auto bytes)
        {
          if (actors.contains(id))
            return false;
          Actor actor;
          NetBinaryReader reader(bytes);
          reader(actor);
          if (reader.Remaining() || reader->adapter().error() != bitsery::ReaderError::NoError ||
              !Finite(actor.state.position) || !Spawn(id, actor))
            return false;
          staged.insert_or_assign(transfer, std::move(actor));
          return true;
        },
        .commitExtract = [this](auto id, auto transfer)
        {
          if (!game.Send({.entity = id.value(), .action = Action::Remove}))
            return false;
          if (const auto found = actors.find(id); found != actors.end())
          {
            spdlog::info("GMod transfer extract entity={} shard={} transfer={} source_ms={:.3f}",
                         id.value(), shard, transfer.to_string(), Milliseconds(found->second.extracting));
            actors.erase(found);
          }
          return true;
        },
        .commitRestore = [this](auto id, auto transfer)
        {
          const auto found = staged.find(transfer);
          if (found == staged.end())
            return actors.contains(id);
          if (!Resume(id))
            return false;
          auto& actor = actors.emplace(id, std::move(found->second)).first->second;
          if (actor.heldBy) actor.toolLease = Clock::now() + physicsLease;
          staged.erase(found);
          if (actor.client.value() && actor.joined)
          {
            BindInterest(id);
            SendView(actor.client, id, ViewAction::Own, actor.state, true, shard);
          }
          spdlog::info("GMod transfer restore entity={} shard={} transfer={}", id.value(), shard,
                       transfer.to_string());
          return true;
        },
        .abortExtract = [this](auto id, auto transfer)
        {
          spdlog::warn("GMod transfer extract aborted entity={} shard={} transfer={}", id.value(),
                       shard, transfer.to_string());
          Resume(id);
        },
        .abortRestore = [this](auto id, auto transfer)
        {
          spdlog::warn("GMod transfer restore aborted entity={} shard={} transfer={}", id.value(),
                       shard, transfer.to_string());
          game.Send({.entity = id.value(), .action = Action::Remove});
          staged.erase(transfer);
        }});
  }

  Process game;
  Spatial::Box mapBounds;
  double viewRadius = 0;
  std::string shard;
  std::uint32_t region = 0;
  std::unordered_map<AtlasNetEntityID, Actor> actors;
  std::unordered_map<AtlasNetEntityID, Message> pendingRemovals;
  std::unordered_map<EntityTransferID, Actor> staged;
  std::unordered_map<CmdSig::CommandID, RawCommandCallback> handlers;
  std::vector<Shot> shots;
  Clock::time_point nextStats{};
  std::size_t commandCount = 0;
  std::size_t signalCount = 0;
  double commandMilliseconds = 0;
};
}

std::unique_ptr<AtlasNet::Module::IAtlasNetShardAPI> MakeGModShard()
{
  return std::make_unique<GModShard>();
}
}
