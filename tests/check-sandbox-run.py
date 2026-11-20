#!/usr/bin/env python3
"""Check stock Sandbox gameplay evidence from an actual atlasnetstack run."""
import argparse
from datetime import datetime
from pathlib import Path
import re


IDENTITY = re.compile(r"\[AtlasNet\] client=(\S+) entity=(\d+) shard=(\S+)")
VITALS = re.compile(
    r"\[AtlasNet\] vitals health=(-?\d+) armor=(\d+) alive=(true|false) weapon=(\S+)"
    r" clip=(-?\d+) reserve=(\d+) sounds=(\d+) gaits=(\d+) blasts=(\d+)"
)
TUI = re.compile(r"TUI client=(\S+) entity=(\S+) shard=(\S+) region=(\d+) position=\(([^,]+),")
REPLICA = re.compile(
    r"\[AtlasNet QA\] replica (\d+) x=([^ ]+) .* seq=(\S+) cycle=(\S+) model=(\S+)"
)
HELD_WEAPON = re.compile(
    r"\[AtlasNet QA\] aim entity=(\d+) yaw=(\S+) (?:aim_pitch|body_pitch|head_pitch)=(\S+)"
    r".* weapon=(\S+) hand_error=(\S+) root_dist=(\S+)"
)
MISSILE = re.compile(
    r"\[AtlasNet QA\] replica (\d+) x=(\S+) y=(\S+) .* model=models/weapons/w_missile(?:_launch|_closed)?\.mdl"
)


def check(condition, description):
    if not condition:
        raise AssertionError(description)
    print(f"PASS {description}")


def weapon_checks(client_logs):
    for log in client_logs:
        states = VITALS.findall(log)
        if len({state[3] for state in states if int(state[4]) > 0}) < 2:
            continue
        if not any(old[3] == new[3] and int(old[4]) < int(new[4]) and int(old[5]) > int(new[5])
                   for old, new in zip(states, states[1:])):
            continue
        snapshots = []
        owner = None
        for line in log.splitlines():
            if identity := IDENTITY.search(line):
                owner = identity[3]
            if state := VITALS.search(line):
                snapshots.append((owner, state.group(4, 5, 6)))
        if any(old_owner != new_owner and old_weapon == new_weapon and int(old_weapon[1]) > 0
               for (old_owner, old_weapon), (new_owner, new_weapon) in zip(snapshots, snapshots[1:])):
            return True
    return False


def split_checks(run, client_logs, node_logs):
    snapshots = []
    for filename in ("tui-before.txt", "tui-swapped.txt"):
        snapshots.append({client: (entity, shard, region, float(x))
                          for client, entity, shard, region, x in TUI.findall((run / filename).read_text())})
    before, after = snapshots
    check(len(before) == 2 and before.keys() == after.keys() and
          len({value[1] for value in before.values()}) == 2 and
          len({value[1] for value in after.values()}) == 2 and
          {value[2] for value in before.values()} == {"1", "2"} and
          all(before[client][0] == after[client][0] and before[client][1] != after[client][1] and
              before[client][3] * after[client][3] < 0 for client in before),
          "TUI confirmed both original clients crossed x=0 and swapped authoritative shards")

    ids = [IDENTITY.search(log)[2] for log in client_logs]
    for index, log in enumerate(client_logs):
        poses = REPLICA.findall(log)
        moving = [(ent, float(x), cycle) for ent, x, seq, cycle, model in poses
                  if seq.startswith(("walk", "run")) and model.startswith("models/player/")]
        check(any(x0 * x1 < 0 and idx0 == idx1 and cycle0 != cycle1
                  for idx0, x0, cycle0 in moving for idx1, x1, cycle1 in moving),
              f"client {index + 1} observed a moving player sequence advance on both sides of x=0")
        check(any(re.search(rf"GMod ray complete .*results=2 nearest={ids[1 - index]} "
                            rf".*shooter={ids[index]}\b", node) for node in node_logs)
              and any(int(tracers) > 0 for tracers in re.findall(r"\btracers=(\d+)", log)),
              f"client {index + 1} hit its remote peer and received a remote tracer")
        owner = after[IDENTITY.search(log)[1]]
        check(any(model.startswith("models/props") and float(x) * owner[3] < 0 and
                  abs(float(x) - owner[3]) > 320 for _, x, _, _, model in poses),
              f"client {index + 1} observed the selected prop model on the other shard beyond the old cutoff")


