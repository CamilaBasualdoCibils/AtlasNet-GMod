#!/usr/bin/env node
'use strict';

// Headless adapter for the SDK's atlasnetstack planner. Launch its validated
// Valkey and node specs the same way as the SDK's own headless smoke test.
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const cp = require('node:child_process');
const { randomUUID } = require('node:crypto');

const root = path.resolve(__dirname, '..');
const sdk = path.join(root, 'AtlasNet-SourceBridge/AtlasNet/tools/vscode-local-stack');
const { defaults, prepare, managedValkeyPort } = require(path.join(sdk, 'stack.js'));
const { tokenName, stopSessionProcesses } = require(path.join(sdk, 'processes.js'));
const token = randomUUID();
const args = new Set(process.argv.slice(2));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const running = child => child.exitCode === null && child.signalCode === null;
const children = [];
let stopping = false;
process.on('SIGINT', () => { stopping = true; });
process.on('SIGTERM', () => { stopping = true; });

function launch(program, options, cwd, name, environment, input = 'ignore') {
  const log = path.join(runtime, name);
  const fd = fs.openSync(log, 'w');
  try {
    const child = cp.spawn(program, options, {
      cwd, env: environment, stdio: [input, fd, fd], detached: true
    });
    child.on('error', error => { child.launchError = error; });
    children.push(child);
    return { child, log };
  } finally { fs.closeSync(fd); }
}

function checkProcesses() {
  for (const child of children) {
    if (child.launchError) throw child.launchError;
    if (!running(child)) throw Error(`Process ${child.pid} exited. See ${runtime}`);
  }
}
async function waitFor(predicate, label, timeout = 45000) {
  const deadline = Date.now() + timeout;
  while (!stopping && Date.now() < deadline) {
    checkProcesses();
    if (await predicate()) return;
    await sleep(150);
  }
  throw Error(`${stopping ? 'Interrupted' : 'Timed out waiting for'} ${label}`);
}
function portReady(port) {
  return new Promise(resolve => {
    const socket = net.connect(port, '127.0.0.1');
    const done = result => { socket.destroy(); resolve(result); };
    socket.setTimeout(150);
    socket.once('connect', () => done(true));
    socket.once('error', () => done(false));
    socket.once('timeout', () => done(false));
  });
}
function inferior(gdb) {
  const proc = `/proc/${gdb.pid}/task/${gdb.pid}/children`;
  try {
    return fs.readFileSync(proc, 'utf8').trim().split(/\s+/).map(Number).find(pid => {
      try { return path.basename(fs.readlinkSync(`/proc/${pid}/exe`)) === 'AtlasNetNode'; }
      catch { return false; }
    });
  } catch { return undefined; }
}

async function stop(nodes, gateways, valkey) {
  // Ingress closes native client sessions while the gateway is still online.
  for (const node of nodes) {
    const pid = inferior(node.child);
    if (pid) { try { process.kill(pid, 'SIGTERM'); } catch {} }
  }
  for (let count = 0; count < 50 && nodes.some(n => running(n.child)); ++count) await sleep(100);
  for (const {child} of gateways) {
    if (!running(child)) continue;
    child.stdin.on('error', () => {});
    child.stdin.write('quit\n');
  }
  for (let count = 0; count < 30 && gateways.some(g => running(g.child)); ++count) await sleep(100);
  if (valkey && running(valkey.child)) {
    try { process.kill(-valkey.child.pid, 'SIGTERM'); } catch {}
  }
  await stopSessionProcesses(token);
  for (const child of children)
    if (running(child)) { try { process.kill(-child.pid, 'SIGKILL'); } catch {} }
}

const dev = path.join(root, '.dev');
fs.mkdirSync(dev, { recursive: true });
const runtime = fs.mkdtempSync(path.join(dev, 'dist-'));

