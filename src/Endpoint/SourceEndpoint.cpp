#include "Endpoint/Chat.hpp"
#include "Gateway/Channel.hpp"
#include "Simulation/Gameplay.hpp"

#include <AtlasNet/Module/ModuleAPI.hpp>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace AtlasNetGMod
{
void RegisterDemo(AtlasNet::Module::ModuleRegistry& registry);

namespace
{
using namespace AtlasNet;
using namespace AtlasNet::Module;
using namespace AtlasNet::Network::Client;
using namespace Gateway;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

sockaddr_in Address(const std::string& ip, int port)
{
  sockaddr_in address{.sin_family = AF_INET};
  if (port < 0 || port > 65535 || inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1 ||
      (ntohl(address.sin_addr.s_addr) >> 24) != 127)
    throw std::invalid_argument("source1 requires a loopback IPv4 address and valid port");
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  return address;
}

std::string ToString(const sockaddr_in& address)
{
  char ip[INET_ADDRSTRLEN]{};
  inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(address.sin_port));
}

bool SameUser(const Socket& socket)
{
  ucred cred{};
  socklen_t size = sizeof(cred);
  return getsockopt(socket.Get(), SOL_SOCKET, SO_PEERCRED, &cred, &size) == 0 &&
         cred.uid == geteuid();
}

Socket UdpSocket(const sockaddr_in& bindAddress)
{
  Socket result(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  Check(result.Get(), "Source UDP socket");
  Check(bind(result.Get(), reinterpret_cast<const sockaddr*>(&bindAddress),
             sizeof(bindAddress)), "Source UDP bind");
  return result;
}

enum class Phase { Negotiating, AdmissionPending, Admitted, Established, Closing };

struct Peer
{
  sockaddr_in remote;
  Socket backend;
  std::uint16_t port;
  std::uint32_t token;
  Phase phase = Phase::Negotiating;
  Clock::time_point deadline;
  std::uint32_t request = 0;
  std::uint64_t commandSequence = 0;
  ClientAdmissionTicket ticket;
  std::optional<AtlasNetClientID> client;
};

class SourceEndpoint final : public IClientEndpoint
{
public:
  explicit SourceEndpoint(const ClientEndpointConfig& config)
      : listenAddress(Address(config.args.value("address", "127.0.0.1"),
                             config.args.value("port", 27015))),
        backendAddress(Address(config.args.value("backend_address", "127.0.0.1"),
                               config.args.value("backend_port", 27016))),
        backendBind(Address("127.0.0.2", 0)),
        socketPath(std::filesystem::absolute(config.args.at("gateway_socket").get<std::string>()).string()),
        admission(config.args.value("admission", "AllowAll")),
        spawn(config.args.value("spawn", "GModDemo")),
        idleTimeout(config.args.value("idle_seconds", 60)),
        simulation(config.args.value("simulation", false))
  {
    UnixAddress(socketPath);
    if (listenAddress.sin_port == 0 || backendAddress.sin_port == 0 ||
        idleTimeout <= 0s ||
        (listenAddress.sin_port == backendAddress.sin_port &&
         listenAddress.sin_addr.s_addr == backendAddress.sin_addr.s_addr))
      throw std::invalid_argument("source1 needs distinct sockets, nonzero ports and a positive idle timeout");
  }

  ~SourceEndpoint() override { Stop(); }

  void Start() override
  {
    publicUdp = UdpSocket(listenAddress);
    listener.Reset(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    Check(listener.Get(), "Gateway socket");
    const auto unixAddress = UnixAddress(socketPath);
    Check(bind(listener.Get(), reinterpret_cast<const sockaddr*>(&unixAddress),
               sizeof(unixAddress)), "Gateway bind");
    ownsSocketPath = true;
    Check(chmod(socketPath.c_str(), S_IRUSR | S_IWUSR), "Gateway chmod");
    Check(listen(listener.Get(), 1), "Gateway listen");
    worker = std::jthread([this](std::stop_token stop) { Run(stop); });
    std::printf("[AtlasNet-GMod endpoint] Listening %s -> %s\n",
                ToString(listenAddress).c_str(), ToString(backendAddress).c_str());
    std::fflush(stdout);
  }

  void Stop() noexcept override
  {
    if (worker.joinable())
    {
      {
        std::scoped_lock lock(mutex);
        for (auto& [key, peer] : peers)
          if (peer.client)
            StartClosing(peer, "AtlasNet endpoint is shutting down", false);
      }
      worker.request_stop();
      worker.join();
    }
    listener.Reset();
    publicUdp.Reset();
    if (std::exchange(ownsSocketPath, false))
      unlink(socketPath.c_str());
  }

  void CloseClient(AtlasNetClientID client) noexcept override
  {
    std::scoped_lock lock(mutex);
    if (auto* peer = FindClient(client))
      StartClosing(*peer, "AtlasNet closed the session", false);
  }

  ClientDispatchResult DispatchClientPacket(AtlasNetClientID client,
      const ClientPacketView& packet, GameplayDelivery delivery) override
  {
    if (packet.header.instruction != ClientInstruction::Signal || packet.header.channelID != 1)
      return ClientDispatchResult::UnsupportedDelivery;

    NetBinaryReader reader(packet.payload);
    V1::SignalPacketHeader header{};
    reader(header.signalID, header.instanceID, header.payloadSize);
    if (reader->adapter().error() != bitsery::ReaderError::NoError ||
        header.payloadSize != reader.Remaining())
      return ClientDispatchResult::Failed;
    const bool world = simulation && (header.signalID == Simulation::WorldLifecycle ||
                                      header.signalID == Simulation::WorldPosition);
    const bool sound = simulation && header.signalID == Simulation::WorldSound;
    if (!world && !sound && header.signalID != ChatSignal)
      return ClientDispatchResult::UnsupportedDelivery;
    if (header.payloadSize == 0 || header.payloadSize > (world || sound ? MaxFramePayload : MaxText))
      return ClientDispatchResult::Failed;

    std::scoped_lock lock(mutex);
    auto* peer = FindClient(client);
    if (!peer || !gateway)
      return ClientDispatchResult::Failed;
    if (peer->phase != Phase::Established)
      return ClientDispatchResult::Backpressure;

    const auto text = packet.payload.subspan(reader.Position());
    Frame frame{.event = Event::Signal, .port = peer->port, .token = peer->token,
                .value = header.instanceID};
    if (world)
    {
      static_assert(sizeof(Simulation::View) <= MaxFramePayload);
      NetBinaryReader body(text);
      Simulation::WorldUpdate update;
      body(update);
      if (body->adapter().error() != bitsery::ReaderError::NoError || body.Remaining() ||
          update.shard.size() != sizeof(Simulation::View::shard) - 1 ||
          update.state.model.back() || update.state.weapon.back())
        return ClientDispatchResult::Failed;
      Simulation::View view{.entity = update.entity, .action = update.action,
                            .state = update.state, .region = update.region};
      std::copy(update.shard.begin(), update.shard.end(), view.shard);
      frame.event = delivery == GameplayDelivery::Reliable ? Event::WorldReliable : Event::World;
      frame.SetText({reinterpret_cast<const char*>(&view), sizeof(view)});
    }
    else if (sound)
    {
      static_assert(sizeof(Simulation::SoundPlayback) <= MaxFramePayload);
      NetBinaryReader body(text);
      Simulation::SoundUpdate update;
      body(update);
      if (body->adapter().error() != bitsery::ReaderError::NoError || body.Remaining() ||
          update.playback.sound.name.back() != '\0')
        return ClientDispatchResult::Failed;
      if (FindClient(update.source))
        return ClientDispatchResult::Sent; // The native server already emitted this sound here.
      frame.event = Event::Sound;
      frame.SetText({reinterpret_cast<const char*>(&update.playback), sizeof(update.playback)});
    }
    else frame.SetText({reinterpret_cast<const char*>(text.data()), text.size()});
    switch (Send(gateway, frame))
    {
    case IO::Ok: return ClientDispatchResult::Sent;
    case IO::Busy: return ClientDispatchResult::Backpressure;
    case IO::Closed: return ClientDispatchResult::Failed;
    }
    return ClientDispatchResult::Failed;
  }

private:
  struct PollPeer
  {
    std::uint64_t key;
    std::uint32_t token;
  };

  void Run(std::stop_token stop) noexcept
  {
    try
    {
      std::vector<pollfd> fds;
      std::vector<PollPeer> polled;
      fds.reserve(67);
      polled.reserve(64);
      std::optional<Clock::time_point> drain;
      for (;;)
      {
        {
          std::scoped_lock lock(mutex);
          if (stop.stop_requested())
          {
            if (!drain)
              drain = Clock::now() + 250ms;
            if (!gateway || Clock::now() >= *drain ||
                std::none_of(peers.begin(), peers.end(), [](const auto& item)
                { return item.second.phase == Phase::Closing; }))
              break;
          }
          // Let the real engine send its disconnect packet on existing
          // backend sockets without accepting new peers during shutdown.
          fds = {{drain ? -1 : publicUdp.Get(), POLLIN, 0},
                 {drain ? -1 : listener.Get(), POLLIN, 0},
                 {gateway.Get(), POLLIN, 0}};
          polled.clear();
          for (const auto& [key, peer] : peers)
          {
            fds.push_back({peer.backend.Get(), POLLIN, 0});
            polled.push_back({key, peer.token});
          }
        }
        const int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), 50);
        if (ready < 0 && errno == EINTR)
          continue;
        Check(ready, "Source endpoint poll");
        std::scoped_lock lock(mutex);
        if (fds[2].revents && gateway.Get() == fds[2].fd)
          ReadGateway();
        if (fds[1].revents & POLLIN)
          AcceptGateway();
        if (fds[0].revents & POLLIN)
          ReadClient();
        for (std::size_t index = 0; index < polled.size(); ++index)
        {
          if (!fds[index + 3].revents)
            continue;
          auto found = peers.find(polled[index].key);
          if (found != peers.end() && found->second.token == polled[index].token)
            ReadBackend(found->second);
        }
        ExpirePeers();
      }
    }
    catch (const std::exception& error)
    {
      std::fprintf(stderr, "[AtlasNet-GMod endpoint] %s\n", error.what());
    }
    std::scoped_lock lock(mutex);
    LoseGateway();
  }

  void AcceptGateway()
  {
    Socket candidate(accept4(listener.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    if (candidate && !gateway && SameUser(candidate))
    {
      gateway = std::move(candidate);
      std::puts("[AtlasNet-GMod endpoint] Dedicated server gateway attached");
      std::fflush(stdout);
    }
  }

  void LoseGateway()
  {
    if (gateway)
    {
      std::puts("[AtlasNet-GMod endpoint] Dedicated server gateway detached");
      std::fflush(stdout);
    }
    gateway.Reset();
    for (auto& [key, peer] : peers)
      Release(peer, true);
    peers.clear();
  }

  void Release(Peer& peer, bool notifyCore)
  {
    if (peer.phase == Phase::AdmissionPending)
      peer.ticket.Cancel();
    if (peer.client)
    {
      if (notifyCore)
        OnClientDisconnect(*peer.client);
      std::printf("[AtlasNet-GMod endpoint] Released client=%s\n",
                  peer.client->to_string().c_str());
      std::fflush(stdout);
      peer.client.reset();
    }
  }

  void StartClosing(Peer& peer, std::string_view reason, bool notifyCore)
  {
    Frame frame{.event = Event::Kick, .port = peer.port, .token = peer.token};
    frame.SetText(reason);
    if (gateway && Send(gateway, frame) != IO::Ok)
      shutdownGateway = true;
    Release(peer, notifyCore);
    peer.phase = Phase::Closing;
    peer.deadline = Clock::now() + 2s; // Allow the native disconnect packet to leave.
  }

  void ExpirePeers()
  {
    if (shutdownGateway)
    {
      shutdownGateway = false;
      LoseGateway();
    }
    const auto now = Clock::now();
    std::erase_if(peers, [&](auto& item)
    {
      auto& peer = item.second;
      if (now < peer.deadline)
        return false;
      if (peer.client)
      {
        StartClosing(peer, "Source gateway timed out", true);
        return false;
      }
      Release(peer, true);
      return true;
    });
  }

  Peer* FindPort(std::uint16_t port)
  {
    for (auto& [key, peer] : peers)
      if (peer.port == port)
        return &peer;
    return nullptr;
  }

  Peer* FindClient(AtlasNetClientID client)
  {
    for (auto& [key, peer] : peers)
      if (peer.client == client)
        return &peer;
    return nullptr;
  }

  void ReadClient()
  {
    sockaddr_in remote{};
    socklen_t length = sizeof(remote);
    const auto size = recvfrom(publicUdp.Get(), datagram.data(), datagram.size(), 0,
                                reinterpret_cast<sockaddr*>(&remote), &length);
    if (size <= 0 || !gateway)
      return;
    const auto key = (static_cast<std::uint64_t>(remote.sin_addr.s_addr) << 16) | remote.sin_port;
    auto found = peers.find(key);
    if (found == peers.end())
    {
      if (peers.size() >= 64)
        return;
      try
      {
        auto backend = UdpSocket(backendBind);
        Check(::connect(backend.Get(), reinterpret_cast<const sockaddr*>(&backendAddress),
                      sizeof(backendAddress)), "Source backend UDP connect");
        sockaddr_in local{};
        length = sizeof(local);
        Check(getsockname(backend.Get(), reinterpret_cast<sockaddr*>(&local), &length),
              "Source backend UDP address");
        found = peers.emplace(key, Peer{.remote = remote, .backend = std::move(backend),
            .port = ntohs(local.sin_port), .token = ++generation,
            .deadline = Clock::now() + 15s}).first;
      }
      catch (const std::system_error& error)
      {
        std::fprintf(stderr, "[AtlasNet-GMod endpoint] %s\n", error.what());
        return;
      }
    }
    auto& peer = found->second;
    if (peer.phase == Phase::Established)
      peer.deadline = Clock::now() + idleTimeout;
    (void)send(peer.backend.Get(), datagram.data(), static_cast<std::size_t>(size), MSG_DONTWAIT);
  }

  void ReadBackend(Peer& peer)
  {
    const auto size = recv(peer.backend.Get(), datagram.data(), datagram.size(), 0);
    if (size > 0)
      (void)sendto(publicUdp.Get(), datagram.data(), static_cast<std::size_t>(size), MSG_DONTWAIT,
                   reinterpret_cast<const sockaddr*>(&peer.remote), sizeof(peer.remote));
    else if (size < 0 && errno == ECONNREFUSED && peer.client)
      StartClosing(peer, "GMod server unavailable", true);
  }

  void SendAdmission(std::uint16_t port, std::uint32_t request, std::uint32_t token,
                     std::string_view text)
  {
    Frame reply{.event = Event::AdmissionResult, .port = port, .token = token, .value = request};
    reply.SetText(text);
    if (Send(gateway, reply) != IO::Ok)
      shutdownGateway = true;
  }

  void Admit(Peer& peer, const Frame& request)
  {
    if (peer.client)
    {
      if (peer.phase == Phase::Admitted)
        peer.request = request.value;
      SendAdmission(peer.port, request.value, peer.token, peer.client->to_string());
      return;
    }
    if (peer.phase != Phase::Negotiating)
    {
      SendAdmission(peer.port, request.value, 0, "Source connection is already pending or closing");
      return;
    }

    peer.phase = Phase::AdmissionPending;
    peer.request = request.value;
    peer.deadline = Clock::now() + 5s;
    const auto steamID = std::as_bytes(std::span(request.Text()));
    ClientConnectionInfo info{.remoteAddress = ToString(peer.remote),
                              .admission = admission, .spawn = spawn,
                              .admissionPayload = {steamID.begin(), steamID.end()}};
    const auto port = peer.port;
    const auto token = peer.token;
    peer.ticket = OnClientConnect(std::move(info), [this, port, token, serial = request.value]
        (ClientConnectResult result)
        {
          std::scoped_lock lock(mutex);
          auto* current = FindPort(port);
          if (!current || current->token != token || current->request != serial ||
              current->phase != Phase::AdmissionPending)
          {
            if (result.client)
              OnClientDisconnect(*result.client);
            return;
          }
          current->phase = Phase::Negotiating;
          current->deadline = Clock::now() + 8s;
          if (result.status != ClientConnectStatus::Accepted || !result.client)
          {
            SendAdmission(port, serial, 0, result.reason.empty() ?
                          "AtlasNet admission failed" : result.reason);
            return;
          }
          current->client = result.client;
          current->phase = Phase::Admitted;
          current->deadline = Clock::now() + 45s;
          SendAdmission(port, serial, token, result.client->to_string());
          std::printf("[AtlasNet-GMod endpoint] Admitted %s client=%s\n",
                      ToString(current->remote).c_str(), result.client->to_string().c_str());
          std::fflush(stdout);
        });
    if (!peer.ticket)
    {
      peer.phase = Phase::Negotiating;
      SendAdmission(port, request.value, 0, "AtlasNet ingress unavailable");
    }
  }

  void Submit(Peer& peer, ClientInstruction instruction, std::span<const std::byte> bytes,
              bool replaceable = false)
  {
    const ClientPacketView packet{{.channelID = 1, .instruction = instruction,
        .payloadSize = static_cast<std::uint32_t>(bytes.size())}, bytes};
    const auto result = OnClientPacket(*peer.client, packet);
    if (result != ClientPacketSubmission::Queued &&
        !(replaceable && result == ClientPacketSubmission::Backpressure))
      StartClosing(peer, "AtlasNet ingress could not accept the gameplay event", true);
  }

  void ReadGateway()
  {
    Frame frame;
    switch (Receive(gateway, frame))
    {
    case IO::Busy: return;
    case IO::Closed: LoseGateway(); return;
    case IO::Ok: break;
    }
    auto* peer = FindPort(frame.port);
    if (frame.event == Event::Admit)
    {
      if (peer)
        Admit(*peer, frame);
      else
        SendAdmission(frame.port, frame.value, 0, "Connect through the AtlasNet ingress address");
      return;
    }
    if (!peer)
      return;
    if (frame.event == Event::Cancel && peer->request == frame.value &&
        (peer->phase == Phase::AdmissionPending || peer->phase == Phase::Admitted))
    {
      Release(*peer, true);
      peer->phase = Phase::Closing;
      peer->deadline = Clock::now() + 2s;
      return;
    }
    if (peer->token != frame.token || !peer->client)
      return;
    switch (frame.event)
    {
    case Event::Established:
      if (peer->phase == Phase::Admitted)
      {
        peer->phase = Phase::Established;
        peer->deadline = Clock::now() + idleTimeout;
        std::printf("[AtlasNet-GMod endpoint] Source session established client=%s\n",
                    peer->client->to_string().c_str());
        std::fflush(stdout);
      }
      break;
    case Event::Disconnect:
      Release(*peer, true);
      peer->phase = Phase::Closing;
      peer->deadline = Clock::now() + 2s;
      break;
    case Event::Chat:
    case Event::Input:
    case Event::Fire:
    case Event::Sound:
    case Event::Blast:
    case Event::Grenade:
    case Event::SpawnProp:
    case Event::Respawn:
    case Event::WeaponView:
    case Event::RemoveProp:
    case Event::Prop:
    case Event::Marker:
      if (peer->phase == Phase::Established)
        SubmitGameplay(*peer, frame);
      break;
    case Event::Delivered:
      if (peer->phase == Phase::Established)
      {
        if (frame.Text() != "1" && frame.Text() != "world")
        {
          OnClientDeliveryFailure(*peer->client, GameplayDelivery::Reliable);
          break;
        }
        NetBinaryWriter writer;
        writer(frame.Text() == "world" ? Simulation::WorldLifecycle : ChatSignal,
               frame.value, CmdSig::SignalAckStatus::Received);
        Submit(*peer, ClientInstruction::SignalAck, writer.GetBytes());
      }
      break;
    default: break;
    }
  }

  void SubmitGameplay(Peer& peer, const Frame& frame)
  {
    CmdSig::CommandPackage command;
    GameplayOptions options;
    const auto write = [&](std::span<const std::byte> data)
    {
      for (const auto byte : data)
        command.commandPayload.payload.push_back(std::to_integer<std::uint8_t>(byte));
    };
    switch (frame.event)
    {
    case Event::Chat:
      if (!frame.size) return;
      command.commandPayload.commandID = ChatCommand;
      write(std::as_bytes(std::span(frame.Text())));
      break;
    case Event::Input:
    {
      if (!simulation || frame.size != sizeof(Simulation::Input)) return;
      Simulation::Input input;
      std::memcpy(&input, frame.text.data(), sizeof(input));
      NetBinaryWriter payload;
      payload(input);
      write(payload.GetBytes());
      command.commandPayload.commandID = Simulation::MoveCommand;
      command.deliveryMode = CmdSig::CommandDeliveryGuarantee::Unreliable;
      options = {.delivery = GameplayDelivery::Unreliable,
                 .ordering = GameplayOrdering::Sequenced, .stream = 1};
      break;
    }
    case Event::Fire:
    {
      if (!simulation || frame.size != sizeof(Simulation::Bullet)) return;
      Simulation::Bullet bullet;
      std::memcpy(&bullet, frame.text.data(), sizeof(bullet));
      NetBinaryWriter payload;
      payload(bullet);
      write(payload.GetBytes());
      command.commandPayload.commandID = Simulation::FireCommand;
      break;
    }
    case Event::Sound:
    {
      if (!simulation || frame.size != sizeof(Simulation::Sound)) return;
      Simulation::Sound sound;
      std::memcpy(&sound, frame.text.data(), sizeof(sound));
      NetBinaryWriter payload;
      payload(sound);
      write(payload.GetBytes());
      command.commandPayload.commandID = Simulation::SoundCommand;
      command.deliveryMode = CmdSig::CommandDeliveryGuarantee::Unreliable;
      options = {.delivery = GameplayDelivery::Unreliable, .ordering = GameplayOrdering::Unordered};
      break;
    }
    case Event::Blast:
    {
      if (!simulation || frame.size != sizeof(Simulation::Blast)) return;
      Simulation::Blast blast;
      std::memcpy(&blast, frame.text.data(), sizeof(blast));
      NetBinaryWriter payload;
      payload(blast);
      write(payload.GetBytes());
      command.commandPayload.commandID = Simulation::BlastCommand;
      break;
    }
    case Event::Grenade:
    {
      if (!simulation || frame.size != sizeof(Simulation::GrenadeLaunch)) return;
      Simulation::GrenadeLaunch launch;
      std::memcpy(&launch, frame.text.data(), sizeof(launch));
      NetBinaryWriter payload;
      payload(launch);
      write(payload.GetBytes());
      command.commandPayload.commandID = Simulation::GrenadeCommand;
      break;
    }
    case Event::SpawnProp:
    {
      if (!simulation || frame.size != sizeof(Simulation::PropPlacement)) return;
      Simulation::PropPlacement prop;
      std::memcpy(&prop, frame.text.data(), sizeof(prop));
      NetBinaryWriter payload;
      payload(prop);
      write(payload.GetBytes());
      command.commandPayload.commandID = Simulation::SpawnPropCommand;
      break;
    }
    case Event::Respawn:
      if (!simulation || frame.size) return;
      command.commandPayload.commandID = Simulation::RespawnCommand;
      break;
    case Event::RemoveProp:
      if (!simulation || frame.size) return;
      command.commandPayload.commandID = Simulation::RemovePropCommand;
      break;
    case Event::WeaponView:
    {
      if (!simulation || frame.size != sizeof(Simulation::WeaponPresentation)) return;
      Simulation::WeaponPresentation weapon;
      std::memcpy(&weapon, frame.text.data(), sizeof(weapon));
      NetBinaryWriter payload;
      payload(weapon);
      write(payload.GetBytes());
      command.commandPayload.commandID = Simulation::WeaponViewCommand;
      break;
    }
    case Event::Prop:
    case Event::Marker:
      if (!simulation) return;
      command.commandPayload.commandID = Simulation::SpawnCommand;
      command.commandPayload.payload.push_back(static_cast<std::uint8_t>(
          frame.event == Event::Prop ? Simulation::Kind::Prop : Simulation::Kind::Marker));
      break;
    default: return;
    }
    NetBinaryWriter writer;
    writer(options, ++peer.commandSequence, command);
    Submit(peer, ClientInstruction::GameplayCommand, writer.GetBytes(),
           options.delivery == GameplayDelivery::Unreliable);
  }

  const sockaddr_in listenAddress, backendAddress, backendBind;
  const std::string socketPath, admission, spawn;
  const std::chrono::seconds idleTimeout;
  const bool simulation;
  Socket publicUdp, listener, gateway;
  std::jthread worker;
  std::mutex mutex;
  std::unordered_map<std::uint64_t, Peer> peers;
  std::array<std::byte, 65536> datagram{};
  std::uint32_t generation = 0;
  bool ownsSocketPath = false;
  bool shutdownGateway = false;
};

void Register(ModuleRegistry& registry)
{
  registry.RegisterClientEndpoint("source1", [](const ClientEndpointConfig& config)
  {
    return std::make_unique<SourceEndpoint>(config);
  });
  RegisterDemo(registry);
}

const ModuleDescriptor descriptor{
    .name = "AtlasNet-GMod Source1",
    .version = ATLASNET_GMOD_VERSION,
    .capabilities = Capability::ClientEndpoint | Capability::ShardProvider |
                    Capability::ClientSpawnResolver | Capability::ClientAdmissionHandler,
    .Register = Register};
}
}

extern "C" __attribute__((visibility("default")))
const AtlasNet::Module::ModuleDescriptor* AtlasNetGetModule()
{
  return &AtlasNetGMod::descriptor;
}
