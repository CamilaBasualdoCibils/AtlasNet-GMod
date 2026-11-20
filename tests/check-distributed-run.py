#!/usr/bin/env python3
"""Validate evidence collected from a live atlasnetstack GMod session."""
import argparse
from datetime import datetime
import re
from pathlib import Path


IDENTITY = re.compile(r"\[AtlasNet\] client=(\S+) entity=(\d+) shard=(\S+) region=(\d+)")
TRANSFERS = re.compile(r"GMod transfer (extract|restore) entity=(\d+) shard=(\S+) transfer=(\S+)")
TRACE = re.compile(r"\[([^]]+)\].*GMod trace shard=(\S+) shooter=(\d+) "
                   r"hit=true target=(\d+) distance=([\d.]+)")
RAY = re.compile(r"\[([^]]+)\].*GMod ray complete shard=\S+ results=2 "
                 r"nearest=(\d+) elapsed_ms=[\d.]+ shooter=(\d+)")


def require(condition, description):
    if not condition:
        raise AssertionError(description)
    print("PASS " + description)


def read(path):
    return path.read_text(errors="replace")


def marker_blocks_player(log, entities):
    markers = set(re.findall(r"GMod object spawn entity=(\d+) shard=\S+ kind=2", log))
    traces = [(datetime.fromisoformat(time), shard, shooter, target, float(distance))
              for time, shard, shooter, target, distance in TRACE.findall(log)]
    for time, winner, shooter in RAY.findall(log):
        if winner not in markers:
            continue
        completed = datetime.fromisoformat(time)
        candidates = [(at, shard, target, distance) for at, shard, source, target, distance in traces
                      if source == shooter and 0 <= (completed - at).total_seconds() <= 0.1]
        if any(near_target == winner and far_target in entities and near_shard != far_shard
               and near_distance < far_distance
               for _, near_shard, near_target, near_distance in candidates
               for _, far_shard, far_target, far_distance in candidates):
            return True
    return False


def client_took_damage(node_log, client_log, entity):
    legacy = "Health 75" in client_log and re.search(
        rf"GMod damage entity={entity} .*health=75\b", node_log)
    native_health = re.findall(r"\[AtlasNet\] vitals health=(-?\d+)", client_log)
    native = any(int(health) < 100 and
                 re.search(rf"GMod vitals entity={entity} .*health={health}\b", node_log)
                 for health in native_health)
    return legacy or native


def verify(run, remote, combat, visibility, disconnect, marker, single_crossing):
    nodes = sorted(run.glob("node-*.log"))
    gateway = "\n".join(map(read, sorted(run.glob("gateway*.log"))))
    logs = [read(run / name) for name in ("client-a.log", "client-b.log")]
    node_log = "\n".join(map(read, nodes))
    require(len(nodes) == (2 if remote else 1), "expected number of real AtlasNet nodes")
    require(len(set(re.findall(r"Simulation ready shard=(\S+)", node_log))) == 2
            and re.search(r"example\.xsplit2d generation \d+ with 2 regions", node_log),
            "two GMod shard simulations and two Controller regions")
    ports = re.findall(r"\[AtlasNet-GMod endpoint\] Listening 127\.0\.0\.1:(\d+) ->", node_log)
    require(len(ports) == len(nodes), "Source endpoint listeners recorded in the stack logs")
    if not remote:
        ports *= 2
    sessions = [IDENTITY.findall(log) for log in logs]
    require(all(log.count("Connected to ") == 1 and f"Connected to 127.0.0.1:{port}" in log
                and "Signon traffic" in log for log, port in zip(logs, ports)),
            "vanilla sign-on at the configured endpoints with no reconnect")
    require(all(sessions) and len({session[0][0] for session in sessions}) == 2
            and len({session[0][1] for session in sessions}) == 2
            and len({session[0][2] for session in sessions}) == 2,
            "distinct client IDs, entity IDs and initial shards")
    require(all(len({event[:2] for event in session}) == 1 for session in sessions),
            "stable ClientID and EntityID through all ownership changes")
    entities = [session[0][1] for session in sessions]
    transfers = TRANSFERS.findall(node_log)
    restored = {(entity, transfer): shard for kind, entity, shard, transfer in transfers
                if kind == "restore"}
    completed = {(entity, shard) for kind, entity, shard, transfer in transfers
                 if kind == "extract" and (entity, transfer) in restored
                 and restored[entity, transfer] != shard}
    directions = 1 if single_crossing else 2
    require(any(len({event[2] for event in session}) == 2
                and sum((session[0][1], shard) in completed
                        for shard in {event[2] for event in session}) >= directions
                for session in sessions),
            "paired extraction and restoration across the boundary"
            + (" in both directions" if not single_crossing else ""))
    for observer, target in (entities, entities[::-1]):
        require(f"Proxy enter observer={observer} entity={target}" in gateway,
                f"Spatial Interest creates player proxy {observer} -> {target}")
    if visibility:
        require(any(f"Proxy exit observer={observer} entity={target}" in gateway
                    for observer, target in (entities, entities[::-1])),
                "Spatial Interest removes a player proxy when the client leaves range")
    if remote:
        metrics = [read(run / f"metrics-node-{index}.prom") for index in range(2)]
        active = [re.search(r'atlasnet_ingress_endpoint_active_clients\{node_id="([^"]+)",'
                            r'endpoint_type="source1",endpoint_id="([^"]+)"\} 1$', log, re.M)
                  for log in metrics]
        require(all(active) and len({match[1] for match in active}) == 2
                and len({match[2] for match in active}) == 2,
                "one active Source endpoint client per distinct node in Prometheus")
    if combat:
        require(any(re.search(rf"GMod ray complete .*results=2 nearest={entity}\b", node_log)
                    and client_took_damage(node_log, log, entity)
                    for log, entity in zip(logs, entities)),
                "two-shard ray selects a player, whose owner applies damage and client receives it")
    if disconnect:
        require(all(f"Released client={session[0][0]}" in node_log for session in sessions),
                "both endpoints released their AtlasNet clients on disconnect")
    if marker:
        require(marker_blocks_player(node_log, entities),
                "nearer native marker wins over a farther player on another shard")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--remote", action="store_true")
    parser.add_argument("--combat", action="store_true")
    parser.add_argument("--visibility", action="store_true")
    parser.add_argument("--disconnect", action="store_true")
    parser.add_argument("--marker", action="store_true")
    parser.add_argument("--single-crossing", action="store_true",
                        help="allow a run that exercises a single transfer instead of a round trip")
    args = parser.parse_args()
    try:
        verify(args.run, args.remote, args.combat, args.visibility,
               args.disconnect, args.marker, args.single_crossing)
    except (OSError, AssertionError) as error:
        parser.exit(1, f"FAIL {error}\n")
