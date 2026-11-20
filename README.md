# AtlasNet-GMod

AtlasNet-GMod is a native Garry's Mod server module for Linux x86-64. The
server autorun loads `atlasnet` and prints the native version and initialization
status. You can also call the diagnostics from server Lua.

```lua
require("atlasnet")
print(atlasnet.Version())
print(atlasnet.IsInitialized())
```

## Build

Use CMake 3.24 or newer, Ninja, a recent Clang with a C++23 standard library,
and vcpkg. Run these commands from the repository root. If you already have the
pinned vcpkg checkout, set `VCPKG_ROOT` to that checkout.

```sh
git submodule update --init --recursive
mkdir -p .dev
git clone https://github.com/microsoft/vcpkg.git .dev/vcpkg
git -C .dev/vcpkg checkout --detach a42757564758c60651ebc616d9b000b7b91249ef
.dev/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$PWD/.dev/vcpkg"

cmake --preset linux
cmake --build --preset linux --parallel
```

The `atlasnet_gmod` target produces `build/gmsv_atlasnet_linux64.dll`, an
ELF shared module. It links `AtlasNetCore` statically and uses the SDK's
allocator for the native module's lifetime state. The preset uses the SDK's
vcpkg manifest and static PIC triplet. The embedded SDK's Tracy profiler is
disabled, including when the GMod target is built in Debug.

| Dependency | Pin |
| --- | --- |
| AtlasNet-SourceBridge | `e28dc7141ffcf1390883709bd71aa5f1756fc3ac` |
| AtlasNet SDK | `a3a8d37cffd32b23680d33454d00ab524f4253ec`, nested git submodule |
| Facepunch gmod-module-base | `4fdafe7d762fe63f0360c67d9e119a129292712e`, with a SHA-256-verified archive |
| vcpkg ports and toolchain | `a42757564758c60651ebc616d9b000b7b91249ef` |
| SDK-managed source dependencies | mimalloc 3.5.3, EnTT 3.16.0, and Valkey 9.1.1, as pinned by the SDK |