def grenade_crossings(shard_log, name):
    origins = re.findall(rf"GMod {name} spawn entity=(\d+) shard=(\S+) .* x=([^\s]+)", shard_log)
    directions = set()
    for entity, origin, start in origins:
        detonations = re.findall(rf"GMod {name} detonated entity={entity} shard=(\S+) x=([^\s]+)", shard_log)
        if len(detonations) != 1:
            continue
        destination, end = detonations[0]
        if origin != destination and float(start) * float(end) < 0 and re.search(
                rf"GMod transfer restore entity={entity} shard={destination}\b", shard_log):
            directions.add(float(start) > 0)
    return directions


def split_owners(run):
    owners = {client: (shard, float(x)) for client, _, shard, _, x in
              TUI.findall((run / "tui.txt").read_text())}
    check(len(owners) == 2 and len({shard for shard, _ in owners.values()}) == 2 and
          {x > 0 for _, x in owners.values()} == {False, True},
          "AtlasNet-TUI recorded the two Steam clients on opposite sides of x=0")
    return owners


def guided_rocket_checks(client_logs, shard_log):
    for index, log in enumerate(client_logs):
        missiles = {}
        trajectories = []
        for entity, x, y in MISSILE.findall(log):
            x, y = float(x), float(y)
            if entity not in missiles or abs(missiles[entity][-1][0] - x) > 400:
                missiles[entity] = []
                trajectories.append(missiles[entity])
            missiles[entity].append((x, y))
        turned = set()
        for positions in trajectories:
            source = positions[0][0]
            course = [y for x, y in positions if source * x < 0]
            if len(course) < 3:
                continue
            turn = course.index(min(course))
            if course[0] - course[turn] > 20 and max(course[turn:]) - course[turn] > 60:
                turned.add(source > 0)
        check(turned == {False, True},
              f"client {index + 1} observed both rockets cross x=0 and turn toward the changed aim")
    check(grenade_crossings(shard_log, "rocket") == {False, True},
          "stock RPG missiles detonated once on the destination shard in both directions")
    for entity, destination in re.findall(r"GMod rocket detonated entity=(\d+) shard=(\S+)", shard_log):
        check(f"GMod rocket guidance received entity={entity} shard={destination}" in shard_log,
              f"rocket {entity} received launcher guidance on its destination shard")


def physics_checks(owners, client_logs, shard_log):
    for index, log in enumerate(client_logs):
        poses = [(float(yaw), float(pitch)) for _, yaw, pitch, _, error, root_distance in
                 HELD_WEAPON.findall(log) if float(error) < 8 and float(root_distance) > 25]
        check(len(poses) > 1 and len({round(yaw) for yaw, _ in poses}) > 1 and
              max(pitch for _, pitch in poses) - min(pitch for _, pitch in poses) > 0.025,
              f"client {index + 1} observed changing remote aim with a weapon attached at the hand")
    identities = {entity: owners[client][0] for client, entity, _ in
                  (IDENTITY.search(log).groups() for log in client_logs)}
    for tool, final_action, name in (("1", "3", "Physics Gun"), ("2", "5", "Gravity Gun")):
        grabs = re.findall(rf"GMod physics tool={tool} action=1 entity=(\d+) player=(\d+) shard=(\S+)", shard_log)
        check(any(identities.get(player) != origin and re.search(
            rf"GMod transfer restore entity={prop} shard={identities.get(player)} ", shard_log) and re.search(
            rf"GMod physics tool={tool} action={final_action} entity={prop} player={player} "
            rf"shard={identities.get(player)}\b", shard_log) for prop, player, origin in grabs),
            f"{name} grabbed a remote prop and completed its action on the new owner")


