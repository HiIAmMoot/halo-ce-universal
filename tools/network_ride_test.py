"""The network riding gate (port/linux/NETCODE.md, Testing): two hidden Windows builds of this tree on this PC, a
host and a client, play each riding test (port/linux/game/network_test.c, debug.network_test_ride) at each round trip,
and their debug.txt records are aligned by the ticks the netcode carries and judged.
The Longsword's --local mode runs one split-screen game without networking, judging only its host-labelled log.

    python tools/network_ride_test.py --output <a new folder>

Exit 0: every required case passed; 1: a required case desynced and nothing failed as tooling; 2: a tooling failure.
"""

import argparse
import collections
import ipaddress
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

STOCK_SCENARIOS = ("straight", "circle", "rise", "convoy", "driven", "gun", "brush", "coop", "coop-double")
# (Extinction's, run by name only: positive enclosed-rider coverage, not the stock gate)
SCENARIOS = STOCK_SCENARIOS + ("longsword",)
# Step 0's measured cockpit (Task 2): the rider's body-local spot and door walk, and the reachable pill-base volume
LONGSWORD_SPOT = (1.750, 0.270, -1.000, 0.000)
LONGSWORD_CABIN = ((-0.050, -0.500, -0.150), (5.250, 0.500, 0.500))
# Step 0's (Task 10): where protection starts for the Extinction rider (T), and the tilt cosine at which it was pushed
# out before the change
LONGSWORD_LIMIT = 0.70711
LONGSWORD_EJECTION_UK = 0.42598
SETTLE_FAILED = 6
# (vehicle_riding.h's enum vehicle_riding_refusal)
REFUSAL_MOTION = 4
REFUSALS = {1: "gone", 2: "moved-alone", 3: "sphere", 4: "motion"}
# Step 0's open-top deck spot (the negative run)
LONGSWORD_WING = (0.998, 3.000, 0.000, 0.000)
LONGSWORD_MAP = "custom_maps\\extinction"
EJECT_TICKS = 30
LONGSWORD_STARTS = (0, 300, 1200, 1500, 2400, 2700, 3000, 3300, 3600, 3900, 4200, 4500, 8100)
LONGSWORD_SETTLE = 60
LONGSWORD_END = 8460
REPORTED_ONLY = {"coop-double"}
# (network_test.c's ticks from the board tick)
RIDE_WALK, RIDE_JUMP, RIDE_JUMP_PERIOD, RIDE_JUMPS = 600, 840, 90, 10
RIDE_CROSS, RIDE_STEP_OFF, RIDE_OBSERVE, RIDE_END = 1740, 1920, 2040, 2400
PHASE_STARTS = (0, RIDE_WALK, RIDE_JUMP, RIDE_CROSS, RIDE_STEP_OFF, RIDE_OBSERVE)
PLACE_LIMIT = 0.05
HEIGHT_LIMIT = -0.1
SETTLE_TICKS = 30
RECOVERY_TIMEOUT = 180
RECOVERY_MAX, RECOVERY_SETTLE = 10, 6
DRIVEN_EXTRA = RECOVERY_MAX * (RECOVERY_TIMEOUT + RECOVERY_SETTLE)
COVERAGE_LIMIT = .5
LANDED_OFF_LIMIT = 2
TICKS_PER_SECOND = 30
PUSH_LIMIT = 0.05
ENCLOSED = 2
KINDS = {
    "support_failure": "tick phase",
    "launch": "tick player unit episode physical",
    "episode_end": "tick player unit episode terminal",
    "contact": "tick player vehicle node surface physical point normal",
    "board_refused": "tick unit vehicle ordinal candidate place ray clearance origin forward up",
    "prepare": "tick players vehicles driver driver_player board_tick driver_missing",
    "ready": "tick board_tick",
    "geometry": "tick gap",
    "phase": "tick player phase",
    "input": "tick player unit phase jump throttle",
    "sample": ("simulation_tick network_tick player unit local phase deck_present vehicle node ticks flags velocity "
               "peak world base place height_valid height body shield board_tick last_applied_host_tick "
               "origin forward left up"),
    "own": ("tick player unit phase deck_present vehicle node ticks flags velocity peak world base place "
            "origin forward left up board_tick"),
    "state": ("tick host_tick player unit predicted_valid predicted_low16 deck_present vehicle node ticks flags "
              "velocity peak world place"),
    "apply": "tick player unit applied_host_tick",
    "prediction": "host_tick player unit predicted_low16 eligible accepted",
    "correction": "tick player unit before after",
    "history": "tick player unit old_world new_world deck_present vehicle node ticks place velocity window air_episode",
    "grace": "tick player grace_ticks",
    "vehicle": "tick vehicle world velocity angular moved written writes carries driver driver_player",
    "reboard": "tick player unit landing_tick ticks",
    "recovery": "tick player unit reason start_tick recoveries",
    "life": ("tick player unit current_unit reason recorded damage_category damage_flags death_tick "
             "attacker_object attacker_player attacker_tick nearest_vehicle vehicle_speed vehicle_distance world"),
    "outcome": "tick player jump_presses jumps landings crossings stepped_off landed_off",
    "end": "tick players",
    "survey_vehicle": "tick vehicle tag radius m slope height width jump",
    "survey": "tick x y z room old_hit old_distance old_inside",
    "route": "tick height world old_hit old_distance ground_k heading",
    "tilt": "tick step hold door descending pitch roll tilt throttle facing grounded speed updates",
    "cabin": ("tick player unit pitch roll tilt uk m limit slope ticks airborne ground_k old_hit old_distance old_inside "
              "place body shield corrections axis raw inside ceiling ceiling_distance floor floor_distance standing "
              "misses jump take_off exception carry settle rejected anchor verdict refusal"),
    "control_inhibited": "tick phase player",
    "scripts_suspended": "tick",
    "error": "tick reason",
}
VECTOR_KEYS = {"world", "base", "place", "origin", "forward", "left", "up", "before", "after", "old_world", "new_world",
               "velocity", "angular", "point", "normal"}
FLOAT_KEYS = {"height", "peak", "body", "shield", "moved", "written", "gap", "radius", "m", "slope", "width", "upward",
              "x", "y", "z", "room", "old_distance", "target_pitch", "target_roll", "error", "pitch", "roll", "tilt",
                "uk", "limit", "ground_k", "speed", "heading", "parked_heading", "flight_heading",
               "ceiling_distance", "floor_distance", "vehicle_speed", "vehicle_distance"}
TEXT_KEYS = {"schema", "kind", "session", "scenario", "side", "phase", "reason", "vehicles", "tag"}


class ToolError(RuntimeError):
    pass


def phase_tick(scenario, tick):
    """Driven reserves the full bounded recovery budget before crossing."""
    return tick + (DRIVEN_EXTRA if scenario == "driven" and tick >= RIDE_CROSS else 0)


def phase_of(elapsed, scenario="straight"):
    for limit, name in ((0, "prepare"), (RIDE_WALK, "stand"), (RIDE_JUMP, "walk"),
                        (phase_tick(scenario, RIDE_CROSS), "jump"),
                        (phase_tick(scenario, RIDE_STEP_OFF), "cross"), (phase_tick(scenario, RIDE_OBSERVE), "step-off")):
        if elapsed < limit:
            return name
    return "observe"


def parse_log(path: Path) -> list:
    if not path.is_file():
        raise ToolError(f"missing log: {path}")
    result, seen = [], {}
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if "ride schema=" not in line:
            continue
        fields = {}
        for token in line[line.index("ride schema="):].split()[1:]:
            if "=" not in token:
                raise ToolError(f"malformed token at {path}:{number}")
            key, value = token.split("=", 1)
            if key in fields:
                raise ToolError(f"duplicate field {key} at {path}:{number}")
            fields[key] = value
        kind = fields.get("kind")
        if fields.get("schema") != "1" or kind not in KINDS:
            raise ToolError(f"unknown schema or kind at {path}:{number}")
        required = {"schema", "kind", "session", "scenario", "side", "seq"} | set(KINDS[kind].split())
        if not required <= fields.keys():
            raise ToolError(f"missing fields at {path}:{number}: {sorted(required - fields.keys())}")
        record = {}
        try:
            for key, value in fields.items():
                if key in VECTOR_KEYS or key in ("throttle", "facing"):
                    parsed = tuple(float(x) for x in value.split(","))
                    if len(parsed) != (2 if key in ("throttle", "facing") else 3) or not all(math.isfinite(x) for x in parsed):
                        raise ValueError(f"vector {key}")
                    record[key] = parsed
                elif key == "height" and value == "none" and fields.get("height_valid") == "0":
                    record[key] = None
                elif key in FLOAT_KEYS or (key == "jump" and kind == "survey_vehicle"):
                    record[key] = float(value)
                    if not math.isfinite(record[key]):
                        raise ValueError(f"scalar {key}")
                elif key in TEXT_KEYS:
                    record[key] = value
                else:
                    record[key] = int(value)
        except ValueError as error:
            raise ToolError(f"invalid value at {path}:{number}: {error}") from error
        identity = (record["session"], record["side"], record["seq"])
        if identity in seen:
            if seen[identity] != record:
                raise ToolError(f"conflicting duplicate record at {path}:{number}")
            continue
        seen[identity] = record
        result.append(record)
    if not result:
        raise ToolError(f"no ride records in {path}")
    return sorted(result, key=lambda r: r["seq"])