async function main() {
  console.log('AtlasNet stack logs: ' + runtime);
  if (!args.has('--skip-build')) {
    for (const [program, options] of [
      ['cmake', ['--build', 'build', '--target', 'AtlasNetNode', 'AtlasNetShardWorker',
        'AtlasNetXAxisSplit', 'Valkey-Server-Compile', 'atlasnet_source1']],
      [path.join(root, 'tools/install-dev.sh'), []]
    ]) {
      const status = cp.spawnSync(program, options, { cwd: root, stdio: 'inherit' }).status;
      if (status !== 0) throw Error(`Build or deployment failed: ${program}`);
    }
  }
  const settings = fs.readFileSync(process.env.ATLASNET_GMOD_STACK_CONFIG ||
    path.join(root, 'config/atlasnetstack-gmod.json'), 'utf8');
  const config = { ...defaults(), ...JSON.parse(settings.replaceAll('${workspaceFolder}', root)
    .replaceAll('${runtime}', runtime)) };
  if (!args.has('--remote')) {
    const ingress = config.groups[0];
    ingress.nodeSettings.shards = { max_count: 2 };
    ingress.nodeSettings['ingress-endpoints'][0].args.spawn = 'GModDistributed';
    config.groups = [ingress];
  }
  const { launches, files } = await prepare(config, runtime);
  for (const [file, contents] of files) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, contents);
  }

  const environment = { ...process.env, [tokenName]: token,
    ATLASNET_GMOD_RUNTIME: runtime,
    ATLASNET_GMOD_SHARD_LAUNCHER: path.join(root, 'tools/run-shard-gmod.py')
  };
  const worldConfig = path.resolve(environment.GMOD_WORLD_CONFIG || path.join(root, 'config/gmod-world.sh'));
  delete environment.ATLASNET_NODE_CONFIG;
  delete environment.ATLASNET_CLUSTER_CONFIG;
  delete environment.ATLASNET_GMOD_GATEWAY_SOCKET;

  if (await portReady(managedValkeyPort(config.valkeyURI)))
    throw Error("The managed Valkey port is already occupied");
  let valkey;
  const nodes = [];
  const gateways = [];
  const read = activity => fs.readFileSync(activity.log, 'utf8');
  try {
    for (const spec of launches) {
      const isValkey = spec.name === 'AtlasNet / Valkey';
      // Batch GDB exits on a followed helper's termination. Keep the
      // debugger on the node while its worker and Source children run normally.
      const setup = spec.setupCommands.filter(command => !command.text.startsWith('-') &&
        command.text !== 'set detach-on-fork off')
        .flatMap(command => ['-ex', command.text]);
      const process = launch('bash',
        ['-c', 'set -a; source "$1" || exit; shift; exec "$@"', 'gmod-world', worldConfig,
          spec.miDebuggerPath, '--batch', '-ex', 'set pagination off', ...setup, '-ex', 'run', '--args',
          spec.program, ...spec.args], spec.cwd,
        isValkey ? 'valkey.log' : `node-${nodes.length}.log`, environment);
      if (isValkey) {
        valkey = process;
        await waitFor(() => portReady(managedValkeyPort(config.valkeyURI)), 'Valkey listener');
      } else {
        nodes.push(process);
        // Spatial directory subscription runs at node startup. Bring up the
        // controller first so the joining node can resolve its initial route.
        if (nodes.length === 1 && config.groups.length > 1)
          await waitFor(() => /\[Controller\].*Controller started/.test(read(process)),
            'AtlasNet controller startup');
      }
    }
    await waitFor(() => nodes.flatMap(n => [...read(n).matchAll(/Simulation ready shard=/g)]).length === 2 &&
      /installed 2D example.xsplit2d generation [^\n]* with 2 regions/.test(read(nodes[0])),
      'both shard simulations and two controller regions', 65000);
    const command = `'${path.join(root, 'tools/run-dev-server.sh').replaceAll("'", "'\\''")}' -noconclr -noworkshop`;
    const endpoints = config.groups.flatMap(group => group.nodeSettings['ingress-endpoints']);
    const baseServer = path.resolve(process.env.GMOD_SERVER_DIR || path.join(root, '.dev/gmod-server'));
    for (const [index, {args: endpoint}] of endpoints.entries()) {
      let server = baseServer;
      if (index) {
        server = path.join(runtime, `gateway-${index + 1}-server`);
        const copied = cp.spawnSync('cp', ['-a', '--reflink=auto', baseServer, server],
          { stdio: 'inherit' });
        if (copied.status !== 0) throw Error('Could not stage the second GMod gateway');
      }
      const gateway = launch('script', ['-qefc', command, '/dev/null'], root,
        index ? `gateway-${index + 1}.log` : 'gateway.log', {
          ...environment, GMOD_SERVER_DIR: server, GMOD_DEV_PORT: String(endpoint.backend_port),
          ATLASNET_GMOD_DISTRIBUTED: '1', ATLASNET_GMOD_GATEWAY_SOCKET: endpoint.gateway_socket
        }, 'pipe');
      gateways.push(gateway);
      await waitFor(() => /Dedicated server gateway attached/.test(read(nodes[index])) &&
        read(gateway).includes(`ports ${endpoint.backend_port} SV`), `GMod ingress gateway ${index + 1}`);
    }
    console.log('READY: two AtlasNet shards, Source ingress ' +
      endpoints.map(({args: endpoint}) => `${endpoint.address}:${endpoint.port}`).join(', '));
    if (!args.has('--verify-start')) {
      while (!stopping) { checkProcesses(); await sleep(250); }
    }
  } finally { await stop(nodes, gateways, valkey); }
}
main().catch(error => {
  if (!stopping) { console.error(error.message); process.exitCode = 1; }
});