def verify(run, cases):
    client_logs = [(run / name).read_text(errors="replace") for name in ("client-a.log", "client-b.log")]
    node_logs = [path.read_text(errors="replace") for path in sorted(run.glob("node-*.log"))]
    shard_log = "\n".join(node_logs)
    check(len(node_logs) == 2, "two real AtlasNet nodes recorded")
    for log in client_logs:
        identities = IDENTITY.findall(log)
        check(log.count("Connected to ") == 1 and "Signon traffic" in log
              and len({identity[:2] for identity in identities}) == 1,
              "vanilla Source session retained the same AtlasNet client and entity identity")
    if "weapons" in cases:
        check(weapon_checks(client_logs), "stock weapon switching, magazine reload, and clip/reserve continuity across migration")
    if "death" in cases:
        check(any(re.search(r"alive=false", log) and
                  re.search(r"alive=false[\s\S]*?\[AtlasNet\] vitals .*alive=true", log)
                  for log in client_logs), "native gateway client died and respawned")
    if "presentation" in cases:
        states = [state for log in client_logs for state in VITALS.findall(log)]
        check(any(int(state[6]) > 0 for state in states), "cross-endpoint native sound replayed")
        check(any(int(state[7]) & 0b111110 for state in states), "remote proxy selected a native moving animation sequence")
    if "held-weapon" in cases:
        switches = [int(count) for log in client_logs for count in re.findall(r"\bweapons=(\d+)", log)]
        check(any(count >= 2 for count in switches), "remote gateway attached different native held weapon models")
    if "dynamite" in cases:
        targets = {IDENTITY.search(log)[2] for log in client_logs}
        check(any("GMod blast source=" in log for log in node_logs)
              and all(any(f"GMod blast dispatched entity={target} " in log for log in node_logs)
                      for target in targets), "native dynamite blast reached both actual owners through EntityRPC")
        check(any(int(state[8]) > 0 for log in client_logs for state in VITALS.findall(log)),
              "remote endpoint played the native explosion effect")
    if "frag" in cases:
        frags = re.findall(r"GMod frag spawn entity=(\d+)", shard_log)
        check(any(len(re.findall(rf"GMod frag detonated entity={entity}\b", shard_log)) == 1
                  and re.search(rf"GMod transfer restore entity={entity}\b", shard_log)
                  for entity in frags), "stock frag crossed a shard boundary and detonated once")
    if "split" in cases:
        split_checks(run, client_logs, node_logs)
        check(grenade_crossings(shard_log, "frag") == {False, True},
              "stock frag crossed x=0 in both directions and each detonated once")
        for name in ("gateway.log", "gateway-2.log"):
            gateway = (run / name).read_text(errors="replace")
            check(all(re.search(rf"Proxy destroy .*entity={entity}\b", gateway) for entity in
                      re.findall(r"GMod frag detonated entity=(\d+)", shard_log)),
                  f"{name} removed the expired grenades from the clients' world")
    if "impact" in cases:
        check(grenade_crossings(shard_log, "impact grenade") == {False, True},
              "stock SMG impact grenade crossed x=0 in both directions and each detonated once")
    if "guided-rocket" in cases or "physics-tools" in cases:
        owners = split_owners(run)
        if "guided-rocket" in cases:
            guided_rocket_checks(client_logs, shard_log)
        if "physics-tools" in cases:
            physics_checks(owners, client_logs, shard_log)
    if "remover" in cases:
        props = re.findall(r"GMod prop spawn entity=(\d+) shard=(\S+) model=(\S+)", shard_log)
        check(any(owner != origin and
                  f"GMod transfer restore entity={entity} shard={owner} " in shard_log
                  for entity, origin, _ in props
                  for owner in re.findall(rf"GMod remover entity={entity} shard=(\S+)", shard_log)),
              "stock Remover deleted the managed prop on its new owner")
    if "sustained" in cases:
        timeline = [datetime.fromisoformat(line) for line in (run / "playtest-timeline.log").read_text().splitlines()]
        check((timeline[-1] - timeline[0]).total_seconds() >= 300, "at least five minutes of connected play exercised")
        check(len(re.findall(r"GMod transfer restore entity=", shard_log)) >= 4,
              "repeated authoritative shard transfers recorded")
        check(not re.search(r"SIGSEGV|GMod simulation process disconnected|Fatal Lua Error", shard_log),
              "no simulation crash recorded during the sustained session")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("cases", nargs="+",
                        choices=("weapons", "death", "presentation", "held-weapon",
                                 "dynamite", "frag", "remover", "sustained", "split", "impact",
                                 "guided-rocket", "physics-tools"))
    args = parser.parse_args()
    try:
        verify(args.run, args.cases)
    except (OSError, AssertionError) as error:
        parser.exit(1, f"FAIL {error}\n")