def unwrap_tick(low16: int, reference: int) -> int:
    if not 0 <= low16 <= 0xffff:
        raise ToolError("a predicted tick outside 16 bits")
    return reference - ((reference - low16) & 0xffff)


def local_point(world, frame):
    delta = tuple(world[i] - frame["origin"][i] for i in range(3))
    return tuple(sum(delta[i] * frame[axis][i] for i in range(3)) for axis in ("forward", "left", "up"))


def distance(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def claim(row):
    return (row["deck_present"], row["vehicle"], row["node"]) if row["deck_present"] else (0, -1, -1)


def align(host: list, client: list) -> list:
    """A client's own captures paired with the host state echoing their tick, history changes logged before that
    state applied; a remote rider's first sample after applying a host state paired with that state, which also names
    the host's state a step on (the sample is the applied state simulated once, so its world is that state's)."""
    authoritative = {(r["player"], r["unit"], r["host_tick"]): r for r in host if r["kind"] == "state"}
    # (a placement the host logged at tick N is in its state N + 1: that state is the result of tick N)
    placed = {(r["player"], r["unit"], r["tick"]) for r in host if r["kind"] == "recovery" and r["reason"] == "place"}
    own, held, scored, pairs, current = {}, {}, set(), [], 0
    for record in sorted(client, key=lambda r: r["seq"]):
        kind = record["kind"]
        current = max(current, record.get("network_tick", record.get("tick", current)))
        if kind == "own":
            own[(record["player"], record["unit"], record["tick"])] = dict(record)
        elif kind == "history":
            key = (record["player"], record["unit"], record["tick"])
            if key in own:
                captured = own[key]
                delta = tuple(record["new_world"][i] - captured["world"][i] for i in range(3))
                captured["world"] = record["new_world"]
                captured["base"] = tuple(captured["base"][i] + delta[i] for i in range(3))
                if record["deck_present"]:
                    captured["place"] = record["place"]
                    captured["velocity"] = record["velocity"]
                else:
                    captured["place"] = local_point(captured["base"], captured)
        elif kind == "state" and record["predicted_valid"]:
            predicted = unwrap_tick(record["predicted_low16"], current)
            key = (record["player"], record["unit"], predicted)
            if key in own:
                pairs.append({"mode": "own", "tick": predicted, "host_tick": record["host_tick"],
                              "player": record["player"], "host": dict(record), "client": dict(own[key])})
        elif kind == "apply":
            key = (record["player"], record["unit"])
            state = authoritative.get((key[0], key[1], record["applied_host_tick"]))
            if state is None:
                # (a state the host never logged leaves nothing held: an older one would pair a later sample with a
                # state the client had already moved past)
                held.pop(key, None)
                continue
            held[key] = (record["tick"], state)
        elif kind == "sample" and not record["local"]:
            key = (record["player"], record["unit"])
            if key in held:
                applied_tick, state = held[key]
                if record["network_tick"] >= applied_tick and (key, state["host_tick"]) not in scored:
                    scored.add((key, state["host_tick"]))
                    del held[key]
                    pairs.append({"mode": "remote", "tick": record["simulation_tick"], "host_tick": state["host_tick"],
                                  "player": record["player"], "host": dict(state), "client": dict(record),
                                  "apply_tick": applied_tick, "one_step": record["simulation_tick"] == applied_tick,
                                  "world_host": authoritative.get((key[0], key[1], state["host_tick"] + 1)),
                                  "placed": (key[0], key[1], state["host_tick"]) in placed})
    return pairs


def net_push(watched):
    """How far a rider ended up out along the hull's horizontal outward direction (the vehicle's origin to the rider at
    its first watched sample), or None where the rider stood on that origin."""
    first, last = watched[0], watched[-1]
    out = (first["world"][0] - first["origin"][0], first["world"][1] - first["origin"][1])
    length = math.hypot(*out)
    if length < 1e-6:
        return None
    return ((last["world"][0] - first["world"][0]) * out[0] + (last["world"][1] - first["world"][1]) * out[1]) / length


def judge_verdicts(pairs, records, fail):
    """The host's verdict and a client copy's, for every aligned pair that names one vehicle and is riding in both or in
    the air in both, agree once the claim grace has passed since the pair's last agreement or claim change."""
    grace = {r["player"]: r["grace_ticks"] for r in records if r["kind"] == "grace" and r["side"] == "client"}
    since = {}
    for pair in sorted(pairs, key=lambda p: (p["mode"], p["player"], p["tick"])):
        host_row, client_row = pair["host"], pair["client"]
        key = (pair["mode"], pair["player"])
        same_phase = (host_row["ticks"] == 60 and client_row["ticks"] == 60) or \
            (1 <= host_row["ticks"] <= 59 and 1 <= client_row["ticks"] <= 59)
        if not (host_row["deck_present"] and client_row["deck_present"] and
                host_row["vehicle"] == client_row["vehicle"] and same_phase) or \
                host_row["flags"] >> 1 == client_row["flags"] >> 1:
            since.pop(key, None)
            continue
        if key in since and since[key][1] != claim(host_row):
            since.pop(key)
        since.setdefault(key, (pair["tick"], claim(host_row)))
        limit = grace.get(pair["player"])
        if limit is None:
            fail(client_row, "missing-grace", None, "a grace", tool=True)
        elif pair["tick"] - since[key][0] + 1 > limit:
            fail(client_row, "verdict-disagreement", host_row["flags"] >> 1, client_row["flags"] >> 1)


def recovery_windows(records, samples, fail):
    """Only contact loss is exempt, on this machine, inside a bounded logged recovery."""
    active, counts, durations, intervals = {}, {}, {}, []
    by_tick = {(r["side"], r["player"], r["simulation_tick"]): r for r in samples}
    for row in sorted((r for r in records if r["kind"] == "recovery"), key=lambda r: (r["side"], r["player"], r["tick"], r["seq"])):
        key = (row["side"], row["player"])
        label = f"{key[0]}:{key[1]}"
        if row["scenario"] != "driven":
            fail(row, "recovery-scenario", row["scenario"], "driven")
            continue
        if row["reason"] == "start":
            if key in active or row["start_tick"] != row["tick"]:
                fail(row, "recovery-order", row["reason"], "one start")
                continue
            counts[label] = counts.get(label, 0) + 1
            if counts[label] > 10 or row["recoveries"] != counts[label]:
                fail(row, "recovery-limit", row["recoveries"], 10)
                continue
            active[key] = [row, False]
        elif key not in active or active[key][0]["tick"] != row["start_tick"]:
            fail(row, "recovery-order", row["reason"], "matching start")
        elif row["reason"] == "place":
            if active[key][1]:
                fail(row, "recovery-order", "duplicate place", "one place")
            active[key][1] = True
        elif row["reason"] in ("acquired", "failed"):
            start, placed = active.pop(key)
            age = row["tick"] - start["tick"]
            intervals.append((*key, start["tick"], min(row["tick"], start["tick"] + RECOVERY_TIMEOUT)))
            durations.setdefault(label, []).append(age)
            if age >= RECOVERY_TIMEOUT or row["reason"] == "failed":
                fail(row, "recovery-timeout", age, "less than 180")
            standing = [by_tick.get((*key, t)) for t in range(row["tick"] - 5, row["tick"] + 1)]
            if not placed or any(not s or s["ticks"] != 60 or not s["deck_present"] for s in standing) or \
                    len({s["vehicle"] for s in standing if s}) != 1:
                fail(row, "recovery-contact", None, "six supporting ticks on one vehicle")
        else:
            fail(row, "recovery-order", row["reason"], "start/place/acquired/failed")
    for key, (row, _) in active.items():
        fail(row, "recovery-incomplete", row["tick"], "bounded acquisition")
        intervals.append((*key, row["tick"], row["tick"] + RECOVERY_TIMEOUT))
    return intervals, counts, durations


def judge(pairs: list, records: list, rtt_ms: int, loss_percent: int) -> dict:
    scenario = records[0]["scenario"] if records else None
    cross, step_off, observe, scenario_end = (phase_tick(scenario, tick) for tick in
                                              (RIDE_CROSS, RIDE_STEP_OFF, RIDE_OBSERVE, RIDE_END))
    required = rtt_ms <= 100 and loss_percent == 0 and scenario not in REPORTED_ONLY
    reasons, tooling = [], []
    metrics = {"max_place_error": None, "min_height": None, "corrections": 0, "max_claim_disagreement": 0,
               "aligned": len(pairs), "correction_share": {}, "landed_off": {}, "reboard_ticks": {},
               "off_deck_share": {}, "alignment_coverage": {}}
    def fail(row, metric, value, limit, tool=False):
        (tooling if tool else reasons).append({"side": row.get("side"), "player": row.get("player"),
                                               "tick": row.get("simulation_tick", row.get("tick")),
                                               "metric": metric, "value": value, "limit": limit})

    # A rider that landed off the deck is put back on its spot by the machine scripting it (the host its own player, a
    # client its own): that machine's `reboard` records are the landings, in its clock. The other copy of the rider
    # must come to the deck too (checked at the window's end), and any exemption needs the host's copy off the deck
    # as well: a landing only a client saw is a landing disagreement, judged as any other. Windows are per player and
    # per jump cycle, and none spans a jump press.
    rtt_ticks = -(-rtt_ms * TICKS_PER_SECOND // 1000)
    boards = {side: next((r["board_tick"] for r in records if r["kind"] == "prepare" and r["side"] == side), None)
              for side in ("host", "client")}
    if all(board is not None for board in boards.values()) and boards["host"] != boards["client"]:
        fail({}, "board-clock-disagreement", boards, "one host-issued tick", tool=True)
    samples = [r for r in records if r["kind"] == "sample"]
    metrics["life_events"] = [r for r in records if r["kind"] == "life"]
    for life in metrics["life_events"]:
        fail(life, "rider-died", life["reason"], "alive")
    recoveries, metrics["recoveries"], metrics["recovery_ticks"] = recovery_windows(records, samples, fail)
    def recovering(row):
        return any(side == row["side"] and player == row["player"] and begin <= row["simulation_tick"] <= end
                   for side, player, begin, end in recoveries)
    owner = {r["player"]: r["side"] for r in samples if r["local"]}

    def windows(record):
        board = boards[record["side"]]
        if board is None:
            return None
        cycle = (record["tick"] - board - RIDE_JUMP) // RIDE_JUMP_PERIOD
        press = board + RIDE_JUMP + (cycle + 1) * RIDE_JUMP_PERIOD
        press_now = board + RIDE_JUMP + cycle * RIDE_JUMP_PERIOD
        start = max(record["landing_tick"], press_now)
        return cycle, start, press

    owner_reboards = {}
    for record in records:
        if record["kind"] == "reboard" and owner.get(record["player"]) == record["side"] and windows(record):
            owner_reboards[(record["player"], windows(record)[0])] = record
    echoes = []
    for (player, cycle), record in owner_reboards.items():
        _, begin, press = windows(record)
        start = record["tick"]
        end = min(start + rtt_ticks + SETTLE_TICKS, press - 1)
        echoes.append((player, cycle, start, end))
        metrics["landed_off"][player] = metrics["landed_off"].get(player, 0) + 1
        metrics["reboard_ticks"][player] = max(metrics["reboard_ticks"].get(player, 0), record["ticks"])
        # (the landing to the end of the echo window is not fully judged)
        metrics["off_deck_share"][player] = metrics["off_deck_share"].get(player, 0.0) + (end - begin) / (cross - RIDE_JUMP)

    def host_off(record):
        if record["side"] == "host":
            return True
        begin = windows(record)[1]
        return any(r["side"] == "host" and r["player"] == record["player"] and not r["deck_present"] and
                   begin <= r["simulation_tick"] <= record["tick"] + rtt_ticks for r in samples)

    def exempt(side, player, tick):
        """Off the deck from a landing to its re-board (in the other copy a round trip later), the host's copy off it
        too."""
        for (p, _), record in owner_reboards.items():
            if p != player:
                continue
            last = record["tick"] if side == record["side"] else record["tick"] + rtt_ticks
            if windows(record)[1] <= tick <= last and host_off(record):
                return True
        return False

    def echo(player, tick):
        """In the short span after a re-board while the host's copy and the client's come together (the host's
        clock for its start; the client's rows use their own tick, a round trip away)."""
        return any(p == player and start <= tick <= end for p, _, start, end in echoes)

    riders = 1 if scenario == "driven" else 2
    # a rider that died ended the case early: what was logged up to it is judged, the rest is not expected
    deaths = [r for r in records if r["kind"] == "error" and r["reason"] == "rider-life-changed"]
    deaths_by_side = {side: [r for r in deaths if r["side"] == side] for side in ("host", "client")}
    settled_deaths = set()

    def grace_of(side, player):
        return next((g["grace_ticks"] for g in records if g["kind"] == "grace" and g["side"] == side and
                     g["player"] == player), 0)

    def dead_rider(side, death):
        """The logger walks the riders in index order and stops at the first whose unit changed, so the dead one is
        the first whose samples ended before the death tick (those before it were sampled at it, those after it are
        not), and its last sample is the tick before. Anything else names nobody."""
        last = {}
        for row in samples:
            if row["side"] == side:
                last[row["player"]] = max(last.get(row["player"], row["simulation_tick"]), row["simulation_tick"])
        order = sorted(last)
        ended = [player for player in order if last[player] < death["tick"]]
        if not ended or any(last[player] >= death["tick"] for player in order[order.index(ended[0]):]):
            return None  # unattributed
        if death["tick"] - last[ended[0]] > max(1, grace_of(side, ended[0])):
            return None  # unattributed
        return ended[0]

    victims = {}
    for side in ("host", "client"):
        for death in deaths_by_side[side]:
            victims[id(death)] = dead_rider(side, death)
            if victims[id(death)] is None:
                settled_deaths.add(id(death))
                fail(death, "rider-died-unattributed", death["tick"], "a rider whose samples end there", tool=True)
    for host_death in deaths_by_side["host"]:
        player = victims[id(host_death)]
        if player is None:
            continue
        window = min(grace_of("host", player), grace_of("client", player))
        client_death = next((r for r in deaths_by_side["client"] if victims[id(r)] == player and
                             abs(r["tick"] - host_death["tick"]) <= window), None)
        if client_death is None:
            continue
        # (the dead rider's own samples before the death, not any living rider's)
        off_both = all(
            sample is not None and not sample["deck_present"] and death["tick"] - sample["simulation_tick"] <= window
            for sample, death in (
                (max((r for r in samples if r["side"] == side and r["player"] == player and
                      r["simulation_tick"] < death["tick"]), key=lambda r: r["simulation_tick"], default=None), death)
                for side, death in (("host", host_death), ("client", client_death))))
        if off_both:
            settled_deaths.update((id(host_death), id(client_death)))
            fail(host_death, "rider-died-agreed", host_death["tick"], "script keeps rider alive", tool=True)
    died = bool(deaths)
    for side in ("host", "client"):
        rows = [r for r in records if r["side"] == side]
        for kind in ("prepare", "end", "sample", "vehicle", "outcome"):
            if not any(r["kind"] == kind for r in rows) and not (kind == "outcome" and died):
                fail({"side": side}, "missing-" + kind, 0, 1, tool=True)
        for row in rows:
            if id(row) in settled_deaths:
                continue
            if row["kind"] == "error" and row["reason"] == "rider-life-changed":
                fail(row, "rider-died", row["tick"], "alive")
            elif row["kind"] == "error":
                fail(row, "error", row["reason"], "none", tool=True)
            elif row["kind"] == "support_failure":
                fail(row, "launch-unsupported", 0, "physical support before first launch")
            elif row["kind"] == "control_inhibited":
                fail(row, "control-inhibited", row["tick"], "delivered owner controls", tool=True)
        board = next((r["board_tick"] for r in rows if r["kind"] == "prepare"), None)
        players = {r["player"] for r in rows if r["kind"] == "sample"}
        if len(players) != riders:
            fail({"side": side}, "rider-count", len(players), riders, tool=True)
        for player in players:
            mine = [r for r in samples if r["side"] == side and r["player"] == player]
            if board is not None and not died and {r["simulation_tick"] for r in mine} != set(range(board, board + scenario_end + 1)):
                fail({"side": side, "player": player}, "sample-coverage", len(mine), scenario_end + 1, tool=True)
            phases = {r["phase"] for r in rows if r["kind"] == "phase" and r["player"] == player}
            if not died and not {"stand", "walk", "jump", "cross", "step-off", "observe"} <= phases:
                fail({"side": side, "player": player}, "phase-coverage", sorted(phases), "six", tool=True)
            if not died and any(r["local"] for r in mine):
                inputs = [r for r in rows if r["kind"] == "input" and r["player"] == player]
                # (a tick's input is asked, and logged, again and again while the tick stalls: a press counts once a tick)
                pressed = len({r["tick"] for r in inputs if r["jump"] and board is not None and
                               RIDE_JUMP <= r["tick"] - board < cross})
                if pressed != RIDE_JUMPS:
                    fail({"side": side, "player": player}, "jump-inputs", pressed, RIDE_JUMPS, tool=True)
    if len({(r["session"], r["scenario"]) for r in records}) > 1:
        fail({}, "session-mismatch", None, "one", tool=True)
    # heights, every machine, every sample from standing to crossing
    for row in samples:
        if row["phase"] not in {"stand", "walk", "jump", "cross"}:
            continue
        if not row["height_valid"]:
            if row["phase"] != "jump" and not exempt(row["side"], row["player"], row["simulation_tick"]) and not recovering(row):
                fail(row, "contact-lost", None, "a surface")
            continue
        metrics["min_height"] = row["height"] if metrics["min_height"] is None else min(metrics["min_height"], row["height"])
        if row["height"] < HEIGHT_LIMIT and not exempt(row["side"], row["player"], row["simulation_tick"]):
            fail(row, "height", row["height"], HEIGHT_LIMIT)
    # corrections on clients, per player
    corrections = collections.defaultdict(list)
    echoed_corrections = collections.Counter()
    for row in sorted(records, key=lambda r: (r["side"], r["seq"])):
        if row["kind"] != "correction":
            continue
        identity = (row["side"], row["player"], row["unit"])
        tick = row["tick"]
        corrections[identity].append(tick)
        metrics["corrections"] += 1
        sample = next((s for s in reversed(samples) if s["side"] == row["side"] and s["player"] == row["player"] and
                       s["network_tick"] <= tick), None)
        if sample is None:
            fail(row, "correction-without-sample", None, "a sample", tool=True)
            continue
        if echo(row["player"], tick):
            cycle = next(c for p, c, a, b in echoes if p == row["player"] and a <= tick <= b)
            echoed_corrections[(row["player"], cycle)] += 1
            if echoed_corrections[(row["player"], cycle)] > 1:
                fail(row, "reboard-corrections", echoed_corrections[(row["player"], cycle)], 1)
            continue
        settled = sample["board_tick"] is not None and sample["board_tick"] >= 0 and \
            sample["simulation_tick"] >= sample["board_tick"] + SETTLE_TICKS
        moving = {"walk", "jump"} | ({"observe"} if scenario == "brush" else set())
        if sample["phase"] == "stand" and settled:
            fail(row, "standing-correction", 1, 0)
        elif sample["phase"] in moving:
            count = sum(tick - 60 < earlier <= tick for earlier in corrections[identity])
            if count > 1:
                fail(row, "corrections-in-two-seconds", count, 1)
    # the claim, against the last host state each client applied, within the logged grace
    grace, held_claim, since = {}, {}, {}
    for row in sorted(records, key=lambda r: (r["side"], r["seq"])):
        key = (row["side"], row.get("player"))
        if row["kind"] == "grace":
            grace[key] = row["grace_ticks"]
        elif row["kind"] == "state" and row["side"] == "client":
            held_claim[key] = claim(row)
        elif row["kind"] == "sample" and row["side"] == "client" and key in held_claim:
            phase_start = {"step-off": step_off, "observe": observe}.get(row["phase"])
            phase_age = row["simulation_tick"] - (row["board_tick"] + phase_start) if phase_start is not None else 0
            allowance = grace.get((row["side"], row["player"]))
            if claim(row) == held_claim[key] or row["phase"] == "prepare" or \
                    (phase_start is not None and allowance is not None and phase_age < allowance):
                since.pop(key, None)
                continue
            if echo(row["player"], row["simulation_tick"]) and key not in since:
                continue
            since.setdefault(key, row["simulation_tick"])
            elapsed = row["simulation_tick"] - since[key] + 1
            metrics["max_claim_disagreement"] = max(metrics["max_claim_disagreement"], elapsed)
            allowance = grace.get((row["side"], row["player"]))
            if allowance is None:
                fail(row, "missing-grace", None, "a grace", tool=True)
            elif elapsed > allowance:
                fail(row, "claim-disagreement", elapsed, allowance)
    # places, once settled
    for pair in pairs:
        host_row, client_row = pair["host"], pair["client"]
        board = client_row.get("board_tick")
        if board is None or board < 0 or pair["tick"] < board + SETTLE_TICKS:
            continue
        if echo(client_row["player"], pair["tick"]):
            continue
        # (a pair claiming different decks has no deck to compare places on: the world decides)
        riding = claim(host_row) == claim(client_row) and host_row["deck_present"] and client_row["deck_present"] and \
            host_row["ticks"] == 60 and client_row["ticks"] == 60
        reference = host_row
        if pair["mode"] == "remote" and not riding and not pair["placed"]:
            # (the client applies a host state and then steps its unit once before the sample, so the sample is the
            # host's world one tick on from the applied state, not the applied state's own. Not where the host placed
            # the rider: that jump is no step of the client's, so the pair stays on the applied state)
            if not pair["one_step"]:
                fail(client_row, "world-step-ambiguous",
                     f"applied host tick {pair['host_tick']}, apply tick {pair['apply_tick']}, "
                     f"sample tick {client_row['simulation_tick']}, unit {client_row['unit']}", "a sample one tick after its apply",
                     tool=True)
                continue
            if pair["world_host"] is None:
                fail(client_row, "missing-aligned-host-state",
                     f"host tick {pair['host_tick'] + 1} (after {pair['host_tick']}) of unit {client_row['unit']}",
                     "a host state of the same player and unit", tool=True)
                continue
            reference = pair["world_host"]
        error = distance(reference["place"] if riding else reference["world"],
                         client_row["place"] if riding else client_row["world"])
        metrics["max_place_error"] = error if metrics["max_place_error"] is None else max(metrics["max_place_error"], error)
        if error > PLACE_LIMIT:
            fail(client_row, "place" if riding else "world", error, PLACE_LIMIT)
    judge_verdicts(pairs, records, fail)
    # a client's own player: every scored phase aligned, or its refusals on the host shown
    for player in {r["player"] for r in samples if r["side"] == "client" and r["local"] and not died}:
        for phase in ("stand", "walk", "jump"):
            # (a capture is joined to the sample of its tick, as the remote rule joins a state to its sample: a capture
            # no sample answers is no pairing, and a sample logged twice is its first)
            first_samples = {}
            for r in sorted((r for r in samples if r["side"] == "client" and r["local"] and r["player"] == player),
                            key=lambda r: r["seq"]):
                first_samples.setdefault((r["unit"], r["simulation_tick"]), r)
            own_samples = [r for r in first_samples.values()
                           if r["phase"] == phase and r["simulation_tick"] >= (r["board_tick"] or 0) + SETTLE_TICKS]
            own_captures = {(p["player"], p["client"]["unit"], p["client"]["tick"]) for p in pairs if p["mode"] == "own"}
            paired = [r for r in own_samples if (player, r["unit"], r["simulation_tick"]) in own_captures]
            if not any(p["mode"] == "own" and p["player"] == player and p["client"]["phase"] == phase for p in pairs):
                ticks = {r["network_tick"] for r in samples if r["side"] == "host" and r["player"] == player and
                         r["phase"] == phase}
                refused = [r for r in records if r["kind"] == "prediction" and r["player"] == player and
                           r["host_tick"] in ticks and r["eligible"] and not r["accepted"]]
                if refused:
                    fail(refused[0], "prediction-refused", len(refused), 0)
                else:
                    fail({"side": "client", "player": player}, "no-echo-" + phase, 0, 1, tool=True)
            share = len(paired) / len(own_samples) if own_samples else 0.0
            metrics["alignment_coverage"][f"own:{player}:{phase}"] = share
            if share < COVERAGE_LIMIT:
                fail({"side": "client", "player": player}, "alignment-coverage", share, COVERAGE_LIMIT, tool=True)
    # every applied host state must be present in the host's log
    authoritative = {(r["player"], r["unit"], r["host_tick"]) for r in records
                     if r["kind"] == "state" and r["side"] == "host"}
    for row in records:
        if row["kind"] == "apply" and row["side"] == "client":
            if (row["player"], row["unit"], row["applied_host_tick"]) not in authoritative:
                fail(row, "missing-applied-host-state", row["applied_host_tick"], "a host state", tool=True)
    # remote riders need alignment in every phase scored for own riders too
    remote_riders = {(r["player"], r["unit"]) for r in samples if r["side"] == "client" and not r["local"] and not died}
    for player, unit in remote_riders:
        for phase in ("stand", "walk", "jump"):
            remote_samples = [r for r in samples if r["side"] == "client" and not r["local"] and r["player"] == player and
                              r["unit"] == unit and r["phase"] == phase and
                              r["simulation_tick"] >= (r["board_tick"] or 0) + SETTLE_TICKS]
            settled_remote = {r["seq"] for r in remote_samples}
            remote_pairs = [p for p in pairs if p["mode"] == "remote" and p["player"] == player and
                            p["client"]["unit"] == unit and p["client"]["phase"] == phase and
                            p["client"]["seq"] in settled_remote]
            share = len(remote_pairs) / len(remote_samples) if remote_samples else 0.0
            metrics["alignment_coverage"][f"remote:{player}:{phase}"] = share
            if share < COVERAGE_LIMIT:
                fail({"side": "client", "player": player}, "alignment-coverage", share, COVERAGE_LIMIT, tool=True)
    # a rider put back must be riding the same vehicle and node in both copies when its echo window ends
    for player, cycle, start, end in echoes:
        hs = next((r for r in samples if r["side"] == "host" and r["player"] == player and r["simulation_tick"] == end), None)
        cs = next((r for r in samples if r["side"] == "client" and r["player"] == player and r["simulation_tick"] == end), None)
        if not (hs and cs and hs["deck_present"] and cs["deck_present"] and hs["vehicle"] == cs["vehicle"] and
                hs["node"] == cs["node"] and claim(hs) == claim(cs)):
            fail(cs or hs or {"side": "client", "player": player}, "reboard-not-taken", end, "riding together")
    # coverage: a rider landing off the deck more than a few of its jumps is a case that does not test the deck
    for player, count in metrics["landed_off"].items():
        if count > LANDED_OFF_LIMIT:
            fail({"side": "host", "player": player}, "landed-off-floor", count, LANDED_OFF_LIMIT)
    # the physical outcome
    if scenario == "convoy":
        metrics["crossings"] = {}
    for outcome in [r for r in records if r["kind"] == "outcome"]:
        if scenario == "convoy":
            # Crossing is report-only. Confirm its phase from physical contact,
            # not an outcome total that older logs may include jump round trips in.
            previous, cross_phase_count = None, 0
            for row in sorted((r for r in samples if r["side"] == outcome["side"] and
                               r["player"] == outcome["player"]), key=lambda r: r["simulation_tick"]):
                if not row["deck_present"] or row["ticks"] != 60:
                    continue
                if previous is not None and previous != row["vehicle"] and row["phase"] == "cross":
                    cross_phase_count += 1
                previous = row["vehicle"]
            metrics["crossings"][f'{outcome["side"]}:{outcome["player"]}'] = {
                "count": outcome["crossings"], "cross_phase_count": cross_phase_count,
                "cross_phase": cross_phase_count > 0}
        if scenario == "driven":
            # A teleport's acquisition is separate from landing on the moving deck.
            completed = sum(r["kind"] == "recovery" and r["side"] == outcome["side"] and
                            r["player"] == outcome["player"] and r["reason"] == "acquired" and
                            r["tick"] <= outcome["tick"] for r in records)
            completed = outcome.get("episode_recoveries", completed)
            accounted = outcome["landings"] + completed
            metrics.setdefault("driven_outcomes", {})[f'{outcome["side"]}:{outcome["player"]}'] = {
                "jumps": outcome["jumps"], "deck_landings": outcome["landings"], "recoveries": completed,
                "accounted_jumps": accounted, "landed_off": outcome["landed_off"]}
            if owner.get(outcome["player"]) == outcome["side"]:
                metrics["landed_off"][outcome["player"]] = outcome["landed_off"]
            if outcome["landed_off"] > LANDED_OFF_LIMIT:
                fail(outcome, "landed-off-floor", outcome["landed_off"], LANDED_OFF_LIMIT)
            if (outcome["jumps"] < RIDE_JUMPS or
                    outcome["landings"] < 1 or accounted < outcome["jumps"] or
                    not outcome["stepped_off"]):
                fail(outcome, "outcome", {"jumps": outcome["jumps"], "deck_landings": outcome["landings"],
                                        "recoveries": completed, "stepped_off": outcome["stepped_off"]},
                     "every jump landed or recovered, at least one deck landing")
        elif outcome["jumps"] < RIDE_JUMPS or outcome["landings"] < RIDE_JUMPS or not outcome["stepped_off"]:
            fail(outcome, "outcome", {k: outcome[k] for k in ("jumps", "landings", "crossings", "stepped_off")},
                 "complete")
    # the scenarios' own rules
    if scenario == "gun":
        for side in ("host", "client"):
            verdicts = [r for r in samples if r["side"] == side and r["phase"] == "jump" and r["deck_present"] and
                        1 <= r["ticks"] <= 59 and (r["flags"] >> 1)]
            if not verdicts and not died:
                fail({"side": side}, "gun-no-verdict", 0, 1, tool=True)
            for row in verdicts:
                verdict = row["flags"] >> 1
                if verdict == ENCLOSED:
                    fail(row, "gun-enclosed", verdict, 1)
    # the owner's premise: no stock vehicle is enclosed, so no stock rider standing on one has an enclosed verdict
    # (riding rows only: the air rows carry the legacy single-ray verdict, which the gun rule judges and which is no
    # cabin evidence; a protected jump's raw sample needs a vehicle tilted past 45 degrees, which no stock scenario does)
    if scenario in STOCK_SCENARIOS:
        for row in samples:
            if row["deck_present"] and row["ticks"] == 60 and (row["flags"] >> 1) == ENCLOSED:
                fail(row, "stock-enclosed", row["flags"] >> 1, "never")
    if scenario == "brush":
        for player in {r["player"] for r in samples}:
            pushed_sides, moved_sides, directionless = set(), set(), False
            for side in ("host", "client"):
                watched = sorted((r for r in samples if r["side"] == side and r["player"] == player and
                                  r["phase"] == "observe"), key=lambda r: r["simulation_tick"])
                if not watched:
                    continue
                # (the rider's net travel out of the hull: jitter in random directions is motion, not a push)
                outward = net_push(watched)
                if outward is None:
                    directionless = True
                    fail({"side": side, "player": player}, "brush-no-direction", 0, 1, tool=True)
                    continue
                pushed = outward > PUSH_LIMIT
                if pushed:
                    pushed_sides.add(side)
                if max(distance(watched[0]["world"], r["world"]) for r in watched) > PUSH_LIMIT:
                    moved_sides.add(side)
            if pushed_sides == {"host", "client"}:
                continue
            if pushed_sides:
                fail({"player": player}, "brush-one-side", sorted(pushed_sides), "both")
            elif moved_sides:
                for side in sorted(moved_sides):
                    fail({"side": side, "player": player}, "brush-no-push", 0, PUSH_LIMIT)
            elif not died and not directionless:
                fail({"player": player}, "brush-no-touch", 0, 1, tool=True)
    if scenario == "driven":
        firsts = {}
        for row in samples:
            key = (row["side"], row["player"])
            first = firsts.setdefault(key, (row["body"], row["shield"]))
            if row["body"] < first[0] or row["shield"] < first[1]:
                fail(row, "rider-hurt", (row["body"], row["shield"]), first)
    # Co-op's AI seat and per-tick carry path are netcode coverage, not setup failures on the client.
    if scenario in ("coop", "coop-double"):
        for row in records:
            if row["kind"] == "prepare" and row["side"] == "client" and row["driver_missing"]:
                fail(row, "driver-seat-missing", row["driver"], "an AI driver")
        host_vehicles = {(r["tick"], r["vehicle"]): r for r in records
                         if r["kind"] == "vehicle" and r["side"] == "host"}
        moving_ticks, carried = 0, 0
        for row in records:
            if row["kind"] != "vehicle" or row["side"] != "client":
                continue
            host_row = host_vehicles.get((row["tick"], row["vehicle"]))
            previous = host_vehicles.get((row["tick"] - 1, row["vehicle"]))
            # (a vehicle moving on this tick only has no tick before it to carry on from)
            if host_row and host_row["moved"] > 0 and previous and previous["moved"] > 0:
                moving_ticks += 1
                carried += row["carries"] > 0
                if row["carries"] == 0:
                    fail(row, "no-extrapolation", row["carries"], "a carry on every moving host tick")
        if moving_ticks and not carried:
            fail({"side": "client"}, "carry-never-ran", 0, 1, tool=True)
    # reported: the part of each vehicle's motion the netcode wrote
    totals = collections.defaultdict(lambda: [0.0, 0.0])
    for row in records:
        if row["kind"] == "vehicle":
            totals[f'{row["side"]}:{row["vehicle"]}'][0] += row["written"]
            totals[f'{row["side"]}:{row["vehicle"]}'][1] += row["moved"]
    metrics["correction_share"] = {key: (written / moved if moved > 0 else 0.0) for key, (written, moved) in totals.items()}
    if not pairs and not died and not any(r["metric"] == "prediction-refused" for r in reasons):
        fail({}, "no-alignment", 0, 1, tool=True)
    return {"status": "TOOL_ERROR" if tooling else "DESYNC" if reasons else "PASS", "required": required,
            "reasons": tooling + reasons, "metrics": metrics}


def inside_box(place, box):
    low, high = box
    return all(low[i] <= place[i] <= high[i] for i in range(3))


def judge_longsword(records, rtt_ms, loss_percent, box, negative=False, local=False, corner=False):
    """The Longsword run (Step 0 and acceptance), each machine on its own: its rider never ejected (its pill's base
    outside the reachable cockpit volume, or off the deck without one, for more than EJECT_TICKS before the door
    walk), alive, out of the door and off the deck at the end if the host landed before the door tick;
    otherwise the run is a tooling error. The host's programme must reach past 45 degrees before
    descent through counted updates. Health lost is reported (Task 10 fails failed-recovery damage). Place agreement is reported only
    (it needs stage 2 phase (b)'s authority); the aligned pairs' verdicts must agree (judge_verdicts)."""
    reasons, tooling = [], []
    metrics = {"first_outside": {}, "max_outside_streak": {}, "min_uk": None, "max_total_tilt": 0.0,
               "max_total_tilt_by_side": {}, "ejection": {},
               "health_lost": {}, "died_in_door_walk": [], "preparation": {}, "landing": {}}
    health = metrics["health_lost"]
    host_board = next((r["board_tick"] for r in records if r["side"] == "host" and r["kind"] == "prepare"), 0)
    # (the game starts the walk when the ship has stood 30 ticks, whenever that comes after touchdown: a single tick's
    # counter chatters while the ship rocks, so the window runs from the tick before the door step to its walk's start)
    host_door = host_board + LONGSWORD_STARTS[-1]
    host_landing = next((r for r in sorted((r for r in records if r["side"] == "host" and r["kind"] == "tilt"),
                                           key=lambda r: r["tick"])
                         if host_door - 1 <= r["tick"] <= host_door + 59 and r["grounded"] >= 30 and r["speed"] < .03 and
                         r["tilt"] < 5.0), None)
    door_judged = host_landing is not None
    metrics["door_walk"] = "judged" if door_judged else "door-not-run"

    def fail(row, metric, value, limit, tool=False):
        (tooling if tool else reasons).append({"side": row.get("side"), "player": row.get("player"),
                                               "tick": row.get("tick"), "metric": metric, "value": value,
                                               "limit": limit})

    if not door_judged:
        fail(host_landing or {"side": "host"}, "door-not-run", metrics["door_walk"], "landed before door walk", tool=True)

    for side in (("host",) if local else ("host", "client")):
        rows = [r for r in records if r["side"] == side]
        for kind in ("prepare", "end", "tilt", "cabin"):
            if not any(r["kind"] == kind for r in rows):
                fail({"side": side}, "missing-" + kind, 0, 1, tool=True)
        tilts = {r["tick"]: r for r in rows if r["kind"] == "tilt"}
        boards = [r["board_tick"] for r in rows if r["kind"] == "prepare"]
        board = boards[0] if boards else 0
        tilt_start = board + LONGSWORD_STARTS[5]
        door_start = board + LONGSWORD_STARTS[-1]
        for row in tilts.values():
            elapsed = row["tick"] - board
            step = max((i for i, start in enumerate(LONGSWORD_STARTS) if elapsed >= start), default=0)
            if (row["step"], row["door"], row["descending"]) != (step, int(step == 12), int(step == 11)):
                fail(row, "longsword-phase-skew", (row["step"], row["door"], row["descending"]), step, tool=True)
                break
        landing = tilts.get(door_start - 1)
        if landing and landing["grounded"] >= 30 and landing["speed"] < .03 and landing["tilt"] < 5.0:
            metrics["landing"][side] = {k: landing[k] for k in ("tick", "grounded", "speed", "tilt")}
        for row in rows:
            if row["kind"] == "error" and row["reason"] == "rider-life-changed":
                # (out of the door at the ground a step down may kill; anywhere else a death is an ejection's fall)
                walking = tilts.get(row["tick"], tilts.get(max(tilts) if tilts else None, {})).get("door")
                if walking or negative:
                    metrics["died_in_door_walk"].append(f"{side}:{row['tick']}")
                else:
                    fail(row, "rider-died", row["tick"], "alive")
            elif row["kind"] == "error":
                fail(row, "error", row["reason"], "none", tool=True)
        failures = metrics.setdefault("recovery_failures", {})
        metrics.setdefault("protected_ticks", {})
        previous = None
        maximum = 0.0
        for row in sorted(tilts.values(), key=lambda r: r["tick"]):
            if row["step"] in (5, 7, 9) and not row["door"] and not row["descending"]:
                maximum = max(maximum, row["tilt"])
            if side == "host" and previous is not None:
                if row["updates"] <= previous:
                    fail(row, "uncounted-update", row["updates"], previous, tool=True)
            previous = row["updates"]
        metrics["max_total_tilt_by_side"][side] = maximum
        if side == "host":
            metrics["max_total_tilt"] = maximum
            if maximum <= 45.0:
                fail({"side": side}, "tilt-not-reached", maximum, ">45 degrees", tool=True)
        for player in sorted({r["player"] for r in records if r["kind"] == "cabin"}):
            mine = sorted((r for r in rows if r["kind"] == "cabin" and r["player"] == player), key=lambda r: r["tick"])
            key = f"{side}:{player}"
            by_tick = {r["tick"]: r for r in mine}
            # (missing observations must not conceal an escape; death and door ticks end the observed span)
            deaths = [r["tick"] for r in rows if r["kind"] == "error" and r["reason"] == "rider-life-changed"]
            coverage_end = min([max(tilts, default=tilt_start - 1) + 1, door_start] + deaths)
            missing = sorted(set(range(tilt_start, coverage_end)) - by_tick.keys())
            if missing:
                fail({"side": side, "player": player, "tick": tilt_start}, "cabin-coverage", len(missing), missing[0], tool=True)
            if not mine:
                continue
            first = (mine[0]["body"], mine[0]["shield"])
            preparation = {}
            for name, end in (("ground", board + LONGSWORD_STARTS[1]), ("pretilt", tilt_start)):
                streak = 0
                for tick in range(end - LONGSWORD_SETTLE, end):
                    row = by_tick.get(tick)
                    if row and row["ticks"] == 60 and not row["airborne"] and (negative or box is None or inside_box(row["place"], box)):
                        streak += 1
                    else:
                        streak = 0
                preparation[name] = {"standing_ticks": streak, "last_place": by_tick.get(end - 1, {}).get("place")}
            metrics["preparation"][key] = preparation
            prepared = all(p["standing_ticks"] >= LONGSWORD_SETTLE for p in preparation.values())
            if not prepared:
                fail({"side": side, "player": player, "tick": tilt_start}, "longsword-preparation",
                     preparation, "60 consecutive standing inside ticks before climb and tilt", tool=True)
            streak, longest, ejected, last, went_out = 0, 0, False, mine[-1], False
            last_failed = None
            recovery_hurt = False
            previous_health = first[0] + first[1]
            previous_row = None
            for row in mine:
                if previous_row is not None:
                    # (an enclosed rider the deck carried keeps it with no touch: it leaves only by a jump or the door)
                    if (previous_row["ticks"] == 60 and previous_row["exception"] and not previous_row["take_off"] and
                            row["ticks"] < 60 and not tilts.get(row["tick"], {}).get("door")):
                        fail(row, "hold-lost", row["tick"], "held")
                    if previous_row["uk"] >= row["limit"] > row["uk"]:
                        metrics.setdefault("crossing_rows", {}).setdefault(key, []).append({
                            "tick": row["tick"], "standing": previous_row["standing"], "raw": row["raw"],
                            "exception": row["exception"], "ticks": row["ticks"], "place": row["place"]})
                previous_row = row
                metrics["min_uk"] = row["uk"] if metrics["min_uk"] is None else min(metrics["min_uk"], row["uk"])
                if LONGSWORD_LIMIT is not None and abs(row["limit"] - LONGSWORD_LIMIT) > 1e-4:
                    fail(row, "limit-mismatch", row["limit"], LONGSWORD_LIMIT, tool=True)
                # (the snapshot guard proper, the vehicle's motion not its counted updates, is tooling: the test's
                # turning is broken; a correction or a recovery moving the rider, a rider leaving the sphere, the
                # vehicle gone are events of play, reported)
                if row["refusal"] == REFUSAL_MOTION and not tilts.get(row["tick"], {}).get("door"):
                    fail(row, "snapshot-guard", row["tick"], "none", tool=True)
                elif row["refusal"]:
                    counts = metrics.setdefault("refusals", {}).setdefault(key, {})
                    name = REFUSALS.get(row["refusal"], str(row["refusal"]))
                    counts[name] = counts.get(name, 0) + 1
                if row["settle"] == SETTLE_FAILED:
                    failures[key] = failures.get(key, 0) + 1
                    last_failed = row["tick"]
                    if corner and not row["anchor"] and (row["raw"] or row["standing"] == ENCLOSED):
                        fail(row, "no-anchor", row["tick"], "anchor while enclosed")
                if row["exception"]:
                    metrics["protected_ticks"][key] = metrics["protected_ticks"].get(key, 0) + 1
                if negative and (row["standing"] == ENCLOSED or row["jump"] == ENCLOSED or
                                 row["verdict"] == ENCLOSED or row["exception"] or row["anchor"]):
                    # (an open deck never has cabin evidence: the owner's premise, live)
                    fail(row, "negative-enclosed", row["tick"], "open")
                outside = not inside_box(row["place"], box) if box else row["ticks"] == 0
                if row["body"] + row["shield"] < first[0] + first[1]:
                    health[key] = first[0] + first[1] - row["body"] - row["shield"]
                # (only a new drop can follow a failed recovery; earlier damage stays a reported total)
                if (row["body"] + row["shield"] < previous_health and
                        last_failed is not None and row["tick"] - last_failed <= EJECT_TICKS and not recovery_hurt):
                    recovery_hurt = True
                    fail(row, "failed-recovery-damage", row["body"] + row["shield"], first)
                previous_health = row["body"] + row["shield"]
                if negative:
                    continue
                if not prepared or row["tick"] < tilt_start:
                    streak = 0
                    continue
                if tilts.get(row["tick"], {}).get("door"):
                    went_out = went_out or outside
                    continue
                if outside:
                    metrics["first_outside"].setdefault(key, row["tick"])
                    streak += 1
                    longest = max(longest, streak)
                    if streak > EJECT_TICKS and not ejected:
                        ejected = True
                        metrics["ejection"][key] = {k: row[k] for k in (
                            "tick", "pitch", "roll", "uk", "old_hit", "old_distance", "old_inside", "ticks")}
                        if last_failed is not None and row["tick"] - last_failed <= streak + EJECT_TICKS:
                            # (the owner's accepted risk: a blocked recovery leaves the rider where it was; reported
                            # as itself, never passed and never taken for an unrecovered escape)
                            fail(row, "blocked-recovery-outside", row["tick"], "inside")
                        else:
                            fail(row, "ejected", row["tick"], "inside")
                else:
                    streak = 0
            metrics["max_outside_streak"][key] = longest
            if LONGSWORD_EJECTION_UK is not None:
                metrics.setdefault("below_ejection_uk", {})[key] = sum(r["uk"] <= LONGSWORD_EJECTION_UK for r in mine)
            # (the door walk: out of the cockpit, and its window left: the deck no longer holds it)
            if not negative and door_judged and tilts.get(last["tick"], {}).get("door") and not (went_out and last["ticks"] < 60):
                fail(last, "door-blocked", last["tick"], "out and off the deck")
    if not local:
        ride_pairs = align([r for r in records if r["side"] == "host"], [r for r in records if r["side"] == "client"])
        judge_verdicts(ride_pairs, records, fail)
    return {"status": "TOOL_ERROR" if tooling else "DESYNC" if reasons else "PASS", "required": False,
            "reasons": tooling + reasons, "metrics": metrics}


def case_seconds(scenario):
    """Keep the launcher's existing margin when driven's simulation grows."""
    return (420 if scenario == "longsword" else 360 if scenario in ("coop", "coop-double") else
            180 + (DRIVEN_EXTRA + TICKS_PER_SECOND - 1) // TICKS_PER_SECOND if scenario == "driven" else 180)


def child_environment(side, data, output, scenario, rtt_ms, loss_percent, host_address, client_address, driver,
                      visible=False, spot="", local=False, corner=False):
    env = {key: value for key, value in os.environ.items() if not key.startswith("HALO_")}
    campaign = scenario in ("coop", "coop-double")
    env.update({
        "HALO_DATA_ROOT": str(data), "HALO_SAVE_ROOT": str(output / side / "saves"),
        "HALO_NET_ADDRESS": host_address if side == "host" else client_address,
        "HALO_NET_BROADCAST": client_address if side == "host" else host_address,
        "HALO_NETWORK_LATENCY": str(rtt_ms / 2), "HALO_NETWORK_LOSS": str(loss_percent),
        "HALO_NETWORK_TEST": ("host:levels\\a30\\a30:coop" if campaign else
                              f"host:{LONGSWORD_MAP}:slayer" if scenario == "longsword" else "host:bloodgulch:slayer")
        if side == "host" else "join",
        "HALO_NETWORK_TEST_RIDE": scenario, "HALO_NETWORK_TEST_RIDE_DRIVER": driver,
        "HALO_NETWORK_TEST_RIDE_SESSION": output.name, "HALO_NETWORK_TEST_RIDE_SPOT": spot,
        "HALO_NETWORK_TEST_RIDE_CLOCK": "d:\\ride-clock\\board.txt",
        "HALO_NETWORK_TEST_START": "20", "HALO_NETWORK_TEST_KILL": "0", "HALO_NETWORK_TEST_SHOOT": "0",
        "HALO_NETWORK_TEST_VEHICLE": "0", "HALO_NETWORK_TEST_PICKUP": "0", "HALO_NETWORK_TEST_SCORE": "0",
        "HALO_NETWORK_TEST_PUBLIC": "0", "HALO_TEST_INPUT": "",
        # (the host's switch decides; the client's own setting is off to show it does not)
        "HALO_VEHICLE_RIDING": "true" if side == "host" else "false",
        "HALO_HIDDEN_WINDOW": "1",
        "HALO_EXIT_AFTER": str(case_seconds(scenario)), "HALO_NET_ONLINE": "false",
        "HALO_TELNET_CONSOLE": "0", "HALO_UPDATE_ANSWER": "never",
    })
    if local:
        env["HALO_NETWORK_TEST"] = f"local:{LONGSWORD_MAP}:slayer"
        for key in ("HALO_NET_ADDRESS", "HALO_NET_BROADCAST", "HALO_NETWORK_LATENCY", "HALO_NETWORK_LOSS",
                    "HALO_NETWORK_TEST_RIDE_CLOCK"):
            del env[key]
    if corner:
        env["HALO_NETWORK_TEST_RIDE_CORNER"] = "true"
    # (any value hides the window, so a watched run has none)
    if visible:
        del env["HALO_HIDDEN_WINDOW"]
    return env


def make_side(halo: Path, data_root: Path, output: Path, side: str, visible=False, custom_maps=False):
    root = output / side
    runtime, data = root / "runtime", root / "data"
    for folder in (runtime, data, root / "saves"):
        folder.mkdir(parents=True)
    executable = runtime / "halo.exe"
    shutil.copy2(halo, executable)
    for dll in halo.parent.glob("*.dll"):
        shutil.copy2(dll, runtime / dll.name)
    maps = data_root / "maps"
    if not maps.is_dir():
        raise ToolError(f"missing maps: {maps}")
    # Game stdio translates Xbox paths; expose the shared clock under each data root.
    clock = output / "clock"
    clock.mkdir(parents=True, exist_ok=True)
    links = [(maps, data / "maps"), (clock, data / "ride-clock")]
    if custom_maps:
        custom = data_root / "custom_maps"
        if not custom.is_dir():
            raise ToolError(f"missing custom_maps: {custom}")
        links.append((custom, data / "custom_maps"))
    for source, destination in links:
        if any(c in str(path) for path in (source, destination) for c in '\r\n"&|<>^%'):
            raise ToolError("a path the junction command cannot take")
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(destination), str(source)], check=True,
                       capture_output=True, text=True)
    # A watched run opens two small windows; leave their input alone (a focused window takes it).
    config = '[display]\nmode = "windowed"\nwindow_scale = 1\n' if visible else '[debug]\nhidden_window = true\n'
    (runtime / "config.toml").write_text(config, encoding="utf-8")
    return executable, data


def stop_children(children):
    for child in children:
        if child.poll() is None:
            child.terminate()
    for child in children:
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=5)


