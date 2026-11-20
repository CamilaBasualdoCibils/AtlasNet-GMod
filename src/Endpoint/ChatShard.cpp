#include "Endpoint/Chat.hpp"
#include "Gateway/Channel.hpp"

#include <AtlasNet/Module/ModuleAPI.hpp>

#include <cstdio>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace AtlasNetGMod
{
std::unique_ptr<AtlasNet::Module::IAtlasNetShardAPI> MakeGModShard();
namespace
{
using namespace AtlasNet;
using namespace AtlasNet::Module;

class DemoShard final : public IAtlasNetShardAPI
{
public:
  void OnStart(ShardID, Network::RPC::LocalShardRPC&) override
  {
    RegisterCommandHandler(ChatCommand,
        [this](AtlasNetEntityID entity, AtlasNetClientID client,
               std::span<const std::byte> chat)
        {
          if (chat.empty() || chat.size() > Gateway::MaxText)
            return;
          std::printf("[AtlasNet-GMod shard] Chat Command client=%s bytes=%zu\n",
                      client.to_string().c_str(), chat.size());
          std::fflush(stdout);
          const ClientSignal signal{.recipient = client, .signal = ChatSignal,
              .payload = {chat.begin(), chat.end()}, .entity = entity};
          DispatchRawSignals(std::span(&signal, 1));
        });
  }
  void OnShutdown() override {}
  void Poll() override {}
  void PollEvents() override {}
  void UnregisterEntity(AtlasNetEntityID entity) override { clients.erase(entity); }
  bool IsEntityRegistered(AtlasNetEntityID entity) const override { return clients.contains(entity); }
  void SetEntityMigrationCallbacks(EntityMigrationCallbacks callbacks) override
  {
    IAtlasNetShardAPI::SetEntityMigrationCallbacks(std::move(callbacks));
  }
  void RegisterCommandHandler(CmdSig::CommandID id, RawCommandCallback handler) override
  {
    handlers.insert_or_assign(id, std::move(handler));
  }

  std::optional<AtlasNetEntityID> OnClientSpawn(AtlasNetClientID client,
      AtlasNetEntityID entity, const ClientSpawnPlacement&, std::span<const std::byte>) override
  {
    clients.emplace(entity, client);
    return entity;
  }

  void HandleCommands(std::span<const ShardCommand> commands) override
  {
    for (const auto& command : commands)
    {
      const auto client = clients.find(command.entity);
      if (client == clients.end())
        continue;
      NetBinaryReader reader(command.payload);
      CmdSig::CommandPackage package;
      reader(package);
      if (reader->adapter().error() != bitsery::ReaderError::NoError || reader.Remaining() != 0)
        continue;
      const auto handler = handlers.find(package.commandPayload.commandID);
      if (handler != handlers.end())
        handler->second(command.entity, client->second,
                         std::as_bytes(std::span(package.commandPayload.payload)));
    }
  }

private:
  std::unordered_map<AtlasNetEntityID, AtlasNetClientID> clients;
  std::unordered_map<CmdSig::CommandID, RawCommandCallback> handlers;
};

class DemoProvider final : public ShardProvider
{
  std::unique_ptr<IAtlasNetShardAPI> CreateShardLogic() override
  {
    if (std::getenv("ATLASNET_GMOD_SHARD_LAUNCHER"))
      return MakeGModShard();
    return std::make_unique<DemoShard>();
  }
};

class DemoSpawn final : public IClientSpawnResolver
{
  ClientSpawnPlacement Resolve(AtlasNetClientID client, const AdmissionData&) override
  {
    std::printf("[AtlasNet-GMod] Demo spawn resolver client=%s\n", client.to_string().c_str());
    std::fflush(stdout);
    return {.worldName = "GModDemo"};
  }
};

class DistributedSpawn final : public IClientSpawnResolver
{
public:
  explicit DistributedSpawn(const char* setting = nullptr) : setting(setting) {}

private:
  ClientSpawnPlacement Resolve(AtlasNetClientID, const AdmissionData&) override
  {
    const auto* key = setting ? setting : (next++ % 2 == 0 ? West : East);
    const auto* text = std::getenv(key);
    char* end = nullptr;
    const double x = text ? std::strtod(text, &end) : 0;
    if (text && (end == text || *end || !std::isfinite(x)))
      throw std::invalid_argument(key);
    return {.worldName = "GModDistributed", .x = x};
  }
public:
  static constexpr auto West = "ATLASNET_GMOD_SPAWN_WEST_X";
  static constexpr auto East = "ATLASNET_GMOD_SPAWN_EAST_X";

private:
  const char* setting;
  std::atomic<unsigned> next{0};
};

// Used only to exercise rejection through the AtlasNet admission pipeline.
class RejectDemo final : public IClientAdmissionHandler
{
  ClientAdmissionResult Evaluate(const Network::Client::V1::ClientPacketHeader&,
                                  std::span<const std::byte>) override
  {
    return {.accepted = false, .rejectionReason = "AtlasNet demo admission rejected"};
  }
};
}

void RegisterDemo(AtlasNet::Module::ModuleRegistry& registry)
{
  registry.RegisterShardProvider(std::make_shared<DemoProvider>());
  registry.RegisterClientSpawnResolver("GModDemo", std::make_shared<DemoSpawn>());
  registry.RegisterClientSpawnResolver("GModDistributed", std::make_shared<DistributedSpawn>());
  registry.RegisterClientSpawnResolver("GModWest", std::make_shared<DistributedSpawn>(DistributedSpawn::West));
  registry.RegisterClientSpawnResolver("GModEast", std::make_shared<DistributedSpawn>(DistributedSpawn::East));
  registry.RegisterClientAdmissionHandler("GModDemoReject", std::make_shared<RejectDemo>());
}
} // namespace AtlasNetGMod