The build adapts Facepunch's headers for the x86-64 Lua state layout and the
`ILuaBase::SetState` slot used by diagnostics called from coroutines.
[garrysmod_common's headers](https://github.com/danielga/garrysmod_common/tree/master/include/GarrysMod/Lua)
provide the layout reference. The project needs only the Lua module ABI.

## Run and verify locally

Install Valve [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD)
and its 32-bit runtime prerequisites. The installer selects app 4020 on the
`x86-64` branch. It installs the native module in `garrysmod/lua/bin` and the
server autorun in the `atlasnet_gmod_dev` addon. The launcher starts a local
Sandbox server on `gm_construct`. Enter `quit` in the dedicated server console to
shut it down.

```sh
./tools/install-dev.sh
./tools/run-dev-server.sh
```

Use `./tools/install-dev.sh --update-server` to update the Steam installation
and redeploy, or `--server-only` to install or update without deploying AtlasNet.
Run the installer while the server is stopped.

| Environment variable | Default |
| --- | --- |
| `STEAMCMD` | `steamcmd` from `PATH` |
| `ATLASNET_GMOD_BUILD_DIR` | `build` under the repo root |
| `GMOD_SERVER_DIR` | `.dev/gmod-server` under the repo root |
| `GMOD_DEV_PORT` | `27015` |

The smoke tests use Python 3 and util-linux's `script` to drive the real server
console and save its logs under `.dev`.
The baseline disables addons so it can also run after deployment. To test a
fresh server with no module files installed, run the baseline immediately after
`--server-only`.

```sh
python3 tests/smoke-dev-server.py --baseline
python3 tests/smoke-dev-server.py
```

## Repository boundary

At its pinned revision, SourceBridge is a container for the nested SDK, so
AtlasNet-GMod links `AtlasNetCore` directly. GMod headers, Lua, lifecycle and
deployment are local to this repository. Future reusable Source 1 code
belongs in SourceBridge, where it can be built independently of GMod and used
by game adapters such as a future TF2 integration.

The pinned SDK already exposes `AtlasNet::Module::IClientEndpoint` and
`ModuleRegistry::RegisterClientEndpoint` for the next integration milestone.
The GMod implementation belongs here. Add a SourceBridge target only when there
is reusable Source 1 code to extract.

## Source 1 ingress prototype

Run the Task 2 demo after the build configuration above.

```sh
python3 tools/run-source1-dev.py
```

The launcher builds `atlasnet_source1.so`, the SDK's `AtlasNetNode-Local` and
`AtlasNetShardWorker`, and deploys the native GMod module. It starts the node
with a private loopback Valkey instance, waits for the controller to assign
one shard, and then starts the GMod dedicated server. Ctrl+C stops the stack.
The launcher prints the directory containing both server logs.

The `source1` factory is configured in [the node YAML](config/source1-dev.yaml).
[The cluster YAML](config/source1-cluster.yaml) selects a single shard. The
ingress listens on UDP `127.0.0.1:27015`, and the GMod server listens on
`127.0.0.1:27016`. The launcher expects these ports and the default
`gateway.sock` socket name when supplying alternate node YAML with `--config`.

1. Open the console in a vanilla GMod client and run `connect 127.0.0.1:27015`.
   For a same-host client that retries without reaching the server, first run
   `net_usesocketsforloopback 1`. The Windows x64 client under Proton used
   this built-in setting during verification.
2. After joining Sandbox on `gm_construct`, enter `hello atlasnet` in public
   chat or run `say hello atlasnet` in the console.
3. The player receives `[AtlasNet] hello atlasnet` in the native chat box.
   `disconnect` exits the game and releases the AtlasNet session.

Use a compatible GMod client and server build. Two-client LAN testing can use
a separate vanilla game instance with the game's `-multirun` option. Team chat
uses the ordinary GMod path.

### Connection and gameplay lifecycle

- The AtlasNet endpoint owns the client-facing UDP socket. It forwards Source
  datagrams through a distinct connected UDP socket for each client address
  and port. The real GMod engine handles queries, challenges, netchannels,
  sign-on, map synchronization, and snapshots.
- GMod's `CheckPassword` hook uses the backend peer address and port to identify
  the pending connection. The native module asks the endpoint for admission
  over a same-user Unix `SOCK_SEQPACKET` channel. GMod's synchronous hook waits
  for up to four seconds while AtlasNet evaluates the admission handler,
  resolves placement, and spawns the client's assigned entity. The native
  socket receives either the assigned ClientID or the rejection reason.
  GMod associates the ID on `PlayerInitialSpawn`.
- Public chat becomes an existing Client V1 `GameplayCommand` packet with a
  `CommandPackage`. The endpoint submits it through `OnClientPacket`. AtlasNet
  routes it to the shard owning that client's entity. The demo shard handles the
  Command and calls `DispatchRawSignals` with the same ClientID as recipient.
  The endpoint receives the Signal through `DispatchClientPacket`, then GMod
  sends a native `ChatPrint` to that player and reports a Signal ACK through
  AtlasNet ingress. Source owns wire reliability to the client.
- Native disconnect events release the session. Timeouts cancel pending
  admission or close the active association. The endpoint checks a connection
  token on private events to discard late messages after port reuse. Loss of
  the private channel releases the sessions owned by that GMod server.

The endpoint uses the SDK contracts in `include/AtlasNet/Module`, the
`ClientIngressManager` lifecycle, and the same packet envelopes as
`AtlasNetClient`. The shard uses the SDK's worker and Command/Signal routing
with a minimal owned-entity handler. SourceBridge is currently the container for
the SDK submodule. All GMod integration stays in this repository.

### Prototype verification and limits

Run the automated endpoint test after the demo targets have been built.

```sh
python3 tests/smoke-source1-endpoint.py
```

That test uses a controlled gateway and the actual AtlasNet node and shard.
It covers separate UDP routes, targeted Command/Signal delivery, admission
rejection and cancellation, inactivity, gateway loss, and endpoint shutdown.
The vanilla client procedure above covers Source sign-on and visible chat.

This prototype is loopback IPv4 with a single fixed-map GMod backend, one demo
shard, and a 64-peer cap that includes connectionless traffic. A client that
silently disappears before Source can report a disconnect is detected by the
admission or sign-on deadline. On abrupt GMod backend loss, clients may wait
for Source's own timeout. The private channel uses bounded, reliable local
delivery. The SDK uses native delivery feedback and backpressure handling,
while its current Client V1 path treats protocol ACKs as informational.

## Distributed Sandbox demo

With AtlasNet Local Stack 0.3.5 or later installed, open this repository
as a single-folder workspace in VS Code, run
**AtlasNet: Open Local Stack**, and click **Run stack**. The saved workspace
settings build and deploy the modules, start Valkey and both nodes, and bring
up a GMod gateway for each Source endpoint. Use **Stop gracefully** when
finished. The GDB sessions attach to the AtlasNet nodes while their shard and
GMod subprocesses run independently.

To run the same topology from the repository root without VS Code:

```sh
node tools/run-atlasnetstack.cjs --remote
```

Wait for `READY`, then join with two vanilla GMod clients. The first connects
to `127.0.0.1:27215` and the second to `127.0.0.1:27315`. These ports
allow the Steam client to keep its default local Source port. Each Source
endpoint and its dedicated-server gateway belong to a different AtlasNet
node. Both nodes run on the local host for this demo. On the same host as the
server, run `net_usesocketsforloopback 1` in both client consoles before
connecting. Omit `--remote` to use one ingress and two real shards on one node.
Pass `--verify-start` to verify both shard simulations and the two Controller
regions, then shut down.

### Multiple Steam clients on Linux

Keep Steam open and signed in with the Windows x64 version of Garry's Mod
and Proton installed. From this repository, launch one client per endpoint:

```sh
tools/run-gmod-client.sh 127.0.0.1:27215 &
tools/run-gmod-client.sh 127.0.0.1:27315 &
wait
```

Each invocation creates its own Proton prefix, writable game directory, and
Source console log under `.dev/gmod-clients`. The launcher locates the Steam
install, shares its content archives, and connects automatically. Use
`tools/run-gmod-client.sh --help` for installation overrides or extra Source
launch flags.

### Stack configuration

The launcher uses AtlasNet's Local Stack planner to validate and generate
node settings and launch specs. It starts the Controller before the second
node subscribes to the spatial directory. The Controller loads the SDK's
`example.xsplit2d` heuristic with the cluster's two-dimensional `XY` plane.
Source uses Z for height. The local world configuration places the two
spawns on an outdoor row of `gm_construct` with Source and AtlasNet's
`x = 0` planes aligned. Set `GMOD_WORLD_CONFIG` to use a different
map or coordinate transform. Logs and generated settings are under
the `.dev/dist-*` directory printed on startup.

1. Use the pistol to shoot the other player from the initial spawn area.
   Each player starts on a different shard. The target's native HUD reflects
   damage applied by the owning shard. Run `atlasnet_diag` to capture it in
   the client console.
2. Walk back and forth across the split. Run `atlasnet_diag` in the client
   console to see ClientID, EntityID, current ShardID, region, and proxy role.
3. Walk away from the other player and back to exercise visibility exit and
   reentry. Spawn a prop through the Sandbox spawn menu to register the
   selected model and placement with AtlasNet. Run
   `atlasnet_marker` to put a solid managed marker 80 Source units in front
   of the player's view. A marker placed between the two spawns can block a
   shot at the player behind it. Disconnect both clients when finished.

AtlasNet-TUI can display the live regions and connected clients. After pressing
Play in Local Stack, run the **AtlasNet TUI** VS Code task or the launcher below.
The launcher uses the Control Panel address and port from the workspace settings.
This stack uses port `27270` and the standalone TUI defaults to `8080`.

```sh
tools/run-atlasnet-tui.sh
python3 tests/smoke-atlasnet-tui.py ../AtlasNet-TUI/build/atlasnet-tui
```

The TUI smoke test supports repeated `--expect CLIENT=SHARD` arguments to
assert ownership before and after a crossing. Node metrics are exposed at
`http://127.0.0.1:27280/metrics` and, in two-node mode,
`http://127.0.0.1:27281/metrics`. Save each client console log as
`client-a.log` or `client-b.log` in the run directory and, for the two-node
check, save the corresponding Prometheus responses as
`metrics-node-0.prom` and `metrics-node-1.prom` while both clients are still
connected. The verifier checks the recorded real deployment.

```sh
python3 tests/check-distributed-run.py .dev/dist-EXAMPLE --remote --combat --visibility --marker --disconnect
```

Pass `--single-crossing` when verifying a run that exercised one transfer.

### Integration design

- `src/Endpoint` keeps the Source session on its original endpoint and
  translates GMod input to AtlasNet Commands and world updates to Source
  messages. `lua/autorun/server/atlasnet_gateway.lua` applies corrections and
  maintains per-client Source proxies selected by AtlasNet Spatial Interests.
- `src/Simulation` implements the shard provider, gameplay and migration
  callbacks, message types, and local process lifecycle.
  `tools/run-shard-gmod.py` runs one dedicated GMod simulation process per
  AtlasNet ShardID. `src/Server/SimulationLua.cpp` and
  `lua/autorun/server/atlasnet_simulation.lua` run native entity operations and
  traces on the Source game thread. Each shard worker talks only to its own
  Source process over a local Unix socket.
- The SDK assigns each player's identity and spawn. Each simulation maps its
  AtlasNet EntityIDs to local Source handles. Players use native movement.
  Players, props and markers report transforms and contribute different
  AtlasNet entity costs. Transfer callbacks serialize gameplay state through
  the SDK and restore the native representation on the destination shard.
- Stock Sandbox weapons retain their native inventory, ammo, reload, and
  cooldown on the Source session gateway. The gateway forwards one Command
  per native hitscan pellet with its spread-adjusted direction and damage.
  SpatialRPC fans the ray out to authoritative shards. Native Source traces
  produce candidates, including map geometry. EntityRPC applies damage on
  the selected entity's current owner. The owning shard also reports armor,
  deaths and respawns to the original Source session.
- Shards use Spatial Signals for nearby player animation, sound, and explosion
  presentation. Spatial Interests still control proxy creation and removal.
  A sample revision travels with the entity during migration, so a stale
  state update cannot overwrite newer health or animation state on the gateway.
  Player proxies select native locomotion sequences for the held weapon type
  and attach the weapon's world model using Source bone merge.
- Stock Sandbox dynamite uses a spatial EntityRPC audience for damage. The
  eventual owners apply native radius and occlusion logic to their assigned
  entities. The gateway that originally ran the stock effect skips its
  forwarded effect copy.
- The stock frag and SMG launchers transfer their native grenades through
  AtlasNet. The owning shard runs the Source entity and transfers its fuse or
  arming state, velocity, and blast parameters. A detonation routes damage
  through spatial EntityRPC, sends a nearby explosion effect and removes its
  replicas on each ingress.
- The stock RPG uses a native missile on the owning shard. The launcher's
  current laser target follows the missile's stable AtlasNet entity ID, so
  changing aim still steers the missile after a transfer.
- The Physics Gun and Gravity Gun select managed physics props through
  SpatialRPC. The current prop owner runs grab, hold, release and freeze
  operations or the Gravity Gun punt through native physics.
- Stock Sandbox prop creation supplies the selected model and Source placement.
  All ingress gateways receive the same map-wide prop set using the native
  map bounds, and Source handles which models are visible to each client.
  The stock Remover primary action can select the caller's managed prop through
  SpatialRPC and delete it through EntityRPC after an ownership change.
- AtlasNet metrics report clients, endpoint packets, and entities per shard
  and region. Shard logs include query results and latency, native trace
  turnaround, transfer timings, damage, Command handler time, and Signal
  dispatch counts. The demo uses the stack's Valkey for AtlasNet infrastructure.

Use `python3 tests/check-sandbox-run.py RUN weapons death presentation held-weapon
dynamite frag remover sustained` to validate recorded gameplay evidence. Supply only the
scenarios exercised in that run. The weapon and presentation counters appear
in `atlasnet_diag`. A sustained run also records start and end timestamps in
`playtest-timeline.log` using `date -Ins`.

For a reproducible setup, add `tests/fixtures/sandbox_scenarios.lua` to the
server autorun directory of a disposable loopback gateway. The helper commands
place and detonate the stock Sandbox dynamite, equip the stock frag, and place
a barrel across the split for the stock Remover. Use standard attack, reload,
and jump inputs during the test.

For the x-split run, also install `tests/fixtures/client_observations.lua` in
the disposable gateways' shared autorun directory. Start `atlasnet_task6_watch`
on each Steam client before moving the other player. Save the real client logs
as `client-a.log` and `client-b.log`. Capture TUI before and after swapping
sides with `--expect CLIENT=SHARD` and `--expect-x-sign CLIENT=-1|1`, and save
its printed results as `tui-before.txt` and `tui-swapped.txt`. The
`split` and `impact` checks validate these recordings and the node logs.

Player replicas use the player's selected model, native locomotion
sequences, and aim pose. Compatible world models use Source bone merge and
other world models follow the right-hand attachment. Weapon gestures,
authoritative ragdolls, full kill attribution, arbitrary addon entities,
constrained assemblies, and cross-host deployment remain outside the
verified path.