def run_pair(halo: Path, data_root: Path, output: Path, scenario: str, rtt_ms: int, loss_percent: int,
             host_address="127.0.0.200", client_address="127.0.0.201", driver="host", visible=False, spot="", local=False, corner=False):
    if os.name != "nt":
        raise ToolError("the gate runs the Windows build: run it with Windows Python")
    if not halo.is_file() or scenario not in SCENARIOS or rtt_ms < 0 or not 0 <= loss_percent <= 100:
        raise ToolError("no such executable or case")
    if local and (scenario != "longsword" or driver != "host"):
        raise ToolError("local mode needs Longsword with local pilot 0")
    addresses = [ipaddress.ip_address(a) for a in (host_address, client_address)]
    if any(a.version != 4 or not a.is_loopback or str(a) == "127.0.0.1" for a in addresses) or \
            addresses[0] == addresses[1]:
        raise ToolError("two distinct IPv4 loopback addresses other than 127.0.0.1")
    output.mkdir(parents=True, exist_ok=False)
    sides = ("host",) if local else ("host", "client")
    children, handles = [], []
    metadata = {"scenario": scenario, "driver": driver, "rtt_ms": rtt_ms, "loss_percent": loss_percent, "children": []}
    started = time.monotonic()

    def observe_exits():
        for child, item in zip(children, metadata["children"]):
            code = child.poll()
            if code is not None and "own_exit" not in item:
                item["own_exit"] = code
                item["first_seen_exit_utc"] = time.time()
                item["first_seen_exit_elapsed"] = time.monotonic() - started

    try:
        for side in sides:
            executable, data = make_side(halo.resolve(), data_root.resolve(), output, side, visible,
                                        custom_maps=scenario == "longsword")
            env = child_environment(side, data, output, scenario, rtt_ms, loss_percent, host_address, client_address,
                                    driver, visible, spot, local, corner)
            stdout = (output / side / "stdout.txt").open("wb")
            stderr = (output / side / "stderr.txt").open("wb")
            handles += [stdout, stderr]
            child = subprocess.Popen([str(executable)], cwd=executable.parent, env=env, stdout=stdout, stderr=stderr,
                                     creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW)
            children.append(child)
            metadata["children"].append({"side": side, "pid": child.pid,
                                         "environment": {k: v for k, v in env.items() if k.startswith("HALO_")}})
        deadline = time.monotonic() + case_seconds(scenario)
        while time.monotonic() < deadline:
            logs = [output / side / "data/debug.txt" for side in sides]
            if all(path.is_file() and " kind=end " in path.read_text(encoding="utf-8", errors="replace") for path in logs):
                break
            observe_exits()
            if any("own_exit" in item for item in metadata["children"]):
                raise ToolError("a game exited before the logs ended")
            time.sleep(.2)
        else:
            raise ToolError("the case timed out")
    finally:
        # Observe all children before terminating any survivor. Code 1 from
        # runner cleanup is not the game's own exit; timestamps are observations.
        observe_exits()
        for child, item in zip(children, metadata["children"]):
            item["alive_before_cleanup"] = child.returncode is None
        (output / "launch.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
        stop_children(children)
        for handle in handles:
            handle.close()
        for child, item in zip(children, metadata["children"]):
            item["exit"] = child.returncode
        for side in sides:
            source = output / side / "data/debug.txt"
            if source.is_file():
                shutil.copy2(source, output / f"{side}.debug.txt")
        (output / "launch.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    return output / "host.debug.txt", None if local else output / "client.debug.txt"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--halo", type=Path, default=Path("build/windows/halo.exe"))
    parser.add_argument("--data-root", type=Path, default=Path(r"D:\Users\haddo\Desktop\halo-windows-release"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scenarios", default="all")
    parser.add_argument("--rtts", default="0,50,100,150,200,250,300")
    parser.add_argument("--loss-case", default="100:2", help="rtt:percent, or none")
    parser.add_argument("--host-address", default="127.0.0.200")
    parser.add_argument("--client-address", default="127.0.0.201")
    parser.add_argument("--visible", action="store_true", help="show both games' windows (do not touch them)")
    parser.add_argument("--local", action="store_true", help="Longsword only: one split-screen game, pilot 0 and rider 1, no network")
    parser.add_argument("--rear-corner", action="store_true", help="local Longsword: hold nose up and walk backward into the rear corner, then forward")
    parser.add_argument("--spot", default=None, help="the Longsword rider's x,y,forward,left (default: LONGSWORD_SPOT)")
    parser.add_argument("--survey", action="store_true", help="the Longsword: survey its deck, board no one")
    parser.add_argument("--negative", action="store_true", help="the Longsword: the open-top spot (LONGSWORD_WING); no cabin evidence may appear")
    parser.add_argument("--cabin", default=None, help="the Longsword's cockpit volume x0,y0,z0,x1,y1,z1 (default: LONGSWORD_CABIN)")
    args = parser.parse_args(argv)
    scenarios = list(STOCK_SCENARIOS) if args.scenarios == "all" else args.scenarios.split(",")
    if not scenarios or any(s not in SCENARIOS for s in scenarios):
        parser.error("unknown scenario")
    if args.local and (scenarios != ["longsword"] or args.survey):
        parser.error("--local needs --scenarios longsword without --survey")
    if args.rear_corner and (not args.local or args.negative):
        parser.error("--rear-corner needs --local without --negative")
    box = None
    spot = ""
    if "longsword" in scenarios:
        try:
            if args.cabin:
                values = [float(v) for v in args.cabin.split(",")]
                if len(values) != 6:
                    raise ValueError
                box = (tuple(values[:3]), tuple(values[3:]))
            else:
                box = LONGSWORD_CABIN
            if args.survey:
                spot = ""
            elif args.negative:
                spot = ",".join(map(str, LONGSWORD_WING))
            elif args.spot is not None:
                spot = args.spot
            elif LONGSWORD_SPOT is not None:
                spot = ",".join(map(str, LONGSWORD_SPOT))
            else:
                parser.error("longsword needs --spot, --survey or LONGSWORD_SPOT")
        except ValueError:
            parser.error("invalid --cabin: x0,y0,z0,x1,y1,z1")
    try:
        cases = [(int(rtt), 0) for rtt in args.rtts.split(",")]
        if args.loss_case != "none":
            rtt, loss = args.loss_case.split(":")
            cases.append((int(rtt), int(loss)))
    except ValueError:
        parser.error("invalid round trips or loss case")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for scenario in scenarios:
        for rtt, loss in cases:
            for driver in (("host", "client") if not args.local and scenario in ("driven", "longsword") and not (scenario == "longsword" and args.survey) else ("host",)):
                case = output / f"{scenario}-{'local' if args.local else driver}-rtt{rtt}-loss{loss}"
                try:
                    mode = {"local": True} if args.local else {}
                    if args.rear_corner:
                        mode["corner"] = True
                    host_path, client_path = run_pair(args.halo.resolve(), args.data_root.resolve(), case, scenario,
                                                      rtt, loss, args.host_address, args.client_address, driver,
                                                      args.visible, spot if scenario == "longsword" else "", **mode)
                    host, client = parse_log(host_path), parse_log(client_path) if client_path is not None else []
                    if scenario == "longsword" and args.survey:
                        for row in host + client:
                            if row["kind"] == "survey":
                                print(f"survey {row['side']}: x={row['x']} y={row['y']} z={row['z']} room={row['room']} "
                                      f"old_hit={row['old_hit']} old_distance={row['old_distance']}")
                        result = {"status": "SURVEY", "required": False, "reasons": [], "metrics": {}}
                    elif scenario == "longsword":
                        result = judge_longsword(host + client, rtt, loss, box, negative=args.negative, **mode)
                    else:
                        result = judge(align(host, client), host + client, rtt, loss)
                except (ToolError, OSError, subprocess.SubprocessError) as error:
                    result = {"status": "TOOL_ERROR", "required": rtt <= 100 and loss == 0 and scenario not in REPORTED_ONLY,
                              "reasons": [str(error)], "metrics": {}}
                except Exception as error:
                    result = {"status": "TOOL_ERROR", "required": rtt <= 100 and loss == 0 and scenario not in REPORTED_ONLY,
                              "reasons": [f"{type(error).__name__}: {error}"], "metrics": {}}
                result.update({"scenario": scenario, "driver": driver, "rtt_ms": rtt, "loss_percent": loss,
                               "logs": [str(case / f"{side}.debug.txt") for side in (("host",) if args.local else ("host", "client"))]})
                results.append(result)
                (output / "summary.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
                print(f"{scenario}/{driver} round trip {rtt} ms, loss {loss}%: {result['status']}")
    if any(r["status"] == "TOOL_ERROR" for r in results):
        return 2
    return 1 if any((r["required"] or r["scenario"] == "longsword") and r["status"] == "DESYNC" for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
