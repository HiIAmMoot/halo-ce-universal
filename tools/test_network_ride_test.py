"""tools/network_ride_test.py's parsing, alignment and judging on synthetic logs (no game), and its launcher's
isolation (no process started)."""

import copy
import importlib.util
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import network_ride_test as ride  # noqa: E402

BOARD = 300


def test_coop_control_inhibited_is_tool_error():
    for scenario in ("coop", "coop-double"):
        for side in ("host", "client"):
            host, client = fixture_logs(scenario)
            assert not any(r["metric"] == "control-inhibited" for r in result(host, client)["reasons"])
            rows = host if side == "host" else client
            rows.append(dict(rows[0], kind="control_inhibited", tick=BOARD + ride.RIDE_JUMP,
                             phase="jump", player=0 if side == "host" else 1))
            answer = result(host, client)
            assert answer["status"] == "TOOL_ERROR", answer
            failures = [r for r in answer["reasons"] if r["metric"] == "control-inhibited"]
            assert len(failures) == 1 and failures[0]["side"] == side, answer

def test_remote_copy_without_takeoffs_fails_outcome():
    host, client = fixture_logs()
    for row in client:
        if row["kind"] == "outcome" and row["player"] == 1:
            row["jumps"] = row["landings"] = 0
    answer = result(host, client)
    assert any(r["metric"] == "outcome" and r["side"] == "client" and r["player"] == 1
               for r in answer["reasons"]), answer


def test_unsupported_first_jump_is_scored_desync():
    host, client = fixture_logs()
    host.append(dict(host[0], kind="support_failure", tick=BOARD + ride.RIDE_JUMP, phase="jump"))
    answer = result(host, client)
    assert answer["status"] == "DESYNC", answer
    assert any(r["metric"] == "launch-unsupported" for r in answer["reasons"]), answer

def test_control_and_suspension_records_parse():
    import tempfile
    with tempfile.TemporaryDirectory() as folder:
        log = Path(folder) / "debug.txt"
        log.write_text("ride schema=1 kind=scripts_suspended session=test scenario=coop side=host seq=1 tick=1061\n"
                       "ride schema=1 kind=control_inhibited session=test scenario=coop side=host seq=2 "
                       "tick=1400 phase=jump player=0\n")
        try:
            suspended, inhibited = ride.parse_log(log)
        except ride.ToolError as error:
            raise AssertionError(str(error))
    assert suspended["tick"] == 1061 and (inhibited["phase"], inhibited["player"]) == ("jump", 0)


def test_board_refusal_diagnostics_parse(tmp_path):
    log = tmp_path / "debug.txt"
    log.write_text("ride schema=1 kind=board_refused session=test scenario=convoy side=host seq=1 "
                   "tick=300 unit=1 vehicle=2 ordinal=1 candidate=3 place=.15,.05,0 ray=1 clearance=0 "
                   "origin=1,2,3 forward=1,0,0 up=0,0,1\n")
    row, = ride.parse_log(log)
    assert (row["ordinal"], row["candidate"], row["ray"], row["clearance"]) == (1, 3, 1, 0)
    assert row["place"] == (.15, .05, 0.)


def fixture_logs(scenario="straight"):
    """Both machines' records of a clean run: two riders on one vehicle at 0.04 a tick, the host's player 0 and the
    client's player 1, every tick from the board tick to the end."""
    logs = {"host": [], "client": []}
    seq = {"host": 0, "client": 0}

    def emit(side, kind, **values):
        seq[side] += 1
        row = {"schema": "1", "session": "fixture", "scenario": scenario, "side": side, "kind": kind,
               "seq": seq[side]}
        row.update(values)
        logs[side].append(row)
        return row

    for side in logs:
        emit(side, "prepare", tick=BOARD, players=2, vehicles="65536,-1",
             driver=196608 if scenario in ("coop", "coop-double") else -1, driver_player=-1, board_tick=BOARD,
             driver_missing=0)
        for player in (0, 1):
            emit(side, "grace", tick=BOARD, player=player, grace_ticks=15)
    scenario_end = ride.phase_tick(scenario, ride.RIDE_END)
    for tick in range(BOARD, BOARD + scenario_end + 1):
        elapsed = tick - BOARD
        phase = ride.phase_of(elapsed, scenario)
        # (brush's riders stand still beside the vehicle while it passes)
        travel = min(elapsed, ride.RIDE_OBSERVE) if scenario == "brush" else elapsed
        # (and beside the hull, an arm's length out along its left)
        beside = 1. if scenario == "brush" else 0.
        for side in logs:
            for player in (0, 1):
                local = int((side == "host") == (player == 0))
                values = {"player": player, "unit": 131072 + player, "phase": phase, "deck_present": 1,
                          "vehicle": 65536, "node": -1, "ticks": 60, "flags": 0, "velocity": (0., 0., 0.), "peak": 0.,
                          "world": (travel * .04, player * .5 + beside, 0.), "base": (travel * .04, player * .5, .2),
                          "place": (0., player * .5, .2), "origin": (travel * .04, 0., 0.),
                          "forward": (1., 0., 0.), "left": (0., 1., 0.), "up": (0., 0., 1.), "board_tick": BOARD}
                emit(side, "sample", simulation_tick=tick, network_tick=tick + 1, local=local, height_valid=1,
                     height=0., body=1., shield=1., last_applied_host_tick=tick, **values)
                if local:
                    jump = int(ride.RIDE_JUMP <= elapsed < ride.RIDE_JUMP + ride.RIDE_JUMPS * ride.RIDE_JUMP_PERIOD and
                               (elapsed - ride.RIDE_JUMP) % ride.RIDE_JUMP_PERIOD == 0)
                    emit(side, "input", tick=tick, player=player, unit=values["unit"], phase=phase, jump=jump,
                         throttle=(0., 0.))
                if side == "client" and local:
                    emit(side, "own", tick=tick + 1, **values)
                if elapsed in {ride.phase_tick(scenario, start) for start in ride.PHASE_STARTS}:
                    emit(side, "phase", tick=tick, player=player, phase=phase)
            emit(side, "vehicle", tick=tick, vehicle=65536, world=(travel * .04, 0., 0.), velocity=(.04, 0., 0.),
                  angular=(0., 0., 0.), moved=.04, written=0.,
                  writes=1 if scenario in ("coop", "coop-double") and side == "client" else 0,
                  carries=1 if scenario in ("coop", "coop-double") and side == "client" else 0,
                  driver=196608 if scenario in ("coop", "coop-double") else -1, driver_player=-1)
        for player in (0, 1):
            state = {"tick": tick + 1, "host_tick": tick + 1, "player": player, "unit": 131072 + player,
                     "predicted_valid": int(player == 1), "predicted_low16": (tick + 1) & 0xffff, "deck_present": 1,
                     "vehicle": 65536, "node": -1, "ticks": 60, "flags": 0, "velocity": (0., 0., 0.), "peak": 0.,
                     "world": (travel * .04, player * .5 + beside, 0.), "place": (0., player * .5, .2)}
            emit("host", "state", **state)
            emit("client", "state", **state)
            emit("client", "apply", tick=tick + 1, player=player, unit=state["unit"], applied_host_tick=tick + 1)
    for side in logs:
        for player in (0, 1):
            emit(side, "outcome", tick=BOARD + scenario_end, player=player, jump_presses=10, jumps=10, landings=10,
                 crossings=0, stepped_off=1, landed_off=0)
        emit(side, "end", tick=BOARD + scenario_end, players=2)
    return logs["host"], logs["client"]


def result(host, client, rtt=100, loss=0):
    return ride.judge(ride.align(host, client), host + client, rtt, loss)


def driven_recovery_logs(end=2060, start=2050):
    host, client = fixture_logs("driven")
    for rows in (host, client):
        rows[:] = [r for r in rows if r.get("player") != 1]
        for r in rows:
            if r["kind"] in ("prepare", "end"):
                r["players"] = 1
            if r["kind"] == "sample" and start <= r["simulation_tick"] < start + 4:
                r.update(height_valid=0, height=None, ticks=0, deck_present=0)
        for event, tick in (("start", start), ("place", start + 2), ("acquired", end)):
            rows.append(dict(schema="1", session="fixture", scenario="driven", side=rows[0]["side"],
                             kind="recovery", seq=100000 + tick, tick=tick, player=0, unit=131072,
                             reason=event, start_tick=start, recoveries=1))
    return host, client


def test_driven_schedule_fits_recovery_budget():
    # Old length ends jumping at 1740; driven now reserves ten (180 + 6)-tick holds.
    assert ride.phase_of(3599, "driven") == "jump"
    assert ride.phase_of(3600, "driven") == "cross"
    assert ride.phase_of(3779, "driven") == "cross"
    assert ride.phase_of(3780, "driven") == "step-off"
    assert ride.phase_of(3899, "driven") == "step-off"
    assert ride.phase_of(3900, "driven") == "observe"
    for scenario in ride.SCENARIOS:
        if scenario in ("driven", "longsword", "brush"):
            continue
        assert ride.phase_of(1739, scenario) == "jump"
        assert ride.phase_of(1740, scenario) == "cross"
        assert ride.phase_of(1920, scenario) == "step-off"
        assert ride.phase_of(2040, scenario) == "observe"


def test_driven_judge_reads_extended_schedule():
    host, client = fixture_logs("driven")
    for rows in (host, client):
        # Ten legal presses, including deferred ones after the former jump-phase end.
        for row in rows:
            if row["kind"] == "input":
                row["jump"] = int(row["tick"] in (1140, 1416, 1692, 1968, 2244, 2520, 2796, 3072, 3348, 3624))
    answer = result(host, client, 0)
    assert not any(r["metric"] in ("sample-coverage", "jump-inputs") for r in answer["reasons"]), answer
    for rows in (host, client):
        for row in rows:
            if row["kind"] == "outcome":
                row.update(jumps=9, landings=9)
    assert any(r["metric"] == "outcome" for r in result(host, client, 0)["reasons"])


def test_driven_launcher_reserves_extra_simulation_time(tmp_path):
    for scenario, seconds in (("driven", "242"), ("circle", "180"), ("coop", "360"), ("longsword", "420")):
        env = ride.child_environment("host", tmp_path, tmp_path, scenario, 0, 0, "127.0.0.230", "127.0.0.231", "host")
        assert env["HALO_EXIT_AFTER"] == seconds
        assert ride.case_seconds(scenario) == int(seconds)


def test_driven_recovery_counts_and_bounds_contact_exclusion():
    # Cross is normally contact-scored: jump's blanket exemption cannot mask this test.
    host, client = driven_recovery_logs(end=3920, start=3910)
    answer = result(host, client)
    assert answer["metrics"].get("recoveries") == {"host:0": 1, "client:0": 1}
    assert not any(r["metric"] == "contact-lost" for r in answer["reasons"])
    for r in host:
        if r["kind"] == "sample" and r["simulation_tick"] == 3922:
            r.update(height_valid=0, height=None)
    assert any(r["metric"] == "contact-lost" and r["tick"] == 3922 for r in result(host, client)["reasons"])


def driven_outcome_logs(landings, recoveries, jumps=10, landed_off=0):
    host, client = fixture_logs("driven")
    for rows in (host, client):
        rows[:] = [r for r in rows if r.get("player") != 1]
        for row in rows:
            if row["kind"] in ("prepare", "end"):
                row["players"] = 1
            elif row["kind"] == "outcome":
                row.update(landings=landings, jumps=jumps, landed_off=landed_off)
        for index in range(recoveries):
            start = 1500 + index * 30
            for event, tick in (("start", start), ("place", start + 2), ("acquired", start + 10)):
                rows.append(dict(schema="1", session="fixture", scenario="driven", side=rows[0]["side"],
                                 kind="recovery", seq=200000 + tick, tick=tick, player=0, unit=131072,
                                 reason=event, start_tick=start, recoveries=index + 1))
    return host, client


def test_driven_outcome_needs_a_deck_landing():
    answer = result(*driven_outcome_logs(0, 10))
    assert any(r["metric"] == "outcome" for r in answer["reasons"]), answer


def test_driven_outcome_accounts_for_every_jump():
    answer = result(*driven_outcome_logs(7, 3))
    assert answer["status"] == "PASS", answer
    for landings, recoveries, jumps in ((7, 2, 10), (7, 3, 11)):
        answer = result(*driven_outcome_logs(landings, recoveries, jumps))
        assert any(r["metric"] == "outcome" for r in answer["reasons"]), answer
    host, client = driven_outcome_logs(7, 3)
    host[:] = [r for r in host if not (r["kind"] == "recovery" and r["reason"] == "acquired")]
    assert any(r["metric"] == "outcome" for r in result(host, client)["reasons"])


def test_driven_recovery_without_episode_cannot_account_jump():
    host, client = driven_outcome_logs(7, 3)
    for row in host + client:
        if row["kind"] == "outcome":
            row["episode_recoveries"] = 0
    assert any(r["metric"] == "outcome" for r in result(host, client)["reasons"])


def test_driven_outcome_keeps_landed_off_limit():
    assert result(*driven_outcome_logs(7, 3, landed_off=ride.LANDED_OFF_LIMIT))["status"] == "PASS"
    answer = result(*driven_outcome_logs(7, 3, landed_off=ride.LANDED_OFF_LIMIT + 1))
    assert any(r["metric"] == "landed-off-floor" for r in answer["reasons"]), answer


def test_driven_recovery_missing_acquisition_fails():
    host, client = driven_recovery_logs()
    host[:] = [r for r in host if not (r["kind"] == "recovery" and r["reason"] == "acquired")]
    assert any(r["metric"] == "recovery-incomplete" for r in result(host, client)["reasons"])


def test_driven_recovery_timeout_fails():
    host, client = driven_recovery_logs(2231)
    assert any(r["metric"] == "recovery-timeout" for r in result(host, client)["reasons"])


def test_driven_recovery_allows_bounded_braking_time():
    host, client = driven_recovery_logs(2150)
    assert not any(r["metric"] == "recovery-timeout" for r in result(host, client)["reasons"])


def test_driven_recovery_requires_physical_support():
    host, client = driven_recovery_logs()
    for r in host:
        if r["kind"] == "sample" and 2054 <= r["simulation_tick"] <= 2060:
            r.update(ticks=59)
    assert any(r["metric"] == "recovery-contact" for r in result(host, client)["reasons"])


def test_driven_acquisition_includes_logged_tick():
    host, client = driven_recovery_logs()
    for rows in (host, client):
        for row in rows:
            if row["kind"] == "recovery":
                row.update(tick={"start": 1510, "place": 1570, "acquired": 1577}[row["reason"]], start_tick=1510)
            elif row["kind"] == "sample" and 1510 <= row["simulation_tick"] <= 1571:
                row.update(ticks=0, deck_present=0)
    # Exactly 1572..1577 support; 1571 is not a supporting tick.
    assert not any(r["metric"] == "recovery-contact" for r in result(host, client)["reasons"])
    for row in host:
        if row["kind"] == "sample" and row["simulation_tick"] == 1577:
            row.update(ticks=0, deck_present=0)
    assert any(r["metric"] == "recovery-contact" for r in result(host, client)["reasons"])


def test_life_record_keeps_engine_cause_and_fails(tmp_path):
    host, client = fixture_logs("driven")
    life = dict(schema="1", session="fixture", scenario="driven", side="host", kind="life", seq=90000,
                tick=2611, player=0, unit=131072, current_unit=-1, reason="unit-changed", recorded=1,
                damage_category=17, damage_flags=4, death_tick=2611, attacker_object=65536,
                attacker_player=1, attacker_tick=2610, nearest_vehicle=65536,
                vehicle_speed=.05, vehicle_distance=.2, world=(1., 2., 3.))
    path = tmp_path / "life.log"
    path.write_text("ride " + " ".join(f"{k}={','.join(str(x) for x in v) if isinstance(v, tuple) else v}"
                                      for k, v in life.items()) + "\n")
    host.extend(ride.parse_log(path))
    answer = result(host, client)
    assert any(r["metric"] == "rider-died" and r["tick"] == 2611 for r in answer["reasons"])
    assert answer["metrics"]["life_events"] == [life]


def test_driven_recovery_limit_fails():
    host, client = driven_recovery_logs()
    for rows in (host, client):
        original = [r for r in rows if r["kind"] == "recovery"]
        for index in range(1, 11):
            for r in original:
                extra = dict(r, tick=r["tick"] + index*15, start_tick=2050 + index*15,
                             seq=r["seq"] + index*100, recoveries=index+1)
                rows.append(extra)
    assert any(r["metric"] == "recovery-limit" for r in result(host, client)["reasons"])


def test_corner_environment(tmp_path):
    import inspect
    assert "corner" in inspect.signature(ride.child_environment).parameters, "opt-in corner script missing"
    env = ride.child_environment("host", tmp_path, tmp_path, "longsword", 0, 0,
                                 "127.0.0.200", "127.0.0.201", "host", local=True, corner=True)
    assert env["HALO_NETWORK_TEST_RIDE_CORNER"] == "true"


def test_corner_no_anchor_rejected():
    import inspect
    assert "corner" in inspect.signature(ride.judge_longsword).parameters, "corner judge missing"
    host, _ = fixture_longsword(failed_at=BOARD + 2800)
    for row in host:
        if row["kind"] == "cabin" and row["tick"] == BOARD + 2800:
            row["anchor"] = 0
            row["raw"] = 1
    answer = ride.judge_longsword(host, 0, 0, LONGSWORD_BOX, local=True, corner=True)
    assert answer["status"] == "DESYNC", answer
    assert any(r["metric"] == "no-anchor" for r in answer["reasons"]), answer


def test_clean_run_passes():
    host, client = fixture_logs()
    answer = result(host, client)
    assert answer["status"] == "PASS", answer["reasons"]
    assert answer["required"] is True


def test_hops_outside_jump_phase_do_not_count():
    host, client = fixture_logs()
    for row in host + client:
        if row["kind"] == "input" and row["tick"] in (BOARD + 839, BOARD + 1740, BOARD + 1748):
            row["jump"] = 1
    answer = result(host, client)
    assert answer["status"] == "PASS", answer["reasons"]


def test_cross_hop_cannot_replace_a_missing_jump_phase_press():
    host, client = fixture_logs()
    for row in host + client:
        if row["kind"] == "input" and row["tick"] == BOARD + 840:
            row["jump"] = 0
        if row["kind"] == "input" and row["tick"] == BOARD + 1740:
            row["jump"] = 1
    answer = result(host, client)
    assert answer["status"] == "TOOL_ERROR", answer
    assert {(r["side"], r["player"], r["value"]) for r in answer["reasons"] if r["metric"] == "jump-inputs"} == {
        ("host", 0, 9), ("client", 1, 9)}


def test_convoy_missing_crossings_are_report_only():
    host, client = fixture_logs("convoy")
    answer = result(host, client, 0)
    assert answer["status"] == "PASS", answer["reasons"]
    assert answer["required"] is True  # Only crossing is report-only, not the whole convoy.
    assert answer["metrics"].get("crossings") == {
        f"{side}:{player}": {"count": 0, "cross_phase_count": 0, "cross_phase": False}
        for side in ("host", "client") for player in (0, 1)}


def test_convoy_crossings_report_counts_and_physical_phase():
    host, client = fixture_logs("convoy")
    for row in host + client:
        if row["kind"] == "outcome":
            row["crossings"] = 1
        if row["kind"] == "sample":
            first = BOARD + (1741 if row["player"] == 0 else 841)
            if row["simulation_tick"] >= first:
                row["vehicle"] = 65537
    answer = result(host, client, 0)
    assert answer["metrics"].get("crossings") == {
        "host:0": {"count": 1, "cross_phase_count": 1, "cross_phase": True},
        "client:0": {"count": 1, "cross_phase_count": 1, "cross_phase": True},
        "host:1": {"count": 1, "cross_phase_count": 0, "cross_phase": False},
        "client:1": {"count": 1, "cross_phase_count": 0, "cross_phase": False}}


def test_convoy_grace_window_is_not_a_physical_crossing():
    host, client = fixture_logs("convoy")
    for row in host + client:
        if row["kind"] == "outcome":
            row["crossings"] = 1
        if row["kind"] == "sample" and row["simulation_tick"] >= BOARD + 1740:
            row.update(vehicle=65537, ticks=59)
    answer = result(host, client, 0)
    assert answer["metrics"].get("crossings") == {
        f"{side}:{player}": {"count": 1, "cross_phase_count": 0, "cross_phase": False}
        for side in ("host", "client") for player in (0, 1)}


def test_convoy_other_outcome_checks_still_fail():
    for field, value in (("jumps", 9), ("landings", 9), ("stepped_off", 0)):
        host, client = fixture_logs("convoy")
        next(r for r in host if r["kind"] == "outcome" and r["player"] == 0)[field] = value
        answer = result(host, client, 0)
        failures = [r for r in answer["reasons"] if r["metric"] == "outcome"]
        assert answer["status"] == "DESYNC", (field, answer)
        assert len(failures) == 1 and failures[0]["side"] == "host" and failures[0]["player"] == 0, failures


@pytest.mark.parametrize("low,reference,expected", [(2, 65540, 65538), (65535, 65540, 65535), (0, 65536, 65536)])
def test_unwrap_tick(low, reference, expected):
    assert ride.unwrap_tick(low, reference) == expected


@pytest.mark.parametrize("error,status", [(.05, "PASS"), (.05001, "DESYNC")])
def test_place_limit(error, status):
    host, client = fixture_logs()
    for row in client:
        if row["kind"] == "own" and row["phase"] == "stand" and row["tick"] > BOARD + 40:
            row["place"] = (error, .5, .2)
    assert result(host, client)["status"] == status


def test_settling_skips_the_first_second():
    host, client = fixture_logs()
    for row in client:
        if row["kind"] == "own" and row["tick"] < BOARD + 30:
            row["place"] = (.2, .5, .2)
    assert result(host, client)["status"] == "PASS"


@pytest.mark.parametrize("height,status", [(-.1, "PASS"), (-.10001, "DESYNC")])
def test_height_limit(height, status):
    host, client = fixture_logs()
    next(r for r in client if r["kind"] == "sample" and r["simulation_tick"] == 400)["height"] = height
    assert result(host, client)["status"] == status


def test_no_hit_is_not_zero_height():
    host, client = fixture_logs()
    row = next(r for r in client if r["kind"] == "sample" and r["phase"] == "stand")
    row.update(height_valid=0, height=None)
    assert result(host, client)["status"] == "DESYNC"


def lose_height(rows, first, last, player=0, height=-.5):
    """A rider (one machine's samples) at height under the limit from elapsed first to last."""
    for row in rows:
        if row["kind"] == "sample" and row["player"] == player and first <= row["simulation_tick"] - BOARD <= last:
            row.update(height=height)


def lose_contact(rows, first, last, player=0):
    """A rider without a surface under it from elapsed first to last."""
    for row in rows:
        if row["kind"] == "sample" and row["player"] == player and first <= row["simulation_tick"] - BOARD <= last:
            row.update(height_valid=0, height=None)


def reboard(rows, landing, tick, player=0, side="host"):
    rows.append({"schema": "1", "session": "fixture", "scenario": "straight", "side": side, "kind": "reboard",
                 "seq": 10 ** 6 + len(rows), "tick": BOARD + tick, "player": player, "unit": 131072 + player,
                 "landing_tick": BOARD + landing, "ticks": tick - landing})


def set_claim_off(rows, first, last, player=1):
    for row in rows:
        if row["kind"] == "sample" and row["player"] == player and first <= row["simulation_tick"] - BOARD <= last:
            row["deck_present"] = 0


# a landing in the fourth jump cycle (the press at 1110, the next at 1200): re-boarded at 1150, the echo window to
# 1150 + 3 (100 ms) + 30 = 1183. The host's player 0 is re-boarded by the host, the client's player 1 by the client.
LANDING, REBOARD, WINDOW_END = 1125, 1150, 1183


def test_off_deck_landing_with_a_reboard_is_an_outcome():
    host, client = fixture_logs()
    lose_height(host, LANDING, REBOARD, 0)
    reboard(host, LANDING, REBOARD, 0)
    lose_height(client, LANDING, REBOARD, 1)
    set_claim_off(host, LANDING, REBOARD + 3, 1)
    reboard(client, LANDING, REBOARD, 1, "client")
    answer = result(host, client)
    assert answer["status"] == "PASS", answer["reasons"]
    assert answer["metrics"]["landed_off"] == {0: 1, 1: 1} and answer["metrics"]["reboard_ticks"] == {0: 25, 1: 25}
    # (from the landing to the end of the echo window: not fully judged)
    assert answer["metrics"]["off_deck_share"][0] == pytest.approx((WINDOW_END - LANDING) / (ride.RIDE_CROSS - ride.RIDE_JUMP))


def test_a_landing_the_host_did_not_see_is_judged():
    host, client = fixture_logs()
    lose_height(client, LANDING, REBOARD, 1)
    reboard(client, LANDING, REBOARD, 1, "client")
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "height"


def test_a_reboard_by_the_other_machine_is_not_a_landing():
    host, client = fixture_logs()
    lose_height(client, LANDING, REBOARD, 1)
    set_claim_off(host, LANDING, REBOARD + 3, 1)
    reboard(host, LANDING, REBOARD, 1)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "height"
    assert answer["metrics"]["landed_off"] == {}


def test_client_copy_of_the_hosts_landing_is_exempt_for_a_round_trip():
    host, client = fixture_logs()
    lose_height(host, LANDING, REBOARD, 0)
    lose_height(client, LANDING, REBOARD + 3, 0)
    reboard(host, LANDING, REBOARD, 0)
    answer = result(host, client)
    assert answer["status"] == "PASS", answer["reasons"]


def test_client_copy_is_judged_after_the_round_trip():
    host, client = fixture_logs()
    lose_height(host, LANDING, REBOARD, 0)
    lose_height(client, LANDING, REBOARD + 4, 0)
    reboard(host, LANDING, REBOARD, 0)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "height"
    assert answer["reasons"][0]["tick"] == BOARD + REBOARD + 4


def test_host_copy_of_the_clients_landing_is_exempt_for_a_round_trip():
    host, client = fixture_logs()
    lose_height(client, LANDING, REBOARD, 1)
    lose_height(host, LANDING, REBOARD + 3, 1)
    set_claim_off(host, LANDING, REBOARD + 3, 1)
    reboard(client, LANDING, REBOARD, 1, "client")
    answer = result(host, client)
    assert answer["status"] == "PASS", answer["reasons"]


def test_host_copy_is_judged_after_the_round_trip():
    host, client = fixture_logs()
    lose_height(client, LANDING, REBOARD, 1)
    lose_height(host, LANDING, REBOARD + 4, 1)
    set_claim_off(host, LANDING, REBOARD + 3, 1)
    reboard(client, LANDING, REBOARD, 1, "client")
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "height"
    assert answer["reasons"][0]["tick"] == BOARD + REBOARD + 4


def test_height_lost_without_a_landing_is_an_error():
    host, client = fixture_logs()
    lose_height(host, LANDING, REBOARD, 0)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "height"


def test_contact_lost_without_a_landing_is_an_error():
    host, client = fixture_logs()
    lose_contact(host, 1735, 1745)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "contact-lost"


def test_contact_lost_after_the_reboard_is_an_error():
    host, client = fixture_logs()
    lose_height(host, 1700, 1739, 0)
    lose_contact(host, 1800, 1800)
    reboard(host, 1700, 1739)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["tick"] == BOARD + 1800


def test_height_a_few_ticks_after_the_reboard_is_judged():
    host, client = fixture_logs()
    lose_height(host, LANDING, REBOARD + 3, 0)
    reboard(host, LANDING, REBOARD, 0)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["tick"] == BOARD + REBOARD + 1


def test_height_before_the_cycles_press_is_judged():
    host, client = fixture_logs()
    lose_height(host, 1100, REBOARD, 0)
    reboard(host, 1100, REBOARD, 0)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["tick"] == BOARD + 1100


def echoed(host, client, player=1):
    if player == 0:
        reboard(host, LANDING, REBOARD, 0)
    else:
        reboard(client, LANDING, REBOARD, player, "client")


def test_place_error_inside_the_echo_window_is_skipped():
    host, client = fixture_logs()
    echoed(host, client)
    for row in client:
        if row["kind"] == "own" and row["player"] == 1 and row["tick"] == BOARD + WINDOW_END:
            row["place"] = (.3, .5, .2)
    assert result(host, client)["status"] == "PASS"


def test_place_error_after_the_echo_window_fails():
    host, client = fixture_logs()
    echoed(host, client)
    for row in client:
        if row["kind"] == "own" and row["player"] == 1 and row["tick"] == BOARD + WINDOW_END + 1:
            row["place"] = (.3, .5, .2)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "place"


def test_place_error_just_before_the_reboard_is_judged():
    host, client = fixture_logs()
    echoed(host, client)
    for row in client:
        if row["kind"] == "own" and row["player"] == 1 and row["tick"] == BOARD + REBOARD - 1:
            row["place"] = (.3, .5, .2)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "place"


def test_echo_window_does_not_cross_the_next_jump():
    host, client = fixture_logs()
    reboard(client, 1170, 1195, 1, "client")
    for row in client:
        if row["kind"] == "own" and row["player"] == 1 and row["tick"] == BOARD + 1201:
            row["place"] = (.3, .5, .2)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "place"


def test_one_correction_in_the_window_is_allowed_two_are_not():
    host, client = fixture_logs()
    echoed(host, client)
    correction(client, BOARD + REBOARD + 5, 1)
    assert result(host, client)["status"] == "PASS"
    correction(client, BOARD + REBOARD + 9, 1)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "reboard-corrections"


def test_correction_after_the_window_counts_with_the_one_in_it():
    host, client = fixture_logs()
    echoed(host, client)
    correction(client, BOARD + REBOARD + 5, 1)
    correction(client, BOARD + WINDOW_END + 1, 1)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "corrections-in-two-seconds"


def test_corrections_just_before_the_reboard_are_judged():
    host, client = fixture_logs()
    echoed(host, client)
    correction(client, BOARD + REBOARD - 3, 1)
    correction(client, BOARD + REBOARD - 1, 1)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "corrections-in-two-seconds"


def test_claim_disagreement_after_the_window_fails():
    host, client = fixture_logs()
    echoed(host, client)
    set_claim_off(client, WINDOW_END + 1, WINDOW_END + 30)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and answer["reasons"][0]["metric"] == "claim-disagreement"


def test_claim_disagreement_inside_the_window_counts_from_its_end():
    host, client = fixture_logs()
    echoed(host, client)
    set_claim_off(client, REBOARD + 5, WINDOW_END + 10)
    answer = result(host, client)
    assert not any(r["metric"] == "claim-disagreement" for r in answer["reasons"])


def test_claim_disagreement_from_before_the_landing_still_fails():
    host, client = fixture_logs()
    echoed(host, client)
    set_claim_off(client, LANDING - 10, WINDOW_END)
    answer = result(host, client)
    assert any(r["metric"] == "claim-disagreement" for r in answer["reasons"])


def test_a_rider_put_back_but_not_converged_is_a_desync():
    host, client = fixture_logs()
    echoed(host, client)
    set_claim_off(client, WINDOW_END, WINDOW_END)
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and any(r["metric"] == "reboard-not-taken" for r in answer["reasons"])


@pytest.mark.parametrize("landings,status", [(2, "PASS"), (3, "DESYNC")])
def test_landing_off_the_deck_floor(landings, status):
    host, client = fixture_logs()
    for cycle in range(landings):
        start = ride.RIDE_JUMP + cycle * ride.RIDE_JUMP_PERIOD
        reboard(host, start + 45, start + 55, 0)
    answer = result(host, client)
    assert answer["status"] == status, answer["reasons"]
    if landings == 3:
        assert answer["reasons"][0]["metric"] == "landed-off-floor" and answer["reasons"][0]["value"] == 3


def ended_early(host, client, reason, last=1000, both=True, victim=0):
    """The logs of a case that stopped at elapsed last on an error: nothing past it, no outcome, an end."""
    def cut(rows, side):
        kept = [r for r in rows if r["kind"] not in ("outcome", "end") and
                r.get("simulation_tick", r.get("tick")) <= BOARD + last + (0 if r["kind"] == "sample" else 1) and
                not (r["kind"] == "sample" and r["simulation_tick"] == BOARD + last and r["player"] >= victim)]
        if both or side == "host":
            kept.append({"schema": "1", "session": "fixture", "scenario": "straight", "side": side, "kind": "error",
                         "seq": 10 ** 7, "tick": BOARD + last, "reason": reason})
        kept.append({"schema": "1", "session": "fixture", "scenario": "straight", "side": side, "kind": "end",
                     "seq": 10 ** 7 + 1, "tick": BOARD + last, "players": 2})
        return kept
    return cut(host, "host"), cut(client, "client")


def test_a_rider_that_died_is_judged_not_a_tool_error():
    host, client = fixture_logs()
    lose_height(host, 900, 905, 0)
    host, client = ended_early(host, client, "rider-life-changed")
    answer = result(host, client)
    assert answer["status"] == "DESYNC", answer["reasons"]
    assert {r["metric"] for r in answer["reasons"]} == {"rider-died", "height"}


def test_another_error_is_a_tool_error():
    host, client = fixture_logs()
    host, client = ended_early(host, client, "vehicle-preparation")
    answer = result(host, client)
    assert answer["status"] == "TOOL_ERROR" and any(r["metric"] == "error" for r in answer["reasons"])
    # (nothing else missing: the error alone decides)
    host, client = fixture_logs()
    host.append({"schema": "1", "session": "fixture", "scenario": "straight", "side": "host", "kind": "error",
                 "seq": 10 ** 7, "tick": BOARD + 5, "reason": "cinematic-preparation-timeout"})
    assert result(host, client)["status"] == "TOOL_ERROR"


def correction(client, tick, player):
    anchor = next(r for r in client if r["kind"] == "sample" and r["network_tick"] == tick and r["player"] == player)
    row = {"schema": "1", "session": "fixture", "scenario": anchor["scenario"], "side": "client", "kind": "correction",
           "seq": anchor["seq"] + .1, "tick": tick, "player": player, "unit": anchor["unit"],
           "before": (0., 0., 0.), "after": (.2, 0., 0.)}
    client.append(row)


@pytest.mark.parametrize("apart,status", [(59, "DESYNC"), (60, "PASS")])
def test_rolling_corrections(apart, status):
    host, client = fixture_logs()
    for tick in (BOARD + 650, BOARD + 650 + apart):
        correction(client, tick, 1)
    assert result(host, client)["status"] == status


def test_standing_correction_is_desync():
    host, client = fixture_logs()
    correction(client, BOARD + 100, 1)
    assert result(host, client)["status"] == "DESYNC"


def test_corrections_are_per_player():
    host, client = fixture_logs()
    correction(client, BOARD + 650, 0)
    correction(client, BOARD + 650, 1)
    assert result(host, client)["status"] == "PASS"


def test_incomplete_log_is_tool_error():
    host, client = fixture_logs()
    assert result(host, [r for r in client if r["kind"] != "end"])["status"] == "TOOL_ERROR"
    assert ride.judge([], [], 0, 0)["status"] == "TOOL_ERROR"


def test_missing_inputs_are_tool_error():
    host, client = fixture_logs()
    client = [r for r in client if not (r["kind"] == "input" and r["jump"])]
    assert result(host, client)["status"] == "TOOL_ERROR"


def test_a_press_tick_asked_often_counts_once():
    host, client = fixture_logs()
    presses = [r for r in host if r["kind"] == "input" and r["jump"]]
    assert result(host + presses * 3, client)["status"] == "PASS"


def test_a_missed_press_is_not_made_up_by_a_repeated_one():
    host, client = fixture_logs()
    presses = [r for r in host if r["kind"] == "input" and r["jump"] and r["player"] == 0]
    host = [r for r in host if r is not presses[0]] + [presses[1]]
    assert result(host, client)["status"] == "TOOL_ERROR"


def test_no_echo_refused_differs_from_missing():
    host, client = fixture_logs()
    for row in client:
        if row["kind"] == "state":
            row["predicted_valid"] = 0
    assert result(host, client)["status"] == "TOOL_ERROR"
    for phase in ("stand", "walk", "jump"):
        sample = next(r for r in host if r["kind"] == "sample" and r["player"] == 1 and r["phase"] == phase)
        host.append(dict(sample, kind="prediction", seq=sample["seq"] + .1, host_tick=sample["network_tick"],
                         predicted_low16=0, eligible=1, accepted=0))
    answer = result(host, client)
    assert answer["status"] == "TOOL_ERROR"
    assert any(row["metric"] == "alignment-coverage" for row in answer["reasons"])


def test_high_latency_reported_not_required():
    host, client = fixture_logs()
    next(r for r in client if r["kind"] == "sample")["height"] = -.2
    answer = result(host, client, 150)
    assert answer["status"] == "DESYNC" and answer["required"] is False


def test_history_before_the_echo_is_applied():
    host, client = fixture_logs()
    capture = next(r for r in client if r["kind"] == "own" and r["tick"] == BOARD + 100)
    state = next(r for r in client if r["kind"] == "state" and r["player"] == 1 and r["host_tick"] == BOARD + 100)
    patch = {"schema": "1", "session": "fixture", "scenario": "straight", "side": "client", "kind": "history",
             "seq": (capture["seq"] + state["seq"]) / 2, "tick": capture["tick"], "player": 1, "unit": capture["unit"],
             "old_world": capture["world"], "new_world": tuple(capture["world"][i] + (.1, 0., 0.)[i] for i in range(3)),
             "deck_present": 0, "vehicle": -1, "node": -1, "ticks": 0, "place": (0., 0., 0.),
             "velocity": (0., 0., 0.), "window": 0, "air_episode": 0}
    client.append(patch)
    pairs = [p for p in ride.align(host, client) if p["mode"] == "own" and p["tick"] == capture["tick"]]
    assert pairs and pairs[0]["client"]["place"] == pytest.approx((.1, .5, .2))
    patch["seq"] = state["seq"] + .1
    pairs = [p for p in ride.align(host, client) if p["mode"] == "own" and p["tick"] == capture["tick"]]
    assert pairs and pairs[0]["client"]["place"] == pytest.approx((0., .5, .2))


def test_delayed_echo_uses_predicted_tick():
    host, client = fixture_logs()
    state = next(r for r in client if r["kind"] == "state" and r["player"] == 1 and r["host_tick"] == BOARD + 100)
    state["host_tick"] = BOARD + 110
    pairs = ride.align(host, client)
    assert any(p["mode"] == "own" and p["tick"] == BOARD + 100 and p["host_tick"] == BOARD + 110 for p in pairs)


def test_history_replay_turning_deck():
    import math

    angle = 10 * .05
    capture = dict(kind="own", seq=1, tick=10, player=1, unit=2, world=(0., 0., 0.), base=(0., 0., 0.),
                   place=(0., 0., 0.), velocity=(.2, 0., 0.), origin=(0., 0., 0.),
                   forward=(1., 0., 0.), left=(0., 1., 0.), up=(0., 0., 1.))
    # (the correction happened ten ticks later against the turned pose, not this capture's pose)
    patch = dict(kind="history", seq=2, tick=10, player=1, unit=2, old_world=(0., 0., 0.),
                 new_world=(math.cos(angle), math.sin(angle), 0.), deck_present=1, vehicle=65536, node=-1,
                 ticks=59, place=(1., 0., 0.), velocity=(.25, 0., 0.), window=1, air_episode=1)
    echo = dict(kind="state", seq=3, tick=20, host_tick=20, player=1, unit=2, predicted_valid=1,
                predicted_low16=10, place=(1., 0., 0.))
    pairs = ride.align([], [capture, patch, echo])
    assert len(pairs) == 1
    assert ride.distance(pairs[0]["client"]["place"], echo["place"]) < .05
    assert pairs[0]["client"]["velocity"] == (.25, 0., 0.)


def test_remote_uses_applied_host_tick():
    host, client = fixture_logs()
    state = next(r for r in host if r["kind"] == "state" and r["player"] == 0 and r["host_tick"] == BOARD + 90)
    applied = dict(next(r for r in client if r["kind"] == "apply" and r["player"] == 0), seq=1, tick=BOARD + 100,
                   applied_host_tick=BOARD + 90)
    sample = dict(next(r for r in client if r["kind"] == "sample" and r["player"] == 0), seq=2,
                  simulation_tick=BOARD + 101, network_tick=BOARD + 102)
    pairs = ride.align([state], [applied, sample])
    assert len(pairs) == 1 and pairs[0]["host_tick"] == BOARD + 90


def test_missing_applies_are_tool_error():
    host, client = fixture_logs()
    client = [r for r in client if r["kind"] != "apply"]
    answer = result(host, client)
    assert answer["status"] == "TOOL_ERROR", answer
    for phase in ("stand", "walk", "jump"):
        assert any(r["metric"] == "alignment-coverage" and r["player"] == 0 for r in answer["reasons"])
    host, client = fixture_logs()
    pairs = ride.align(host, client)
    for phase in ("stand", "walk", "jump"):
        remaining = [p for p in pairs if not (p["mode"] == "remote" and p["client"]["phase"] == phase)]
        answer = ride.judge(remaining, host + client, 100, 0)
        assert answer["status"] == "TOOL_ERROR", answer
        assert any(r["metric"] == "alignment-coverage" for r in answer["reasons"])


def test_unknown_applied_host_ticks_are_tool_error():
    host, client = fixture_logs()
    for row in client:
        if row["kind"] == "apply":
            row["applied_host_tick"] += 100000
        if row["kind"] == "sample" and not row["local"]:
            for key in ("place", "world"):
                row[key] = (row[key][0] + 5., row[key][1], row[key][2])
    answer = result(host, client)
    assert answer["status"] == "TOOL_ERROR", answer
    missing_tick = next(r["applied_host_tick"] for r in client if r["kind"] == "apply")
    assert any(r["metric"] == "missing-applied-host-state" and r["value"] == missing_tick
               for r in answer["reasons"])


def test_claim_uses_logged_grace():
    host, client = fixture_logs()
    for row in client:
        if row["kind"] == "grace":
            row["grace_ticks"] = 1
        if row["kind"] == "sample" and row["player"] == 1 and row["simulation_tick"] in (400, 401):
            row["vehicle"] = 65537
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and any(r.get("metric") == "claim-disagreement" for r in answer["reasons"])


def test_step_off_claim_exemption_expires_and_world_is_compared():
    host, client = fixture_logs()
    for row in client:
        if row.get("player") == 1 and row.get("simulation_tick", row.get("tick", 0)) >= BOARD + ride.RIDE_STEP_OFF + 20:
            if row["kind"] in ("sample", "own"):
                row["deck_present"] = 0
                row["world"] = (row["world"][0] + 1., row["world"][1], row["world"][2])
    answer = result(host, client)
    assert answer["status"] == "DESYNC"
    assert any(r["metric"] == "claim-disagreement" for r in answer["reasons"])
    pairs = ride.align(host, client)
    pair = next(p for p in pairs if p["mode"] == "own" and p["client"]["phase"] == "observe")
    pair["tick"] = BOARD + ride.RIDE_OBSERVE + 20
    pair["client"].update(board_tick=BOARD, deck_present=0, world=(1., 0., 0.))
    pair["host"].update(board_tick=BOARD, deck_present=1, world=(0., 0., 0.))
    judged = ride.judge(pairs, host + client, 100, 0)
    assert any(r["metric"] == "world" for r in judged["reasons"])


def test_pair_on_different_decks_is_compared_in_the_world():
    host, client = fixture_logs()
    pairs = ride.align(host, client)
    pair = next(p for p in pairs if p["mode"] == "own" and p["client"]["phase"] == "observe")
    pair["host"].update(vehicle=65537)
    pair["client"].update(world=(1000., 0., 0.))
    answer = ride.judge(pairs, host + client, 100, 0)
    assert answer["status"] == "DESYNC", answer
    assert any(r["metric"] == "world" for r in answer["reasons"])


def test_one_own_pair_is_not_coverage():
    host, client = fixture_logs()
    pairs = ride.align(host, client)
    stand = [p for p in pairs if p["mode"] == "own" and p["client"]["phase"] == "stand" and p["tick"] >= BOARD + 60]
    kept = [p for p in pairs if not (p["mode"] == "own" and p["client"]["phase"] == "stand")] + stand[:1]
    answer = ride.judge(kept, host + client, 100, 0)
    assert answer["status"] == "TOOL_ERROR", answer
    assert any(r["metric"] == "alignment-coverage" and r["value"] < .5 for r in answer["reasons"])
    assert answer["metrics"]["alignment_coverage"]["own:1:stand"] < .5


def test_remote_coverage_counts_settled_pairs_only():
    host, client = fixture_logs()
    pairs = ride.align(host, client)
    stand = [p for p in pairs if p["mode"] == "remote" and p["client"]["phase"] == "stand"]
    stand.sort(key=lambda p: p["client"]["simulation_tick"])
    # (285 pairs would be half of 570 settled samples, but the first 30 are before the settle: 256 settled)
    kept = [p for p in pairs if p not in stand] + stand[:285]
    answer = ride.judge(kept, host + client, 100, 0)
    assert answer["status"] == "TOOL_ERROR", answer
    assert answer["metrics"]["alignment_coverage"]["remote:0:stand"] == pytest.approx(256 / 570)
    assert any(r["metric"] == "alignment-coverage" for r in answer["reasons"])


def test_own_coverage_counts_settled_pairs_only():
    host, client = fixture_logs()
    pairs = ride.align(host, client)
    stand = sorted((p for p in pairs if p["mode"] == "own" and p["client"]["phase"] == "stand"), key=lambda p: p["tick"])
    kept = [p for p in pairs if p not in stand] + stand[:285]
    answer = ride.judge(kept, host + client, 100, 0)
    assert answer["status"] == "TOOL_ERROR", answer
    assert answer["metrics"]["alignment_coverage"]["own:1:stand"] < .5


def production_shaped(host, client):
    """The own capture's tick is the tick the host echoes back (one under the state's), not the sample's network tick."""
    for row in client:
        if row["kind"] == "own":
            row["tick"] -= 1
    for row in host + client:
        if row["kind"] == "state":
            row["predicted_low16"] = (row["host_tick"] - 1) & 0xffff


def settled_stand_pairs(pairs):
    return sorted((p for p in pairs if p["mode"] == "own" and p["client"]["phase"] == "stand" and
                   p["client"]["tick"] >= BOARD + ride.SETTLE_TICKS), key=lambda p: p["tick"])


def test_own_coverage_uses_sample_ticks():
    host, client = fixture_logs()
    production_shaped(host, client)
    pairs = ride.align(host, client)
    stand = settled_stand_pairs(pairs)
    assert len(stand) == 570
    # (a capture no sample answers is no pairing: 284 of 570, not 285)
    stand[284]["client"]["tick"] = stand[284]["tick"] = BOARD + 5000
    kept = [p for p in pairs if not (p["mode"] == "own" and p["client"]["phase"] == "stand")] + stand[:285]
    answer = ride.judge(kept, host + client, 100, 0)
    assert answer["status"] == "TOOL_ERROR", answer
    assert answer["metrics"]["alignment_coverage"]["own:1:stand"] == pytest.approx(284 / 570)
    assert any(r["metric"] == "alignment-coverage" and r["value"] < .5 for r in answer["reasons"])
    # (a duplicated sample is one sample, the first)
    pairs = ride.align(host, client)
    stand = settled_stand_pairs(pairs)
    kept = [p for p in pairs if not (p["mode"] == "own" and p["client"]["phase"] == "stand")] + stand[:285]
    twin = dict(next(r for r in client if r["kind"] == "sample" and r["local"] and r["simulation_tick"] == BOARD + 400),
                seq=10 ** 9)
    answer = ride.judge(kept, host + client + [twin], 100, 0)
    assert answer["metrics"]["alignment_coverage"]["own:1:stand"] == pytest.approx(285 / 570)
    assert not any(r["metric"] == "alignment-coverage" for r in answer["reasons"]), answer["reasons"]


def disagree(host, first, count, flags=ride.ENCLOSED << 1, ticks=None):
    """The host's states for host ticks first.. (count of them) say a verdict the client's samples do not."""
    for row in host:
        if row["kind"] == "state" and first <= row["host_tick"] < first + count:
            row["flags"] = flags
            if ticks is not None:
                row["ticks"] = ticks


def test_verdict_disagreement_is_desync():
    host, client = fixture_logs()
    disagree(host, BOARD + 100, 20)
    answer = result(host, client)
    assert answer["status"] == "DESYNC", answer
    assert any(r["metric"] == "verdict-disagreement" for r in answer["reasons"])
    host, client = fixture_logs()
    disagree(host, BOARD + 100, 10)
    assert result(host, client)["status"] == "PASS"
    # (the host's riding verdict against the client's air verdict is no pair of one phase)
    host, client = fixture_logs()
    disagree(host, BOARD + 100, 20, ticks=40)
    assert result(host, client)["status"] == "PASS"
    host, client = fixture_logs()
    disagree(host, BOARD + 100, 20)
    for row in host:
        if row["kind"] == "state" and BOARD + 100 <= row["host_tick"] < BOARD + 120:
            row["vehicle"] = 65537
    assert not any(r["metric"] == "verdict-disagreement" for r in result(host, client)["reasons"])


def longsword_verdicts(count):
    """Longsword's run, and the host's states for its rider saying enclosed against the client's open samples."""
    host, client = fixture_longsword()
    for side, rows in (("host", host), ("client", client)):
        rows.append({"schema": "1", "session": "fixture", "scenario": "longsword", "side": side, "kind": "grace",
                     "seq": 10 ** 6, "tick": BOARD, "player": 1, "grace_ticks": 15})
    number = 10 ** 6
    for tick in range(BOARD + 3000, BOARD + 3000 + count):
        number += 3
        common = {"schema": "1", "session": "fixture", "scenario": "longsword", "player": 1, "unit": 131073,
                  "deck_present": 1, "vehicle": 65536, "node": -1, "ticks": 60, "velocity": (0., 0., 0.), "peak": 0.,
                  "world": (0., 0., 0.), "place": (0., 0., .2)}
        host.append(dict(common, side="host", kind="state", seq=number, tick=tick + 1, host_tick=tick + 1,
                         predicted_valid=0, predicted_low16=0, flags=ride.ENCLOSED << 1))
        client.append(dict(common, side="client", kind="apply", seq=number + 1, tick=tick + 1, applied_host_tick=tick + 1))
        client.append(dict(common, side="client", kind="sample", seq=number + 2, simulation_tick=tick, network_tick=tick + 1,
                           local=0, phase="observe", flags=0, board_tick=BOARD))
    return host, client


def test_longsword_judges_verdict_disagreement():
    host, client = longsword_verdicts(20)
    answer = longsword(host, client)
    assert answer["status"] == "DESYNC", answer
    assert any(r["metric"] == "verdict-disagreement" for r in answer["reasons"])
    host, client = longsword_verdicts(10)
    assert longsword(host, client)["status"] == "PASS"


def test_coverage_is_reported_for_every_scored_phase():
    host, client = fixture_logs()
    answer = result(host, client)
    assert answer["status"] == "PASS"
    for phase in ("stand", "walk", "jump"):
        assert answer["metrics"]["alignment_coverage"][f"own:1:{phase}"] >= .5
        assert answer["metrics"]["alignment_coverage"][f"remote:0:{phase}"] >= .5


def test_scored_phase_requires_half_aligned_samples():
    host, client = fixture_logs()
    pairs = ride.align(host, client)
    sparse = [p for p in pairs if not (p["mode"] == "own" and p["client"]["phase"] == "stand")]
    answer = ride.judge(sparse, host + client, 100, 0)
    assert answer["status"] == "TOOL_ERROR"
    assert any(r["metric"] == "alignment-coverage" and r["value"] < .5 for r in answer["reasons"])
    assert answer["metrics"]["alignment_coverage"]


def test_runner_phase_constants_match_network_test_source():
    source = (Path(__file__).resolve().parents[1] / "port/linux/game/network_test.c").read_text()
    import re
    for name in ("RIDE_WALK", "RIDE_JUMP", "RIDE_JUMP_PERIOD", "RIDE_JUMPS", "RIDE_CROSS", "RIDE_STEP_OFF",
                 "RIDE_OBSERVE", "RIDE_END"):
        expected = int(re.search(r"\b" + name + r"\s*=\s*(\d+)", source).group(1))
        assert getattr(ride, name) == expected


def test_coop_driver_missing_reads_logged_field_not_driver_index():
    host, client = fixture_logs("coop")
    next(r for r in client if r["kind"] == "prepare").update(driver=196608, driver_missing=1)
    answer = result(host, client, 0)
    assert any(r["metric"] == "driver-seat-missing" for r in answer["reasons"])


def death_logs(victim, host_last=900, client_last=902, off_before=(True, True), sides=("host", "client")):
    """Both machines' logs of a rider dying: its samples end the tick before the error, earlier riders' at it (the
    logger walks riders in index order), off the deck on each machine as off_before says. A side not in sides never
    saw the death."""
    host, client = fixture_logs()
    out = []
    shared = BOARD + min(host_last, client_last) + 1
    for rows, side, last, off in ((host, "host", host_last, off_before[0]), (client, "client", client_last, off_before[1])):
        end = BOARD + last
        # (the host's states, and the client's applies of them, end with the earlier death)
        # (one more host state than the applies, so the last remote world pair still has its successor)
        kept = [r for r in rows if r["kind"] not in ("outcome", "end") and
                r.get("simulation_tick", r.get("tick")) <= (shared + (side == "host" and r["kind"] == "state")
                                                            if r["kind"] in ("state", "apply") else
                                                            end + (0 if r["kind"] == "sample" else 1))]
        if side in sides:
            kept = [r for r in kept if not (r["kind"] == "sample" and r["player"] >= victim and r["simulation_tick"] >= end)]
        for row in kept:
            if row["kind"] == "sample" and row["player"] == victim and off and row["simulation_tick"] < end:
                row["deck_present"] = 0
            elif row["kind"] == "state" and row["player"] == victim and off_before[0] and row["tick"] <= end:
                row["deck_present"] = 0
        if side in sides:
            kept.append({"schema": "1", "session": "fixture", "scenario": "straight", "side": side, "kind": "error",
                         "seq": 10 ** 7, "tick": end, "reason": "rider-life-changed"})
        kept.append({"schema": "1", "session": "fixture", "scenario": "straight", "side": side, "kind": "end",
                     "seq": 10 ** 7 + 1, "tick": end, "players": 2})
        out.append(kept)
    return out


@pytest.mark.parametrize("victim", (0, 1))
def test_agreed_off_deck_death_is_scene_tool_error(victim):
    host, client = death_logs(victim)
    answer = result(host, client)
    assert answer["status"] == "TOOL_ERROR"
    metrics = {r["metric"] for r in answer["reasons"]}
    assert "rider-died-agreed" in metrics, answer["reasons"]
    assert "rider-died" not in metrics and "error" not in metrics


def test_one_sided_death_remains_desync():
    host, client = death_logs(1, sides=("host",))
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and any(r["metric"] == "rider-died" for r in answer["reasons"])


@pytest.mark.parametrize("victim", (0, 1))
def test_agreed_death_with_deck_disagreement_remains_desync(victim):
    host, client = death_logs(victim, off_before=(False, True))
    answer = result(host, client)
    assert answer["status"] == "DESYNC", answer["reasons"]
    assert any(row["metric"] == "rider-died" for row in answer["reasons"])


def test_death_deck_judged_for_the_dead_rider_not_a_living_one():
    # (the living rider 0 off the deck on both copies must not make rider 1's disagreeing death look agreed)
    host, client = death_logs(1, off_before=(False, True))
    for rows in (host, client):
        for row in rows:
            if row["kind"] == "sample" and row["player"] == 0:
                row["deck_present"] = 0
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and any(r["metric"] == "rider-died" for r in answer["reasons"])
    assert not any(r["metric"] == "rider-died-agreed" for r in answer["reasons"])


def test_deaths_of_different_riders_are_not_agreed():
    host, _ = death_logs(1)
    _, client = death_logs(0)
    # (rider 1 also off the deck on the client, so only the riders differ)
    for row in client:
        if row["kind"] == "sample" and row["player"] == 1:
            row["deck_present"] = 0
    answer = result(host, client)
    assert answer["status"] == "DESYNC" and any(r["metric"] == "rider-died" for r in answer["reasons"])
    assert not any(r["metric"] == "rider-died-agreed" for r in answer["reasons"])


def test_death_beyond_the_claim_grace_is_not_agreed():
    host, client = death_logs(1, host_last=900, client_last=900 + 16)
    answer = result(host, client)
    assert not any(r["metric"] == "rider-died-agreed" for r in answer["reasons"])
    assert any(r["metric"] == "rider-died" for r in answer["reasons"])


def test_death_without_an_identifiable_rider_is_tool_error():
    host, client = death_logs(1)
    # (every rider sampled at the death tick: nobody's samples ended there)
    for rows in (host, client):
        for player in (0, 1):
            template = next(r for r in reversed(rows) if r["kind"] == "sample" and r["player"] == player)
            rows.append(dict(template, simulation_tick=template["simulation_tick"] + 1, seq=10 ** 6 + player))
    answer = result(host, client)
    assert answer["status"] == "TOOL_ERROR"
    metrics = {r["metric"] for r in answer["reasons"]}
    assert "rider-died-unattributed" in metrics
    assert "rider-died-agreed" not in metrics


def test_main_turns_unexpected_judge_exception_into_tool_error(tmp_path, monkeypatch):
    import json
    monkeypatch.setattr(ride, "run_pair", lambda *args: (tmp_path / "host", tmp_path / "client"))
    monkeypatch.setattr(ride, "parse_log", lambda path: [{"kind": "anything"}])
    monkeypatch.setattr(ride, "align", lambda *args: [])
    def broken(*args):
        raise KeyError("judge-fixture")
    monkeypatch.setattr(ride, "judge", broken)
    try:
        status = ride.main(["--output", str(tmp_path / "output"), "--scenarios", "straight", "--rtts", "0",
                            "--loss-case", "none"])
    except Exception as error:
        assert False, f"unexpected exception escaped: {error}"
    assert status == 2
    summary = json.loads((tmp_path / "output/summary.json").read_text())
    assert summary[0]["status"] == "TOOL_ERROR"
    assert "KeyError: 'judge-fixture'" in summary[0]["reasons"][0]


def test_gun_enclosed_verdict_is_desync():
    host, client = fixture_logs("gun")
    for row in host + client:
        if row["kind"] == "sample" and row["phase"] == "jump":
            row.update(ticks=40, flags=1 | (1 << 1))
    assert result(host, client)["status"] == "PASS"
    next(r for r in client if r["kind"] == "sample" and r["phase"] == "jump").update(flags=1 | (2 << 1))
    assert result(host, client)["status"] == "DESYNC"


def test_gun_without_verdict_is_tool_error():
    host, client = fixture_logs("gun")
    assert result(host, client)["status"] == "TOOL_ERROR"


def push(rows, player):
    """A rider moved 0.2 out along the hull's outward direction (+y) after the first watched tick, as the vehicle
    brushes it."""
    watched = [r for r in rows if r["kind"] == "sample" and r["player"] == player and r["phase"] == "observe"]
    for row in watched[1:]:
        row["world"] = (row["world"][0], row["world"][1] + .2, row["world"][2])


def test_brush_one_machine_is_desync():
    host, client = fixture_logs("brush")
    for player in (0, 1):
        push(host, player)
        push(client, player)
    assert result(host, client)["status"] == "PASS"
    host, client = fixture_logs("brush")
    push(host, 0)
    push(host, 1)
    push(client, 0)
    assert result(host, client)["status"] == "DESYNC"
    host, client = fixture_logs("brush")
    assert result(host, client)["status"] == "TOOL_ERROR"


def test_brush_push_is_directional():
    import random
    jitter = random.Random(7)
    host, client = fixture_logs("brush")
    for rows in (host, client):
        for player in (0, 1):
            watched = [r for r in rows if r["kind"] == "sample" and r["player"] == player and r["phase"] == "observe"]
            for row in watched[1:-1]:
                angle = jitter.uniform(0., 6.283)
                row["world"] = (row["world"][0] + .08 * ride.math.cos(angle), row["world"][1] + .08 * ride.math.sin(angle),
                                row["world"][2])
    answer = result(host, client)
    assert answer["status"] == "DESYNC", answer
    assert {(r["side"], r["player"]) for r in answer["reasons"] if r["metric"] == "brush-no-push"} == {
        (side, player) for side in ("host", "client") for player in (0, 1)}
    host, client = fixture_logs("brush")
    for rows in (host, client):
        for player in (0, 1):
            push(rows, player)
    assert result(host, client)["status"] == "PASS"


def test_brush_rider_at_the_hull_origin_has_no_direction():
    host, client = fixture_logs("brush")
    for row in host + client:
        if row["kind"] == "sample" and row["phase"] == "observe":
            row["world"] = row["origin"]
    answer = result(host, client)
    assert answer["status"] == "TOOL_ERROR", answer
    assert any(r["metric"] == "brush-no-direction" for r in answer["reasons"])


def test_driven_rider_hurt_is_desync():
    host, client = fixture_logs("driven")
    host = [r for r in host if not (r.get("player") == 0 and r["kind"] in ("sample", "input", "outcome", "phase",
                                                                           "state", "grace"))]
    client = [r for r in client if not (r.get("player") == 0 and r["kind"] in ("sample", "input", "outcome", "phase",
                                                                               "state", "apply", "grace"))]
    for row in host + client:
        if row["kind"] in ("end", "prepare"):
            row["players"] = 1
    assert result(host, client)["status"] == "PASS"
    next(r for r in client if r["kind"] == "sample" and r["simulation_tick"] == 1200)["shield"] = .5
    assert result(host, client)["status"] == "DESYNC"


def test_correction_share_reported():
    host, client = fixture_logs()
    for row in client:
        if row["kind"] == "vehicle":
            row["written"] = .01
    share = result(host, client)["metrics"]["correction_share"]
    assert share["client:65536"] == pytest.approx(.25) and share["host:65536"] == pytest.approx(0.)


def test_coop_double_not_required():
    host, client = fixture_logs("coop-double")
    assert result(host, client, 0)["required"] is False


def test_coop_client_missing_driver_is_desync():
    host, client = fixture_logs("coop")
    next(r for r in client if r["kind"] == "prepare").update(driver=-1, driver_missing=1)
    answer = result(host, client, 0)
    assert answer["status"] == "DESYNC"
    assert any(r["metric"] == "driver-seat-missing" and r["side"] == "client" for r in answer["reasons"])


def test_coop_moving_host_without_client_write_is_desync():
    host, client = fixture_logs("coop")
    next(r for r in client if r["kind"] == "vehicle" and r["tick"] == BOARD + 10)["carries"] = 0
    answer = result(host, client, 0)
    assert answer["status"] == "DESYNC"
    assert any(r["metric"] == "no-extrapolation" and r["tick"] == BOARD + 10 for r in answer["reasons"])


def test_coop_stationary_host_needs_no_client_write():
    host, client = fixture_logs("coop")
    next(r for r in host if r["kind"] == "vehicle" and r["tick"] == BOARD + 10)["moved"] = 0
    next(r for r in client if r["kind"] == "vehicle" and r["tick"] == BOARD + 10)["carries"] = 0
    assert result(host, client, 0)["status"] == "PASS"


def client_vehicle(client, tick):
    return next(r for r in client if r["kind"] == "vehicle" and r["tick"] == tick)


def test_no_extrapolation_uses_carries():
    host, client = fixture_logs("coop")
    for tick in (BOARD + 9, BOARD + 10):
        client_vehicle(client, tick).update(writes=2, carries=0)
    answer = result(host, client, 0)
    assert answer["status"] == "DESYNC", answer
    assert {r["tick"] for r in answer["reasons"] if r["metric"] == "no-extrapolation"} == {BOARD + 9, BOARD + 10}
    host, client = fixture_logs("coop")
    client_vehicle(client, BOARD + 10).update(writes=0, carries=1)
    assert result(host, client, 0)["status"] == "PASS"
    # (a host row moving only on that tick, the tick before standing still: nothing yet to carry)
    host, client = fixture_logs("coop")
    next(r for r in host if r["kind"] == "vehicle" and r["tick"] == BOARD + 9)["moved"] = 0
    client_vehicle(client, BOARD + 10).update(writes=2, carries=0)
    assert result(host, client, 0)["status"] == "PASS"
    host, client = fixture_logs("coop")
    for row in client:
        if row["kind"] == "vehicle":
            row["carries"] = 0
    answer = result(host, client, 0)
    assert answer["status"] == "TOOL_ERROR", answer
    assert any(r["metric"] == "carry-never-ran" and r["side"] == "client" for r in answer["reasons"])


def test_watched_runs_open_windows(tmp_path):
    import os
    if os.name != "nt":
        pytest.skip("real Windows data-root junctions")
    source = tmp_path / "source"
    (source / "maps").mkdir(parents=True)
    halo = source / "halo.exe"
    halo.write_bytes(b"test executable, not launched")
    hidden, _ = ride.make_side(halo, source, tmp_path / "hidden", "host")
    shown, _ = ride.make_side(halo, source, tmp_path / "shown", "host", visible=True)
    assert "hidden_window = true" in (hidden.parent / "config.toml").read_text()
    config = (shown.parent / "config.toml").read_text()
    assert "hidden_window = true" not in config and 'mode = "windowed"' in config
    import inspect
    assert inspect.signature(ride.run_pair).parameters["visible"].default is False
    # (any value of HALO_HIDDEN_WINDOW hides the window, over the config)
    arguments = ("host", tmp_path, tmp_path, "straight", 0, 0, "127.0.0.200", "127.0.0.201", "host")
    assert ride.child_environment(*arguments)["HALO_HIDDEN_WINDOW"] == "1"
    assert "HALO_HIDDEN_WINDOW" not in ride.child_environment(*arguments, visible=True)


def test_coop_clock_transport_is_shared_but_case_scoped(tmp_path):
    import os
    if os.name != "nt":
        pytest.skip("real Windows data-root junctions")
    source = tmp_path / "source"
    source.mkdir()
    (source / "maps").mkdir()
    halo = source / "halo.exe"
    halo.write_bytes(b"test executable, not launched")
    root = tmp_path / "case"
    clocks = []
    for side in ("host", "client"):
        _, data = ride.make_side(halo, source, root, side)
        env = ride.child_environment(side, data, root, "coop", 0, 0, "127.0.0.200", "127.0.0.201", "host")
        # Game stdio translates Xbox d: paths below each process's data root.
        name = env.get("HALO_NETWORK_TEST_RIDE_CLOCK", "")
        assert name.lower().startswith("d:\\")
        clock = data / name[3:].replace("\\", "/")
        assert clock.parent.is_dir(), "clock route not exposed to game stdio"
        clocks.append(clock)
    clocks[0].write_text("session 1122\n")
    assert clocks[1].is_file() and clocks[1].read_text() == "session 1122\n"
    _, other_data = ride.make_side(halo, source, tmp_path / "other", "host")
    other = other_data / "ride-clock/board.txt"
    assert not other.exists(), "another case receives this case's clock"


def test_board_clock_disagreement_is_tool_error():
    host, client = fixture_logs("coop")
    next(row for row in client if row["kind"] == "prepare")["board_tick"] += 1
    answer = result(host, client, 0)
    assert answer["status"] == "TOOL_ERROR"
    assert any(row["metric"] == "board-clock-disagreement" for row in answer["reasons"])


def test_side_environment_isolation_and_units(tmp_path):
    host = ride.child_environment("host", tmp_path / "host/data", tmp_path, "straight", 100, 2, "127.0.0.200",
                                  "127.0.0.201", "host")
    client = ride.child_environment("client", tmp_path / "client/data", tmp_path, "straight", 100, 2, "127.0.0.200",
                                    "127.0.0.201", "host")
    assert host["HALO_NETWORK_LATENCY"] == client["HALO_NETWORK_LATENCY"] == "50.0"
    assert host["HALO_NETWORK_LOSS"] == client["HALO_NETWORK_LOSS"] == "2"
    assert host["HALO_DATA_ROOT"] != client["HALO_DATA_ROOT"] and host["HALO_SAVE_ROOT"] != client["HALO_SAVE_ROOT"]
    assert host["HALO_HIDDEN_WINDOW"] == client["HALO_HIDDEN_WINDOW"] == "1"
    assert host["HALO_NETWORK_TEST"].startswith("host:") and client["HALO_NETWORK_TEST"] == "join"


def test_stop_children_only_retained_handles():
    from unittest.mock import patch

    class Child:
        def __init__(self):
            self.stopped = False

        def poll(self):
            return 0 if self.stopped else None

        def terminate(self):
            self.stopped = True

        def wait(self, timeout):
            return 0

        def kill(self):
            raise AssertionError("not needed")

    ours, unrelated = Child(), Child()
    with patch.object(ride.subprocess, "run", side_effect=AssertionError("a process command run")):
        ride.stop_children([ours])
    assert ours.stopped and not unrelated.stopped


def write_records(path, records):
    def value(v):
        if isinstance(v, tuple):
            return ",".join(str(x) for x in v)
        return "none" if v is None else str(v)
    path.write_text("\n".join("ride " + " ".join(f"{k}={value(v)}" for k, v in row.items()) for row in records),
                    encoding="utf-8")


@pytest.mark.parametrize("fault", ["truncate", "nan", "schema", "duplicate"])
def test_parser_refuses_broken_records(tmp_path, fault):
    host, _ = fixture_logs()
    row = copy.deepcopy(next(r for r in host if r["kind"] == "sample"))
    rows = [row]
    if fault == "truncate":
        del row["world"]
    elif fault == "nan":
        row["world"] = (float("nan"), 0., 0.)
    elif fault == "schema":
        row["schema"] = "2"
    else:
        rows.append(dict(row, height=-.2))
    path = tmp_path / "debug.txt"
    write_records(path, rows)
    with pytest.raises(ride.ToolError):
        ride.parse_log(path)


def test_parser_reads_its_own_records(tmp_path):
    host, _ = fixture_logs()
    path = tmp_path / "debug.txt"
    write_records(path, host[:50])
    assert len(ride.parse_log(path)) == 50


def test_parser_requires_driver_missing_field(tmp_path):
    host, _ = fixture_logs("coop")
    row = dict(host[0])
    del row["driver_missing"]
    path = tmp_path / "debug.txt"
    write_records(path, [row])
    try:
        ride.parse_log(path)
    except ride.ToolError:
        return
    assert False, "prepare driver_missing field is optional"


LONGSWORD_BOX = ((-1.0, -0.6, 0.0), (1.0, 0.6, 1.5))


def fixture_longsword(outside_from=None, hurt_at=None, door_out=True, lag=0.0, frozen_updates=False, died_at=None,
                      failed_at=None, refusal_at=None, refusal_kind=4, limit=None, wing=False):
    """Both machines' records of a Longsword run: the rider inside the box until the door walk (from BOARD + 300),
    then out of it and off the deck."""
    logs = {"host": [], "client": []}
    seq = {"host": 0, "client": 0}

    def emit(side, kind, **values):
        seq[side] += 1
        row = {"schema": "1", "session": "fixture", "scenario": "longsword", "side": side, "kind": kind, "seq": seq[side]}
        row.update(values)
        logs[side].append(row)

    for side in logs:
        emit(side, "prepare", tick=BOARD, players=1, vehicles="65536,-1", driver=196608, driver_player=0, board_tick=BOARD,
             driver_missing=0)
    failed_ticks = {failed_at} if isinstance(failed_at, int) else set(failed_at or ())
    starts = (0, 300, 1200, 1500, 2400, 2700, 3000, 3300, 3600, 3900, 4200, 4500, 8100)
    for tick in range(BOARD, BOARD + 8251):
        if died_at is not None and tick > died_at:
            break
        step = max(i for i, start in enumerate(starts) if tick - BOARD >= start)
        door = int(step == 12)
        outside = (outside_from is not None and tick >= outside_from) or (door and door_out and tick >= BOARD + 8150)
        for side in logs:
            emit(side, "tilt", tick=tick, step=step, hold=tick - BOARD - starts[step], door=door, descending=int(step == 11),
                 target_pitch=40.0, target_roll=0.0, error=lag, pitch=40.0, roll=30.0,
                 tilt=48.44 if step in (5, 7, 9) else 0.0, grounded=60 if step >= 11 else 0, speed=0.0,
                 throttle=(1.0, 0.0), facing=(0.0, 0.7), updates=1000 if frozen_updates else 1000 + tick)
            emit(side, "cabin", tick=tick, player=1, unit=131073, pitch=40.0, roll=0.0, tilt=40.0, uk=0.766, m=0.5,
                 limit=limit if limit is not None else ride.LONGSWORD_LIMIT if ride.LONGSWORD_LIMIT is not None else 0.7071,
                 slope=60.0, ticks=0 if outside else 60, airborne=0, ground_k=0.766, old_hit=1,
                 old_distance=0.5, old_inside=0, place=(2.0, 0.0, 0.2) if outside or wing else (0.0, 0.0, 0.2),
                 body=0.5 if hurt_at is not None and tick >= hurt_at else 1.0, shield=1.0, corrections=0,
                 axis=0, raw=1, inside=0, ceiling=1, ceiling_distance=0.5, floor=1, floor_distance=0.2,
                 standing=2, misses=0, jump=0, take_off=0, exception=0, carry=0,
                 settle=6 if tick in failed_ticks else 0, rejected=0, anchor=1, verdict=2,
                 refusal=refusal_kind if tick == refusal_at else 0)
    for side in logs:
        if died_at is not None:
            emit(side, "error", tick=died_at, reason="rider-life-changed")
        emit(side, "end", tick=BOARD + 8251, players=1)
    return logs["host"], logs["client"]


def longsword(host, client, box=LONGSWORD_BOX):
    return ride.judge_longsword(host + client, 0, 0, box)


def test_longsword_clean_passes():
    host, client = fixture_longsword()
    answer = longsword(host, client)
    assert answer["status"] == "PASS", answer["reasons"]


def test_longsword_ejection_is_a_failure():
    host, client = fixture_longsword(outside_from=BOARD + 2800)
    answer = longsword(host, client)
    assert answer["status"] == "DESYNC"
    assert any(r["metric"] == "ejected" for r in answer["reasons"])


def test_longsword_short_outside_is_not_an_ejection():
    host, client = fixture_longsword(outside_from=BOARD + 8090)
    assert longsword(host, client)["status"] == "PASS"


def test_longsword_health_lost_is_reported_not_failed():
    host, client = fixture_longsword(hurt_at=BOARD + 50)
    answer = longsword(host, client)
    assert answer["status"] == "PASS", answer["reasons"]
    assert answer["metrics"]["health_lost"]["host:1"] == 0.5


def test_longsword_death_is_a_failure():
    host, client = fixture_longsword(died_at=BOARD + 2850)
    assert any(r["metric"] == "rider-died" for r in longsword(host, client)["reasons"])


def test_longsword_death_in_the_door_walk_is_reported():
    host, client = fixture_longsword(died_at=BOARD + 8160)
    answer = longsword(host, client)
    assert not any(r["metric"] == "rider-died" for r in answer["reasons"])
    assert answer["metrics"]["died_in_door_walk"]


def test_longsword_door_blocked_is_a_failure():
    host, client = fixture_longsword(door_out=False)
    assert any(r["metric"] == "door-blocked" for r in longsword(host, client)["reasons"])


def test_longsword_pilot_lag_is_not_a_tool_error():
    host, client = fixture_longsword(lag=6.0)
    answer = longsword(host, client)
    assert answer["status"] == "PASS", answer["reasons"]
    assert answer["metrics"]["max_total_tilt"] == pytest.approx(48.44)


@pytest.mark.parametrize("angle,status", [(45.0, "TOOL_ERROR"), (45.001, "PASS")])
def test_longsword_requires_measured_tilt_past_limit(angle, status):
    host, client = fixture_longsword()
    for row in host + client:
        if row["kind"] == "tilt" and row["step"] in (5, 7, 9):
            row["tilt"] = angle
    answer = longsword(host, client)
    assert answer["status"] == status, answer["reasons"]
    if status == "TOOL_ERROR":
        assert any(r["metric"] == "tilt-not-reached" for r in answer["reasons"])


def test_longsword_uncounted_updates_are_a_tool_error():
    host, client = fixture_longsword(frozen_updates=True)
    assert longsword(host, client)["status"] == "TOOL_ERROR"


def test_longsword_tilt_coverage_uses_the_hosts_counted_programme():
    host, client = fixture_longsword()
    for row in client:
        if row["kind"] == "tilt":
            row["tilt"] = 0.0
    answer = longsword(host, client)
    assert answer["status"] == "PASS", answer["reasons"]
    assert answer["metrics"]["max_total_tilt"] == pytest.approx(48.44)
    for row in host:
        if row["kind"] == "tilt" and row["step"] in (5, 7, 9):
            row["tilt"] = 45.0
    for row in client:
        if row["kind"] == "tilt" and row["step"] in (5, 7, 9):
            row["tilt"] = 90.0
    answer = longsword(host, client)
    assert answer["status"] == "TOOL_ERROR"
    assert answer["metrics"]["max_total_tilt"] == 45.0


def test_longsword_tipping_during_the_door_walk_is_not_tilt_coverage():
    host, client = fixture_longsword()
    for row in host + client:
        if row["kind"] == "tilt" and (row["step"] in (5, 7, 9) or row["door"]):
            row["tilt"] = 90.0 if row["door"] else 45.0
    answer = longsword(host, client)
    assert answer["status"] == "TOOL_ERROR", answer["reasons"]
    assert answer["metrics"]["max_total_tilt"] == 45.0


def test_longsword_without_a_box_judges_the_deck():
    host, client = fixture_longsword(outside_from=BOARD + 2800)
    assert any(r["metric"] == "ejected" for r in longsword(host, client, None)["reasons"])


def test_longsword_environment(tmp_path):
    env = ride.child_environment("host", tmp_path / "data", tmp_path, "longsword", 0, 0, "127.0.0.200", "127.0.0.201",
                                 "host", spot="1,2,0.5,0")
    assert env["HALO_NETWORK_TEST"] == "host:custom_maps\\extinction:slayer"
    assert env["HALO_NETWORK_TEST_RIDE_SPOT"] == "1,2,0.5,0"
    assert env["HALO_EXIT_AFTER"] == "420"
    assert "longsword" not in ride.STOCK_SCENARIOS and "longsword" in ride.SCENARIOS


@pytest.mark.parametrize("bad", ["outside", "no-contact", "short"])
def test_longsword_preparation_requires_standing_inside_on_each_side(bad):
    host, client = fixture_longsword()
    # A remote view is judged too: ticks60 outside the cabin is not preparation.
    for row in client:
        if row["kind"] == "cabin" and BOARD + 2640 <= row["tick"] < BOARD + 2700:
            if bad == "outside":
                row["place"] = (0.0, 0.0, 20.0)
            elif bad == "no-contact":
                row["ticks"] = 59
            elif row["tick"] < BOARD + 2680:
                row["ticks"] = 0
    answer = longsword(host, client)
    assert answer["status"] == "TOOL_ERROR", answer["reasons"]
    assert any(r["metric"] == "longsword-preparation" and r["side"] == "client" for r in answer["reasons"])


def test_longsword_pretilt_loss_is_not_ejection_evidence():
    host, client = fixture_longsword(outside_from=BOARD)
    answer = longsword(host, client)
    assert answer["status"] == "TOOL_ERROR", answer["reasons"]
    assert any(r["metric"] == "longsword-preparation" for r in answer["reasons"])
    assert not any(r["metric"] == "ejected" for r in answer["reasons"])
    assert not answer["metrics"]["ejection"]


def test_longsword_two_side_phase_skew_is_tool_error():
    host, client = fixture_longsword()
    for row in client:
        if row["kind"] == "tilt" and row["tick"] >= BOARD + 4700:
            row["door"] = 1
    answer = longsword(host, client)
    assert answer["status"] == "TOOL_ERROR", answer["reasons"]
    assert any(r["metric"] == "longsword-phase-skew" for r in answer["reasons"])


def test_door_walk_needs_landing():
    host, client = fixture_longsword(door_out=False)
    for row in host:
        if row["kind"] == "tilt":
            row["grounded"] = 0
    answer = longsword(host, client)
    assert answer["status"] == "TOOL_ERROR", answer["reasons"]
    assert answer["metrics"]["door_walk"] == "door-not-run"
    assert any(r["metric"] == "door-not-run" for r in answer["reasons"]), answer
    assert not any(r["metric"] == "door-blocked" for r in answer["reasons"]), answer


def hold_rows(drop_at, exception=1, take_off=0, door_from=None):
    host, client = fixture_longsword()
    for rows in (host, client):
        for row in rows:
            if row["kind"] == "cabin" and BOARD + 3000 <= row["tick"] < BOARD + 3100:
                row["exception"] = exception
                row["take_off"] = take_off if row["tick"] == drop_at - 1 else 0
                if row["tick"] >= drop_at:
                    row["ticks"] = 59
    return host, client


def test_hold_lost_fails_an_exception_drop():
    host, client = hold_rows(BOARD + 3050)
    answer = longsword(host, client)
    assert answer["status"] == "DESYNC", answer["reasons"]
    assert any(r["metric"] == "hold-lost" for r in answer["reasons"])


def test_hold_lost_ignores_a_jumps_drop():
    host, client = hold_rows(BOARD + 3050, take_off=1)
    answer = longsword(host, client)
    assert not any(r["metric"] == "hold-lost" for r in answer["reasons"]), answer["reasons"]


def test_hold_lost_ignores_a_drop_with_no_exception():
    host, client = hold_rows(BOARD + 3050, exception=0)
    answer = longsword(host, client)
    assert not any(r["metric"] == "hold-lost" for r in answer["reasons"]), answer["reasons"]


def test_crossing_rows_reported_at_each_up_crossing():
    host, client = fixture_longsword()
    for rows in (host, client):
        for row in rows:
            if row["kind"] == "cabin" and BOARD + 3000 <= row["tick"] < BOARD + 3010:
                row["uk"] = 0.9 if row["tick"] < BOARD + 3005 else 0.6
                row["standing"] = 1 if row["tick"] < BOARD + 3005 else 2
    answer = longsword(host, client)
    crossings = answer["metrics"]["crossing_rows"]["host:1"]
    assert [c["tick"] for c in crossings][:1] == [BOARD + 3005], crossings
    assert crossings[0]["standing"] == 1 and crossings[0]["exception"] == 0 and crossings[0]["ticks"] == 60


def test_longsword_host_landing_enables_door_judging_on_both_sides():
    host, client = fixture_longsword(door_out=False)
    for row in client:
        if row["kind"] == "tilt":
            row["grounded"] = 0
    answer = longsword(host, client)
    assert answer["status"] == "DESYNC", answer["reasons"]
    assert answer["metrics"]["door_walk"] == "judged"
    assert {r["side"] for r in answer["reasons"] if r["metric"] == "door-blocked"} == {"host", "client"}


def landed_at(host, last_reset, speed=0.0, tilt=0.0):
    """The host's ship rocking until last_reset (its counter reset then), standing after."""
    for row in host:
        if row["kind"] == "tilt":
            row["grounded"] = 0 if row["tick"] <= last_reset else 60
            row["speed"] = speed
            row["tilt"] = tilt if row["tick"] > last_reset else row["tilt"]


def test_door_walk_judged_when_the_counter_resets_late():
    # (the counter is 11 at the tick before the door step, 30 within the window: the game's walk starts then)
    host, client = fixture_longsword(door_out=False)
    landed_at(host, BOARD + 8100 - 1 + 11)
    answer = longsword(host, client)
    assert answer["metrics"]["door_walk"] == "judged", answer["reasons"]
    assert not any(r["metric"] == "door-not-run" for r in answer["reasons"]), answer


def test_door_walk_not_run_when_never_standing_in_the_window():
    host, client = fixture_longsword(door_out=False)
    landed_at(host, BOARD + 8100 + 59)
    answer = longsword(host, client)
    assert answer["metrics"]["door_walk"] == "door-not-run", answer["reasons"]


def test_door_walk_not_run_while_the_ship_stays_tilted_in_the_window():
    host, client = fixture_longsword(door_out=False)
    for row in host:
        if row["kind"] == "tilt" and BOARD + 8100 - 1 <= row["tick"] <= BOARD + 8100 + 59:
            row["grounded"], row["tilt"] = 60, 6.99
    answer = longsword(host, client)
    assert answer["metrics"]["door_walk"] == "door-not-run", answer["reasons"]


def test_longsword_landing_after_door_does_not_enable_door_judging():
    host, client = fixture_longsword(door_out=False)
    for row in host:
        if row["kind"] == "tilt" and row["tick"] <= BOARD + 8100 + 59:
            row["grounded"] = 0
    answer = longsword(host, client)
    assert answer["status"] == "TOOL_ERROR", answer["reasons"]
    assert answer["metrics"]["door_walk"] == "door-not-run"


def test_longsword_parser_reads_measured_speed(tmp_path):
    path = tmp_path / "debug.txt"
    path.write_text("ride schema=1 kind=tilt session=fixture scenario=longsword side=host seq=1 tick=300 "
                    "step=0 hold=0 door=0 descending=0 pitch=0 roll=0 tilt=0 throttle=0,0 facing=0,0 "
                    "grounded=60 speed=0.0006 updates=301\n")
    assert ride.parse_log(path)[0]["speed"] == pytest.approx(0.0006)


def test_longsword_parser_reads_route_probe(tmp_path):
    path = tmp_path / "route.debug.txt"
    path.write_text("ride schema=1 kind=route session=test scenario=longsword side=host seq=1 "
                    "tick=599 height=0.000 world=-7.9,-1067.4,-7.8 old_hit=1 old_distance=46.245 "
                    "ground_k=0.0000 heading=-104.468\n")
    row = ride.parse_log(path)[0]
    assert row["old_distance"] == pytest.approx(46.245)
    assert row["heading"] == pytest.approx(-104.468)


def test_longsword_blocked_recovery_outside_is_reported():
    host, client = fixture_longsword(outside_from=BOARD + 2800, failed_at=BOARD + 2795)
    metrics = [r["metric"] for r in longsword(host, client)["reasons"]]
    assert "blocked-recovery-outside" in metrics and "ejected" not in metrics


def test_longsword_failed_recovery_damage_is_a_failure():
    host, client = fixture_longsword(failed_at=BOARD + 2795, hurt_at=BOARD + 2800)
    assert any(r["metric"] == "failed-recovery-damage" for r in longsword(host, client)["reasons"])


def test_longsword_other_damage_is_reported_only():
    host, client = fixture_longsword(failed_at=BOARD + 2700, hurt_at=BOARD + 2800)
    assert longsword(host, client)["status"] == "PASS"


def test_longsword_earlier_damage_is_not_failed_recovery_damage():
    host, client = fixture_longsword(hurt_at=BOARD + 2700, failed_at=BOARD + 2800)
    answer = longsword(host, client)
    assert answer["status"] == "PASS", answer["reasons"]
    assert not any(r["metric"] == "failed-recovery-damage" for r in answer["reasons"])
    assert answer["metrics"]["health_lost"]["host:1"] == 0.5
    assert answer["metrics"]["health_lost"]["client:1"] == 0.5


def test_longsword_snapshot_guard_is_a_tool_error():
    host, client = fixture_longsword(refusal_at=BOARD + 120, refusal_kind=4)
    assert longsword(host, client)["status"] == "TOOL_ERROR"


@pytest.mark.parametrize("kind,name", [(1, "gone"), (2, "moved-alone"), (3, "sphere")])
def test_longsword_other_refusals_are_reported(kind, name):
    host, client = fixture_longsword(refusal_at=BOARD + 120, refusal_kind=kind)
    answer = longsword(host, client)
    assert answer["status"] == "PASS", answer["reasons"]
    assert answer["metrics"].get("refusals", {}).get("host:1", {}).get(name) == 1


def test_longsword_limit_mismatch_is_a_tool_error(monkeypatch):
    monkeypatch.setattr(ride, "LONGSWORD_LIMIT", 0.7071, raising=False)
    host, client = fixture_longsword(limit=0.5)
    assert longsword(host, client)["status"] == "TOOL_ERROR"


def test_longsword_counts_recoveries():
    host, client = fixture_longsword(failed_at=[BOARD + 2800 + 10 * i for i in range(5)])
    assert longsword(host, client)["metrics"].get("recovery_failures", {}).get("host:1") == 5


def test_longsword_negative_run_enclosed_is_a_failure():
    import inspect
    assert "negative" in inspect.signature(ride.judge_longsword).parameters
    host, client = fixture_longsword()
    answer = ride.judge_longsword(host + client, 0, 0, LONGSWORD_BOX, negative=True)
    assert any(r["metric"] == "negative-enclosed" for r in answer["reasons"])


def test_longsword_negative_run_open_passes():
    import inspect
    assert "negative" in inspect.signature(ride.judge_longsword).parameters
    host, client = fixture_longsword(wing=True)
    for row in host + client:
        if row["kind"] == "cabin":
            row.update(standing=1, verdict=1, jump=0, exception=0, anchor=0)
    assert ride.judge_longsword(host + client, 0, 0, LONGSWORD_BOX, negative=True)["status"] == "PASS"


def test_stock_standing_enclosed_verdict_is_desync():
    host, client = fixture_logs()
    row = next(r for r in client if r["kind"] == "sample" and r["phase"] == "stand" and r["simulation_tick"] == 400)
    row["flags"] = 2 << 1
    assert any(r["metric"] == "stock-enclosed" for r in result(host, client)["reasons"])


def test_stock_air_legacy_verdict_is_not_stock_enclosed():
    host, client = fixture_logs()
    row = next(r for r in client if r["kind"] == "sample" and r["phase"] == "stand" and r["simulation_tick"] == 400)
    row["flags"] = 2 << 1
    row["ticks"] = 40
    assert not any(r["metric"] == "stock-enclosed" for r in result(host, client)["reasons"])


@pytest.mark.parametrize("gap", ["tail", "single", "all"])
def test_longsword_cabin_coverage_is_a_tool_error(gap):
    host, client = fixture_longsword()
    host = [r for r in host if not (r["kind"] == "cabin" and
            (r["tick"] >= BOARD + (2700 if gap == "all" else 2800) if gap != "single" else r["tick"] == BOARD + 2800))]
    answer = longsword(host, client)
    assert answer["status"] == "TOOL_ERROR", answer["reasons"]
    coverage = [r for r in answer["reasons"] if r["metric"] == "cabin-coverage"]
    assert coverage and coverage[0]["side"] == "host" and coverage[0]["player"] == 1
    assert coverage[0]["value"] == {"tail": 5300, "single": 1, "all": 5400}[gap]
    assert coverage[0]["limit"] == BOARD + (2700 if gap == "all" else 2800)
    assert not any(r["metric"] == "ejected" for r in answer["reasons"])


def test_longsword_coverage_stops_at_death():
    host, client = fixture_longsword()
    host = [r for r in host if not (r["kind"] == "cabin" and r["tick"] >= BOARD + 2850)]
    host.append(dict(host[-1], kind="error", tick=BOARD + 2850, reason="rider-life-changed", seq=10**7))
    answer = longsword(host, client)
    assert not any(r["metric"] == "cabin-coverage" for r in answer["reasons"])


def test_longsword_completely_missing_rider_has_coverage_error():
    host, client = fixture_longsword()
    answer = longsword([r for r in host if r["kind"] != "cabin"], client)
    assert any(r["metric"] == "cabin-coverage" and r["side"] == "host" and r["player"] == 1
               for r in answer["reasons"])


def test_longsword_negative_requires_cabin_coverage():
    host, client = fixture_longsword(wing=True)
    for row in host + client:
        if row["kind"] == "cabin":
            row.update(standing=1, verdict=1, jump=0, exception=0, anchor=0)
    host = [r for r in host if not (r["kind"] == "cabin" and r["tick"] == BOARD + 2800)]
    answer = ride.judge_longsword(host + client, 0, 0, LONGSWORD_BOX, negative=True)
    assert answer["status"] == "TOOL_ERROR"
    assert any(r["metric"] == "cabin-coverage" for r in answer["reasons"])


def test_longsword_door_death_without_tilt_uses_last_tilt():
    host, client = fixture_longsword(died_at=BOARD + 8160)
    for rows in (host, client):
        rows[:] = [r for r in rows if not (r["kind"] == "tilt" and r["tick"] == BOARD + 8160)]
    answer = longsword(host, client)
    assert answer["metrics"]["died_in_door_walk"] == ["host:8460", "client:8460"]
    assert not any(r["metric"] == "rider-died" for r in answer["reasons"])
    # (only the final tilt says door: choosing the first tilt would turn the death into an ejection)
    for row in host + client:
        if row["kind"] == "tilt" and row["tick"] == BOARD + 8159:
            row["door"] = 0
    answer = longsword(host, client)
    assert any(r["metric"] == "rider-died" for r in answer["reasons"])


def test_longsword_missing_kind_is_a_tool_error():
    host, client = fixture_longsword()
    answer = longsword([r for r in host if r["kind"] != "end"], client)
    assert answer["status"] == "TOOL_ERROR"
    assert any(r["metric"] == "missing-end" for r in answer["reasons"])


def test_longsword_other_error_is_a_tool_error():
    host, client = fixture_longsword()
    host.append(dict(host[-1], kind="error", reason="vehicle-preparation", seq=10**7))
    answer = longsword(host, client)
    assert answer["status"] == "TOOL_ERROR"
    assert any(r["metric"] == "error" and r["value"] == "vehicle-preparation" for r in answer["reasons"])


def test_parser_keeps_input_jump_integer(tmp_path):
    host, _ = fixture_logs()
    row = next(r for r in host if r["kind"] == "input" and r["jump"])
    path = tmp_path / "input.txt"
    write_records(path, [row])
    assert type(ride.parse_log(path)[0]["jump"]) is int


def test_parser_keeps_survey_jump_fractional(tmp_path):
    row = dict(schema="1", session="fixture", scenario="longsword", side="host", kind="survey_vehicle", seq=1,
               tick=BOARD, vehicle=65536, tag="fighterbomber", radius=5.0, m=0.5, slope=60.0,
               height=0.3, width=0.2, jump=0.8976)
    path = tmp_path / "survey.txt"
    write_records(path, [row])
    try:
        parsed = ride.parse_log(path)
    except ride.ToolError as error:
        assert False, f"valid fractional survey jump refused: {error}"
    assert parsed[0]["jump"] == pytest.approx(0.8976)


def invoke_main(tmp_path, arguments, status="PASS"):
    from unittest.mock import patch
    calls = []

    def no_game(*args):
        calls.append((args[3], args[8], args[10]))
        return tmp_path / "host.txt", tmp_path / "client.txt"

    answer = {"status": status, "required": False, "reasons": [], "metrics": {}}
    with patch.object(ride, "run_pair", side_effect=no_game), patch.object(ride, "parse_log", return_value=[]), \
            patch.object(ride, "judge", return_value=answer), patch.object(ride, "judge_longsword", return_value=answer):
        code = ride.main(["--output", str(tmp_path / "output"), "--rtts", "0", "--loss-case", "none"] + arguments)
    return code, calls


def test_survey_keeps_both_driven_roles(tmp_path):
    _, calls = invoke_main(tmp_path, ["--scenarios", "driven", "--survey"])
    assert [driver for _, driver, _ in calls] == ["host", "client"]


def test_longsword_desync_exits_nonzero(tmp_path):
    code, _ = invoke_main(tmp_path, ["--scenarios", "longsword"], status="DESYNC")
    assert code == 1


def test_all_excludes_longsword(tmp_path):
    _, calls = invoke_main(tmp_path, ["--scenarios", "all"])
    assert "longsword" not in {scenario for scenario, _, _ in calls}
    assert {scenario for scenario, _, _ in calls} == set(ride.STOCK_SCENARIOS)


def test_cabin_log_and_parser_require_diagnostics(tmp_path):
    import re
    source = (Path(__file__).resolve().parents[1] / "port/linux/game/network_test.c").read_text()
    logger = source.split("static void ride_cabin_log(", 1)[1].split("\n}", 1)[0]
    fields = set(re.findall(r"\b(\w+)=%", logger))
    diagnostics = set("axis raw inside ceiling ceiling_distance floor floor_distance standing misses jump take_off "
                      "exception carry settle rejected anchor verdict refusal".split())
    assert diagnostics <= fields
    assert diagnostics <= set(ride.KINDS["cabin"].split())
    host, _ = fixture_longsword()
    row = next(r for r in host if r["kind"] == "cabin")
    path = tmp_path / "cabin.txt"
    write_records(path, [row])
    parsed = ride.parse_log(path)[0]
    assert parsed["ceiling_distance"] == pytest.approx(0.5)
    assert parsed["floor_distance"] == pytest.approx(0.2)
    assert type(parsed["jump"]) is int


def test_longsword_runner_matches_pilot_starts():
    import re
    source = (Path(__file__).resolve().parents[1] / "port/linux/game/network_test.c").read_text()
    constants = {name: int(value) for name, value in re.findall(r"^#define (RIDE_PILOT_\w+) (\d+)$", source, re.M)}
    table = source.split("static long const starts[] = {", 1)[1].split("};", 1)[0]
    starts = tuple(constants[token] if token in constants else int(token) for token in re.findall(r"RIDE_PILOT_\w+|\b0\b", table))
    assert starts == (0, 300, 1200, 1500, 2400, 2700, 3000, 3300, 3600, 3900, 4200, 4500, 8100)
    assert ride.LONGSWORD_STARTS == starts
    assert constants["RIDE_PILOT_END"] == ride.LONGSWORD_END
    assert ride.LONGSWORD_END - starts[-1] == 360


def local_judge(rows, negative=False):
    import inspect
    assert "local" in inspect.signature(ride.judge_longsword).parameters, "single-side judge missing"
    return ride.judge_longsword(rows, 0, 0, LONGSWORD_BOX, negative=negative, local=True)


def test_local_longsword_accepts_only_host():
    host, _ = fixture_longsword()
    answer = local_judge(host)
    assert answer["status"] == "PASS", answer["reasons"]
    assert set(answer["metrics"]["preparation"]) == {"host:1"}
    assert ride.judge_longsword(host, 0, 0, LONGSWORD_BOX)["status"] == "TOOL_ERROR"


def test_local_longsword_coverage():
    host, _ = fixture_longsword()
    host = [r for r in host if not (r["kind"] == "cabin" and r["tick"] == BOARD + 2800)]
    answer = local_judge(host)
    assert answer["status"] == "TOOL_ERROR"
    assert any(r["metric"] == "cabin-coverage" for r in answer["reasons"])


def test_local_longsword_preparation():
    host, _ = fixture_longsword()
    next(r for r in host if r["kind"] == "cabin" and r["tick"] == BOARD + 2640)["ticks"] = 59
    answer = local_judge(host)
    assert answer["status"] == "TOOL_ERROR"
    assert any(r["metric"] == "longsword-preparation" for r in answer["reasons"])


def test_local_longsword_ejection():
    host, _ = fixture_longsword(outside_from=BOARD + 2800)
    answer = local_judge(host)
    assert answer["status"] == "DESYNC"
    assert any(r["metric"] == "ejected" for r in answer["reasons"])


def test_local_longsword_damage():
    host, _ = fixture_longsword(failed_at=BOARD + 2795, hurt_at=BOARD + 2800)
    answer = local_judge(host)
    assert answer["status"] == "DESYNC"
    assert any(r["metric"] == "failed-recovery-damage" for r in answer["reasons"])


def test_local_longsword_door():
    host, _ = fixture_longsword(door_out=False)
    answer = local_judge(host)
    assert answer["status"] == "DESYNC"
    assert any(r["metric"] == "door-blocked" for r in answer["reasons"])


def test_local_longsword_tilt():
    host, _ = fixture_longsword()
    for row in host:
        if row["kind"] == "tilt":
            row["tilt"] = 45.0
    answer = local_judge(host)
    assert answer["status"] == "TOOL_ERROR"
    assert any(r["metric"] == "tilt-not-reached" for r in answer["reasons"])


def test_local_longsword_updates():
    host, _ = fixture_longsword(frozen_updates=True)
    answer = local_judge(host)
    assert answer["status"] == "TOOL_ERROR"
    assert any(r["metric"] == "uncounted-update" for r in answer["reasons"])


def test_local_longsword_below_ejection():
    host, _ = fixture_longsword()
    cabins = [r for r in host if r["kind"] == "cabin"]
    cabins[3000]["uk"] = 0.42598
    cabins[3001]["uk"] = 0.42
    answer = local_judge(host)
    assert answer["status"] == "PASS", answer["reasons"]
    assert answer["metrics"]["below_ejection_uk"] == {"host:1": 2}


def test_local_longsword_negative():
    host, _ = fixture_longsword(wing=True)
    for row in host:
        if row["kind"] == "cabin":
            row.update(standing=1, verdict=1, jump=0, exception=0, anchor=0)
    answer = local_judge(host, negative=True)
    assert answer["status"] == "PASS", answer["reasons"]
    next(r for r in host if r["kind"] == "cabin")["anchor"] = 1
    answer = local_judge(host, negative=True)
    assert answer["status"] == "DESYNC"
    assert any(r["metric"] == "negative-enclosed" for r in answer["reasons"])


def test_local_environment(tmp_path):
    import inspect
    assert "local" in inspect.signature(ride.child_environment).parameters, "local environment missing"
    args = ("host", tmp_path / "data", tmp_path, "longsword", 100, 2, "127.0.0.200", "127.0.0.201", "host")
    env = ride.child_environment(*args, spot="1,2,-1,0", local=True)
    assert env["HALO_NETWORK_TEST"] == "local:custom_maps\\extinction:slayer"
    assert env["HALO_VEHICLE_RIDING"] == "true" and env["HALO_NETWORK_TEST_RIDE_SPOT"] == "1,2,-1,0"
    assert "HALO_HIDDEN_WINDOW" in env and env["HALO_HIDDEN_WINDOW"] == "1"
    assert not {"HALO_NET_ADDRESS", "HALO_NET_BROADCAST", "HALO_NETWORK_LATENCY", "HALO_NETWORK_LOSS",
                "HALO_NETWORK_TEST_RIDE_CLOCK"} & env.keys()
    assert "HALO_HIDDEN_WINDOW" not in ride.child_environment(*args, visible=True, local=True)


def test_local_main(tmp_path):
    import json
    from unittest.mock import patch
    calls = []
    host, _ = fixture_longsword()

    def no_game(*args, **kwargs):
        calls.append((args, kwargs))
        path = tmp_path / "host.txt"
        write_records(path, host)
        return path, None

    # Keep parsing and judging real; only the game launch is external.
    with patch.object(ride, "run_pair", side_effect=no_game):
        try:
            code = ride.main(["--output", str(tmp_path / "out"), "--scenarios", "longsword", "--local",
                              "--rtts", "0", "--loss-case", "none", "--cabin=-1,-0.6,0,1,0.6,1.5"])
        except SystemExit as error:
            assert False, f"local CLI not accepted: {error}"
    assert code == 0
    assert len(calls) == 1
    args, kwargs = calls[0]
    assert args[8] == "host" and (kwargs.get("local") or (len(args) > 11 and args[11]))
    assert args[2].name == "longsword-local-rtt0-loss0"
    assert args[10] == "1.75,0.27,-1.0,0.0"
    summary = json.loads((tmp_path / "out/summary.json").read_text())
    assert len(summary) == 1 and summary[0]["status"] == "PASS"
    assert summary[0]["logs"] == [str(args[2] / "host.debug.txt")]


def test_local_negative_main(tmp_path):
    from unittest.mock import patch
    calls = []
    answer = {"status": "PASS", "required": False, "reasons": [], "metrics": {}}
    with patch.object(ride, "run_pair", return_value=(tmp_path / "host.txt", None)) as launch, \
            patch.object(ride, "parse_log", return_value=[]), \
            patch.object(ride, "judge_longsword", side_effect=lambda *a, **k: (calls.append(k), answer)[1]):
        try:
            ride.main(["--output", str(tmp_path / "out"), "--scenarios", "longsword", "--local", "--negative",
                       "--rtts", "0", "--loss-case", "none"])
        except SystemExit as error:
            assert False, f"local negative CLI not accepted: {error}"
    assert launch.call_count == 1
    assert launch.call_args.args[10] == "0.998,3.0,0.0,0.0"
    assert calls == [{"negative": True, "local": True}]


def test_local_rejects_other_modes(tmp_path):
    from unittest.mock import patch
    for index, args in enumerate((["--scenarios", "straight"], ["--scenarios", "longsword", "--survey"],
                                  ["--scenarios", "longsword,driven"])):
        launched = []

        def launch(*_args, **_kwargs):
            launched.append(args)
            raise AssertionError("invalid local mode launched")

        code = None
        # (main reports any launch failure as a tool error, so a launch is told from the launcher's own record, not an exception)
        with patch.object(ride, "run_pair", side_effect=launch):
            try:
                ride.main(["--output", str(tmp_path / str(index)), "--local"] + args)
            except SystemExit as error:
                code = error.code
        assert not launched, "invalid local mode launched"
        assert code == 2
        assert not (tmp_path / str(index)).exists()


def test_local_launcher(tmp_path):
    import inspect
    import os
    from unittest.mock import patch
    assert "local" in inspect.signature(ride.run_pair).parameters, "single-process launcher missing"
    if os.name != "nt":
        pytest.skip("real Windows data-root junctions")
    source = tmp_path / "source"
    (source / "maps").mkdir(parents=True)
    (source / "custom_maps").mkdir()
    halo = source / "halo.exe"
    halo.write_bytes(b"test executable, not launched")
    root = tmp_path / "case"
    launches = []

    class Child:
        pid = 12345
        returncode = None

        def __init__(self, command, **kwargs):
            launches.append((command, kwargs))
            assert len(launches) == 1, "local launcher started a second game"
            self.stopped = False
            (root / "host/data/debug.txt").write_text("ride kind=end finished\n")

        def poll(self):
            return self.returncode

        def terminate(self):
            self.stopped = True

        def wait(self, timeout):
            assert self.stopped
            self.returncode = 0
            return 0

    real_popen = ride.subprocess.Popen

    def launch(command, **kwargs):
        # Junction creation uses subprocess.run/Popen too; keep that filesystem setup real.
        return real_popen(command, **kwargs) if command[0] == "cmd.exe" else Child(command, **kwargs)

    with patch.object(ride.subprocess, "Popen", side_effect=launch):
        host, client = ride.run_pair(halo, source, root, "longsword", 0, 0, local=True)
    assert len(launches) == 1 and client is None
    assert host.is_file(), "local host log not collected"
    assert host == root / "host.debug.txt" and host.read_text() == "ride kind=end finished\n"
    assert not (root / "client").exists()
    assert launches[0][1]["env"]["HALO_NETWORK_TEST"].startswith("local:")
    assert launches[0][1]["env"]["HALO_HIDDEN_WINDOW"] == "1"
    import json
    metadata = json.loads((root / "launch.json").read_text())
    assert len(metadata["children"]) == 1 and metadata["children"][0]["exit"] == 0


@pytest.mark.parametrize("first", [0, 1])
def test_child_own_exit_before_cleanup(tmp_path, first):
    import json
    import types
    from unittest.mock import patch

    output = tmp_path / "case"
    halo = tmp_path / "halo.exe"
    halo.write_bytes(b"not launched")

    class Child:
        def __init__(self, pid, code):
            self.pid, self.returncode = pid, code

        def poll(self):
            return self.returncode

        def terminate(self):
            self.returncode = 1

        def wait(self, timeout):
            return self.returncode

    children = [Child(100 + i, 42 if i == first else None) for i in range(2)]

    def make_side(executable, data, root, side, *args, **kwargs):
        (root / side).mkdir()
        return executable, data

    original_stop = ride.stop_children

    def stop_checked(launched):
        metadata = json.loads((output / "launch.json").read_text())
        own, survivor = metadata["children"][first], metadata["children"][1-first]
        assert own["own_exit"] == 42
        assert own["first_seen_exit_utc"] == 1234.5
        assert own["first_seen_exit_elapsed"] >= 0
        assert "own_exit" not in survivor and "first_seen_exit_utc" not in survivor
        assert children[1-first].poll() is None
        original_stop(launched)

    with patch.object(ride, "os", types.SimpleNamespace(name="nt")), \
         patch.object(ride, "make_side", side_effect=make_side), \
         patch.object(ride, "child_environment", return_value={}), \
         patch.object(ride.subprocess, "CREATE_NEW_PROCESS_GROUP", 0, create=True), \
         patch.object(ride.subprocess, "CREATE_NO_WINDOW", 0, create=True), \
         patch.object(ride.subprocess, "Popen", side_effect=children), \
         patch.object(ride.time, "time", return_value=1234.5), \
         patch.object(ride, "stop_children", side_effect=stop_checked):
        with pytest.raises(ride.ToolError, match="a game exited before the logs ended"):
            ride.run_pair(halo, tmp_path, output, "longsword", 0, 0)
    data = json.loads((output / "launch.json").read_text())["children"]
    assert data[first]["own_exit"] == data[first]["exit"] == 42
    assert data[first]["alive_before_cleanup"] is False
    assert data[1-first]["exit"] == 1 and data[1-first]["alive_before_cleanup"] is True
    assert "own_exit" not in data[1-first]


# ---------- a remote rider's world is judged against the host state of the same instant (Task 6j)

SETTLED = BOARD + 400
# (T\m2-driven0-r1, client tick 1145: the host's state H, its successor H+1, and the client's sample after applying H)
R1_APPLIED = (36.7630, -88.3598, 0.5481)
R1_SUCCESSOR = (36.7192, -88.0737, 0.5983)
R1_SAMPLE = (36.7099, -88.0806, 0.5835)


def host_state(host, tick, player=0):
    return next(r for r in host if r["kind"] == "state" and r["player"] == player and r["host_tick"] == tick)


def client_sample(client, tick, player=0):
    return next(r for r in client if r["kind"] == "sample" and not r["local"] and r["player"] == player and
                r["simulation_tick"] == tick)


def world_pair_logs(tick=SETTLED, applied=R1_APPLIED, successor=R1_SUCCESSOR, sample=R1_SAMPLE, ticks=(60, 59, 59)):
    """Remote player 0 at a settled tick as a world pair: the host's state H (applied), H+1 (successor) and the
    client's sample, each with its world and support ticks."""
    host, client = fixture_logs()
    for row, world, count in ((host_state(host, tick), applied, ticks[0]),
                              (host_state(host, tick + 1), successor, ticks[1]),
                              (client_sample(client, tick), sample, ticks[2])):
        row.update(world=world, ticks=count)
    return host, client


def remote_pair(host, client, tick=SETTLED, player=0):
    return next(p for p in ride.align(host, client) if p["mode"] == "remote" and p["player"] == player and
                p["tick"] == tick)


def judge_pair(host, client, tick=SETTLED):
    """The judge on one remote pair alone, so a neighbour's records do not enter its metrics."""
    return ride.judge([remote_pair(host, client, tick)], host + client, 100, 0)


def reasons_of(answer, metric):
    return [r for r in answer["reasons"] if r["metric"] == metric]


def test_remote_world_aligned_to_successor_state():
    host, client = world_pair_logs()
    pair = remote_pair(host, client)
    assert pair["host_tick"] == SETTLED and pair["host"]["ticks"] == 60
    assert pair.get("one_step") is True and pair.get("apply_tick") == SETTLED
    assert pair.get("world_host") is not None and pair["world_host"]["host_tick"] == SETTLED + 1
    # (the applied state alone would be 0.2864 off: the one-tick artefact the successor removes)
    assert ride.distance(R1_APPLIED, R1_SAMPLE) == pytest.approx(.2864, abs=1e-4)
    answer = judge_pair(host, client)
    assert answer["metrics"]["max_place_error"] == pytest.approx(.0188, abs=1e-4), answer
    assert reasons_of(answer, "world") == [] and reasons_of(answer, "place") == [], answer


def test_remote_world_genuine_residual_still_fails():
    base = (10., 20., 1.)
    behind = (base[0], base[1], base[2] - .3)
    cases = [  # (tick, displacement of the client from H+1, the applied world, expected world failure or None)
        (SETTLED, (0., 0., .06), base, .06),
        (SETTLED, (0., 0., .04), base, None),
        # (T\m2-driven0-r2, client 1143: z .0562 behind with equal velocities, and 1233's .063)
        (SETTLED, (.01435, 0., -.0562), base, .058),
        (SETTLED, (0., 0., .063), base, .063),
        # (the first air tick of a jump, the applied state still a step behind)
        (BOARD + ride.RIDE_JUMP + 2, (0., 0., .06), behind, .06),
        # (the nearest-row trap: the applied state is within .04 of the sample, the successor is .06 away)
        (SETTLED, (0., 0., .06), (base[0], base[1], base[2] + .02), .06),
    ]
    for tick, shift, applied, expected in cases:
        sample = tuple(base[i] + shift[i] for i in range(3))
        host, client = world_pair_logs(tick, applied, base, sample)
        failures = reasons_of(judge_pair(host, client, tick), "world")
        if expected is None:
            assert failures == [], (shift, failures)
        else:
            assert len(failures) == 1 and failures[0]["value"] == pytest.approx(expected, abs=1e-4), (shift, failures)


def test_remote_place_pairs_unchanged():
    for offset in (.08, .16):
        host, client = fixture_logs()
        row = host_state(host, SETTLED)
        row.update(place=(row["place"][0] + offset, row["place"][1], row["place"][2]))
        failures = reasons_of(judge_pair(host, client), "place")
        assert len(failures) == 1 and failures[0]["value"] == pytest.approx(offset, abs=1e-6), (offset, failures)
    # (the successor's place is not the pair's)
    host, client = fixture_logs()
    row = host_state(host, SETTLED + 1)
    row.update(place=(row["place"][0] + .16, row["place"][1], row["place"][2]))
    answer = judge_pair(host, client)
    assert reasons_of(answer, "place") == [] and reasons_of(answer, "world") == [], answer
    # (a riding pair whose successor is a claim transition stays a place pair of the applied state)
    for change in ({"ticks": 59}, {"vehicle": 65537}):
        host, client = fixture_logs()
        row = host_state(host, SETTLED)
        row.update(place=(row["place"][0] + .12, row["place"][1], row["place"][2]))
        host_state(host, SETTLED + 1).update(change)
        answer = judge_pair(host, client)
        failures = reasons_of(answer, "place")
        assert len(failures) == 1 and failures[0]["value"] == pytest.approx(.12, abs=1e-6), (change, answer)
        assert reasons_of(answer, "world") == [], (change, answer)


def verdict_and_claim_logs():
    """Remote player 0 over 80 settled ticks: the client's verdict differs from the host's for 50 of them, the host's
    states cross a phase (60 to 59), a vehicle change and a verdict change, and the client's claim then leaves the
    state it applied."""
    host, client = fixture_logs()
    first = SETTLED
    for tick in range(first, first + 50):
        client_sample(client, tick).update(flags=2)
    for tick in range(first + 30, first + 35):
        host_state(host, tick).update(ticks=59)
    for tick in range(first + 40, first + 45):
        host_state(host, tick).update(vehicle=65537)
    for tick in range(first + 50, first + 60):
        host_state(host, tick).update(flags=2)
    for tick in range(first + 60, first + 80):
        client_sample(client, tick).update(vehicle=65537)
    return host, client


def test_remote_verdict_and_claim_unchanged():
    host, client = verdict_and_claim_logs()
    answer = result(host, client)
    seen = {}
    for reason in answer["reasons"]:
        seen.setdefault(reason["metric"], []).append((reason["tick"], reason["value"], reason["limit"]))
    assert VERDICT_AND_CLAIM_GOLDEN == {metric: seen.get(metric) for metric in VERDICT_AND_CLAIM_GOLDEN}, seen
    assert answer["metrics"]["max_claim_disagreement"] == 20


# (the old runner's results for these records)
VERDICT_AND_CLAIM_GOLDEN = {
    "verdict-disagreement": [(SETTLED + 15 + i, 0, 1) for i in range(15)],
    "claim-disagreement": [(SETTLED + 75 + i, 16 + i, 15) for i in range(5)],
    "standing-correction": None,
    "corrections-in-two-seconds": None,
}


def test_remote_world_placement_is_judged_against_the_applied_state():
    base = (10., 20., 1.)
    moved = (base[0] + 5., base[1], base[2])

    def placed_logs(**placement):
        # (T\m2-driven0-r1, client 1453: the host placed the rider at its tick H, so H+1 is 8 wu from the sample)
        host, client = world_pair_logs(SETTLED, base, moved, (base[0], base[1], base[2] + .01), ticks=(0, 0, 0))
        row = dict(host[0], kind="recovery", seq=10 ** 6, tick=SETTLED, player=0, unit=131072, reason="place",
                   start_tick=SETTLED - 70, recoveries=1)
        row.update(placement)
        host.append(row)
        return host, client

    host, client = placed_logs()
    assert remote_pair(host, client).get("placed") is True
    answer = judge_pair(host, client)
    assert reasons_of(answer, "world") == [] and answer["metrics"]["max_place_error"] == pytest.approx(.01, abs=1e-6), answer
    # (only the host's own placement of this player and unit at this tick: not a start, an acquisition, another rider,
    # another tick, or the size of the jump)
    for change in ({"reason": "start"}, {"reason": "acquired"}, {"player": 1}, {"unit": 131090}, {"tick": SETTLED + 1}):
        host, client = placed_logs(**change)
        failures = reasons_of(judge_pair(host, client), "world")
        assert len(failures) == 1 and failures[0]["value"] == pytest.approx(5., abs=1e-3), (change, failures)


def test_remote_world_missing_successor_is_tool_error():
    # (a) the applied state missing: reported as before
    host, client = fixture_logs()
    host.remove(host_state(host, SETTLED))
    failures = reasons_of(result(host, client), "missing-applied-host-state")
    assert [(r["tick"], r["value"]) for r in failures] == [(SETTLED, SETTLED)], failures
    # (b) the successor missing for a world pair, coverage otherwise sufficient
    host, client = world_pair_logs()
    host.remove(host_state(host, SETTLED + 1))
    answer = result(host, client)
    failures = [r for r in reasons_of(answer, "missing-aligned-host-state") if r["tick"] == SETTLED]
    assert answer["status"] == "TOOL_ERROR" and len(failures) == 1 and failures[0]["player"] == 0, answer
    text = str(failures[0]["value"])
    for part in (f"{SETTLED}", f"{SETTLED + 1}", "131072"):
        assert part in text, failures
    assert reasons_of(answer, "alignment-coverage") == [], answer
    assert reasons_of(answer, "world") == [], answer
    # (c) the successor present only for another unit, or another player
    for change in ({"unit": 131090}, {"player": 1}):
        host, client = world_pair_logs()
        host_state(host, SETTLED + 1).update(change)
        answer = result(host, client)
        assert [r for r in reasons_of(answer, "missing-aligned-host-state") if r["tick"] == SETTLED], (change, answer)
    # (d) an older valid apply, then a newer one whose state is missing, then a sample
    host, client = fixture_logs()
    host.remove(host_state(host, SETTLED))
    client.remove(client_sample(client, SETTLED - 1))
    pairs = ride.align(host, client)
    assert [p for p in pairs if p["mode"] == "remote" and p["player"] == 0 and p["tick"] in (SETTLED - 1, SETTLED)] == []
    failures = reasons_of(result(host, client), "missing-applied-host-state")
    assert [(r["tick"], r["value"]) for r in failures] == [(SETTLED, SETTLED)], failures
    # (e) a place pair needs no successor
    host, client = fixture_logs()
    host.remove(host_state(host, SETTLED + 1))
    row = host_state(host, SETTLED)
    row.update(place=(row["place"][0] + .08, row["place"][1], row["place"][2]))
    answer = result(host, client)
    assert [r["value"] for r in reasons_of(answer, "place") if r["tick"] == SETTLED] == [pytest.approx(.08, abs=1e-6)], answer
    assert not [r for r in reasons_of(answer, "missing-aligned-host-state") if r["tick"] == SETTLED], answer


def test_remote_pairing_sequence():
    stand_samples = 570
    # (a) three applies before one sample: one pair, the latest state
    host, client = fixture_logs()
    for tick in (SETTLED, SETTLED + 1):
        client.remove(client_sample(client, tick))
    pairs = [p for p in ride.align(host, client) if p["mode"] == "remote" and p["player"] == 0 and
             SETTLED <= p["tick"] <= SETTLED + 2]
    assert [(p["tick"], p["host_tick"], p["client"]["seq"]) for p in pairs] == \
        [(SETTLED + 2, SETTLED + 2, client_sample(client, SETTLED + 2)["seq"])], pairs
    assert result(host, client)["metrics"]["alignment_coverage"]["remote:0:stand"] == 1.0
    # (b) a sample two ticks after its apply: ambiguous for the world, no matter for a place
    host, client = world_pair_logs()
    next(r for r in client if r["kind"] == "apply" and r["player"] == 0 and r["applied_host_tick"] == SETTLED).update(tick=SETTLED - 2)
    pair = remote_pair(host, client)
    assert pair.get("one_step") is False and pair.get("apply_tick") == SETTLED - 2, pair
    answer = judge_pair(host, client)
    failures = reasons_of(answer, "world-step-ambiguous")
    assert answer["status"] == "TOOL_ERROR" and len(failures) == 1, answer
    for part in (str(SETTLED), str(SETTLED - 2)):
        assert part in str(failures[0]["value"]), failures
    assert failures[0]["player"] == 0 and failures[0]["tick"] == SETTLED
    assert reasons_of(answer, "world") == [], answer
    host, client = fixture_logs()
    next(r for r in client if r["kind"] == "apply" and r["player"] == 0 and r["applied_host_tick"] == SETTLED).update(tick=SETTLED - 2)
    row = host_state(host, SETTLED)
    row.update(place=(row["place"][0] + .08, row["place"][1], row["place"][2]))
    answer = judge_pair(host, client)
    assert [r["value"] for r in reasons_of(answer, "place")] == [pytest.approx(.08, abs=1e-6)], answer
    assert reasons_of(answer, "world-step-ambiguous") == [], answer
    # (c) a duplicate sample record: the first is paired
    host, client = fixture_logs()
    original = client_sample(client, SETTLED)
    client.insert(client.index(original) + 1, dict(original, seq=original["seq"] + .5))
    pairs = [p for p in ride.align(host, client) if p["mode"] == "remote" and p["player"] == 0 and p["tick"] == SETTLED]
    assert [p["client"]["seq"] for p in pairs] == [original["seq"]], pairs
    assert result(host, client)["metrics"]["alignment_coverage"]["remote:0:stand"] == pytest.approx(stand_samples / (stand_samples + 1))
    # (d) a sample with no apply of its own after a consumed one forms no pair
    host, client = fixture_logs()
    client.remove(next(r for r in client if r["kind"] == "apply" and r["player"] == 0 and r["applied_host_tick"] == SETTLED))
    pairs = [p for p in ride.align(host, client) if p["mode"] == "remote" and p["player"] == 0 and p["tick"] == SETTLED]
    assert pairs == [], pairs
    assert result(host, client)["metrics"]["alignment_coverage"]["remote:0:stand"] == pytest.approx((stand_samples - 1) / stand_samples)
    # (e) a unit replaced between the apply and the sample: no pair for the new unit
    host, client = fixture_logs()
    replaced = client_sample(client, SETTLED)
    replaced.update(unit=131090)
    pairs = [p for p in ride.align(host, client) if p["mode"] == "remote" and p["player"] == 0 and p["tick"] == SETTLED]
    assert pairs == [], pairs


def test_own_path_unchanged():
    host, client = fixture_logs()
    pairs = ride.align(host, client)
    own = sorted((p for p in pairs if p["mode"] == "own"), key=lambda p: (p["player"], p["tick"]))
    assert len(own) == OWN_GOLDEN["count"]
    assert [(p["tick"], p["host_tick"]) for p in own[:3]] == OWN_GOLDEN["first"]
    assert [(p["tick"], p["host_tick"]) for p in own[-3:]] == OWN_GOLDEN["last"]
    assert sum(p["tick"] for p in own) == OWN_GOLDEN["tick_sum"]
    assert sum(p["host_tick"] for p in own) == OWN_GOLDEN["host_tick_sum"]
    assert [p["client"]["world"] for p in own[:2]] == OWN_GOLDEN["worlds"]
    answer = ride.judge(pairs, host + client, 100, 0)
    assert {key: value for key, value in answer["metrics"]["alignment_coverage"].items() if key.startswith("own:")} == \
        OWN_GOLDEN["coverage"]
    # (an own world pair: the applied state rides another vehicle and is .2 off, the successor agrees exactly)
    tick = BOARD + ride.RIDE_OBSERVE + 20
    host, client = fixture_logs()
    capture = next(r for r in client if r["kind"] == "own" and r["player"] == 1 and r["tick"] == tick)
    echo = next(r for r in client if r["kind"] == "state" and r["player"] == 1 and r["host_tick"] == tick)
    world = capture["world"]
    echo.update(vehicle=65537, world=(world[0] + .2, world[1], world[2]))
    host_state(host, tick + 1, 1).update(world=world)
    pairs = ride.align(host, client)
    pair = next(p for p in pairs if p["mode"] == "own" and p["tick"] == tick)
    assert pair["host_tick"] == tick
    answer = ride.judge(pairs, host + client, 100, 0)
    failures = [r for r in reasons_of(answer, "world") if r["player"] == 1 and r["tick"] == tick]
    assert len(failures) == 1 and failures[0]["value"] == pytest.approx(.2, abs=1e-4), answer


# (the old runner's own pairs for fixture_logs())
OWN_GOLDEN = {
    "count": 2401, "first": [(301, 301), (302, 302), (303, 303)], "last": [(2699, 2699), (2700, 2700), (2701, 2701)],
    "tick_sum": 3603901, "host_tick_sum": 3603901, "worlds": [(0.0, 0.5, 0.0), (0.04, 0.5, 0.0)],
    "coverage": {"own:1:stand": 1.0, "own:1:walk": 1.0, "own:1:jump": 1.0},
}


NEGATIVE_CONTROLS = {
    "suspension-kind-unparsed": ('"scripts_suspended": "tick",', '', 'test_control_and_suspension_records_parse'),
    "inhibited-kind-unparsed": ('"control_inhibited": "tick phase player",', '', 'test_control_and_suspension_records_parse'),
    "control-inhibited-not-judged": ('fail(row, "control-inhibited", row["tick"], "delivered owner controls", tool=True)',
                                     'pass', 'test_coop_control_inhibited_is_tool_error'),
    "unsupported-launch-ignored": ('elif row["kind"] == "support_failure":', 'elif False:', 'test_unsupported_first_jump_is_scored_desync'),
    "no-landing-floor": ('outcome["landings"] < 1 or accounted', 'False or accounted',
                         'test_driven_outcome_needs_a_deck_landing'),
    "driven-unaccounted-jump": ('accounted < outcome["jumps"]', 'False',
                                'test_driven_outcome_accounts_for_every_jump'),
    "driven-landed-off-unlimited": ('if outcome["landed_off"] > LANDED_OFF_LIMIT:', 'if False:',
                                   'test_driven_outcome_keeps_landed_off_limit'),
    "recovery-contact-exemption-removed": (' and not recovering(row):', ':',
                                         'test_driven_recovery_counts_and_bounds_contact_exclusion'),
    "driven-runner-old-length": ('DRIVEN_EXTRA = RECOVERY_MAX * (RECOVERY_TIMEOUT + RECOVERY_SETTLE)',
                                  'DRIVEN_EXTRA = 0', 'test_driven_schedule_fits_recovery_budget'),
    "driven-judge-old-input-window": ('RIDE_JUMP <= r["tick"] - board < cross',
                                        'RIDE_JUMP <= r["tick"] - board < RIDE_CROSS',
                                        'test_driven_judge_reads_extended_schedule'),
    "life-record-not-failure": ('fail(life, "rider-died", life["reason"], "alive")', 'pass',
                               "test_life_record_keeps_engine_cause_and_fails"),
    "recovery-acquisition-old-clock": ('range(row["tick"] - 5, row["tick"] + 1)',
                                       'range(row["tick"] - 6, row["tick"])',
                                       "test_driven_acquisition_includes_logged_tick"),
    "replay-by-world-delta": ('captured["place"] = record["place"]',
                              'captured["place"] = local_point(captured["base"], captured)',
                              "test_history_replay_turning_deck"),
    "recovery-contact-unbounded": ('begin <= row["simulation_tick"] <= end', 'begin <= row["simulation_tick"]',
                                   "test_driven_recovery_counts_and_bounds_contact_exclusion"),
    "recovery-timeout-ignored": ('if age >= RECOVERY_TIMEOUT or row["reason"] == "failed":', 'if False:',
                                  "test_driven_recovery_timeout_fails"),
    "child-own-exit-lost": ('item["own_exit"] = code', 'item["own_exit"] = 1', "test_child_own_exit_before_cleanup"),
    "door-not-run-passes": ('fail(host_landing or {"side": "host"}, "door-not-run", metrics["door_walk"], "landed before door walk", tool=True)',
                            'pass', "test_door_walk_needs_landing"),
    "crossing-still-required": ('or not outcome["stepped_off"]:',
                                'or not outcome["stepped_off"] or (scenario == "convoy" and outcome["crossings"] < 1):',
                                "test_convoy_missing_crossings_are_report_only"),
    "crossing-count-hidden": ('"count": outcome["crossings"], "cross_phase_count": cross_phase_count,',
                              '"count": 0, "cross_phase_count": cross_phase_count,',
                              "test_convoy_crossings_report_counts_and_physical_phase"),
    "jump-switch-is-cross": ('and row["phase"] == "cross":', 'and True:',
                              "test_convoy_crossings_report_counts_and_physical_phase"),
    "grace-is-physical-crossing": ('if not row["deck_present"] or row["ticks"] != 60:',
                                   'if not row["deck_present"]:',
                                   "test_convoy_grace_window_is_not_a_physical_crossing"),
    "crossing-every-contact": ('previous != row["vehicle"] and row["phase"] == "cross"',
                                'row["phase"] == "cross"',
                                "test_convoy_crossings_report_counts_and_physical_phase"),
    "convoy-jumps-unchecked": ('outcome["jumps"] < RIDE_JUMPS or outcome["landings"]',
                                'False or outcome["landings"]', "test_convoy_other_outcome_checks_still_fail"),
    "convoy-landings-unchecked": ('outcome["landings"] < RIDE_JUMPS or not outcome["stepped_off"]',
                                   'False or not outcome["stepped_off"]', "test_convoy_other_outcome_checks_still_fail"),
    "convoy-step-off-unchecked": ('or not outcome["stepped_off"]:', 'or False:',
                                   "test_convoy_other_outcome_checks_still_fail"),
    "cross-hop-counted": ('RIDE_JUMP <= r["tick"] - board < cross', 'True',
                          "test_hops_outside_jump_phase_do_not_count"),
    "cross-hop-replaces-press": ('RIDE_JUMP <= r["tick"] - board < cross', 'True',
                                 "test_cross_hop_cannot_replace_a_missing_jump_phase_press"),
    "first-phase-press-skipped": ('RIDE_JUMP <= r["tick"] - board < cross',
                                  'RIDE_JUMP < r["tick"] - board < RIDE_CROSS',
                                  "test_hops_outside_jump_phase_do_not_count"),
    "local-client-required": ('for side in (("host",) if local else ("host", "client")):',
                              'for side in ("host", "client"):', "test_local_longsword_accepts_only_host"),
    "networked-client-optional": ('for side in (("host",) if local else ("host", "client")):',
                                  'for side in ("host",):', "test_local_longsword_accepts_only_host"),
    "local-coverage-skipped": ('if missing:', 'if missing and not local:', "test_local_longsword_coverage"),
    "local-preparation-short": ('streak += 1\n                    else:', 'streak += 2 if local else 1\n                    else:',
                                 "test_local_longsword_preparation"),
    "local-ejection-skipped": ('fail(row, "ejected", row["tick"], "inside")',
                                'if not local: fail(row, "ejected", row["tick"], "inside")', "test_local_longsword_ejection"),
    "local-damage-skipped": ('fail(row, "failed-recovery-damage", row["body"] + row["shield"], first)',
                              'if not local: fail(row, "failed-recovery-damage", row["body"] + row["shield"], first)', "test_local_longsword_damage"),
    "local-door-skipped": ('fail(last, "door-blocked", last["tick"], "out and off the deck")',
                            'if not local: fail(last, "door-blocked", last["tick"], "out and off the deck")', "test_local_longsword_door"),
    "local-tilt-skipped": ('if maximum <= 45.0:', 'if maximum <= 45.0 and not local:', "test_local_longsword_tilt"),
    "local-updates-skipped": ('if row["updates"] <= previous:', 'if row["updates"] <= previous and not local:', "test_local_longsword_updates"),
    "local-ejection-threshold-strict": ('sum(r["uk"] <= LONGSWORD_EJECTION_UK for r in mine)',
                                        'sum(r["uk"] < LONGSWORD_EJECTION_UK for r in mine)', "test_local_longsword_below_ejection"),
    "local-negative-skipped": ('if negative and (row["standing"]', 'if negative and not local and (row["standing"]', "test_local_longsword_negative"),
    "local-negative-boxed": ('(negative or box is None or inside_box(row["place"], box))',
                              '((negative and not local) or box is None or inside_box(row["place"], box))', "test_local_longsword_negative"),
    "local-environment-hosts-network": ('env["HALO_NETWORK_TEST"] = f"local:{LONGSWORD_MAP}:slayer"',
                                         'env["HALO_NETWORK_TEST"] = f"host:{LONGSWORD_MAP}:slayer"', "test_local_environment"),
    "local-environment-keeps-transport": ('del env[key]', 'pass', "test_local_environment"),
    "local-visible-hidden": ('if visible:\n        del env["HALO_HIDDEN_WINDOW"]',
                              'if visible and not local:\n        del env["HALO_HIDDEN_WINDOW"]', "test_local_environment"),
    "local-launches-two-games": ('sides = ("host",) if local else ("host", "client")',
                                 'sides = ("host", "client")', "test_local_launcher"),
    "local-log-not-collected": ('for side in sides:\n            source = output',
                                'for side in (() if local else sides):\n            source = output', "test_local_launcher"),
    "local-summary-client-log": ('for side in (("host",) if args.local else ("host", "client"))]',
                                  'for side in ("host", "client")]', "test_local_main"),
    "local-mode-not-forwarded": ('mode = {"local": True} if args.local else {}', 'mode = {}', "test_local_main"),
    "local-cli-two-roles": ('if not args.local and scenario in', 'if scenario in', "test_local_main"),
    "local-cli-wrong-directory": ("'local' if args.local else driver", "driver", "test_local_main"),
    "local-other-modes-allowed": ('if args.local and (scenarios != ["longsword"] or args.survey):',
                                   'if False:', "test_local_rejects_other_modes"),
    "local-negative-not-forwarded": ('negative=args.negative, **mode', 'negative=False, **mode', "test_local_negative_main"),
    "blocked-recovery-hidden": ('fail(row, "blocked-recovery-outside", row["tick"], "inside")',
                                'fail(row, "ejected", row["tick"], "inside")', "test_longsword_blocked_recovery_outside_is_reported"),
    "recovery-damage-allowed": ('fail(row, "failed-recovery-damage", row["body"] + row["shield"], first)', "pass",
                                "test_longsword_failed_recovery_damage_is_a_failure"),
    "recovery-damage-unbounded": ('row["tick"] - last_failed <= EJECT_TICKS and ', '',
                                  "test_longsword_other_damage_is_reported_only"),
    "recovery-damage-against-start": ('row["body"] + row["shield"] < previous_health and',
                                     'row["body"] + row["shield"] < first[0] + first[1] and',
                                     "test_longsword_earlier_damage_is_not_failed_recovery_damage"),
    "refusal-any-as-guard": ('if row["refusal"] == REFUSAL_MOTION and not tilts.get(row["tick"], {}).get("door"):',
                             'if row["refusal"] and not tilts.get(row["tick"], {}).get("door"):',
                             "test_longsword_other_refusals_are_reported"),
    "guard-unchecked": ('fail(row, "snapshot-guard", row["tick"], "none", tool=True)', "pass",
                         "test_longsword_snapshot_guard_is_a_tool_error"),
    "limit-unchecked": ('fail(row, "limit-mismatch", row["limit"], LONGSWORD_LIMIT, tool=True)', "pass",
                         "test_longsword_limit_mismatch_is_a_tool_error"),
    "recoveries-uncounted": ('failures[key] = failures.get(key, 0) + 1', "pass", "test_longsword_counts_recoveries"),
    "negative-unchecked": ('fail(row, "negative-enclosed", row["tick"], "open")', "pass",
                            "test_longsword_negative_run_enclosed_is_a_failure"),
    "negative-preparation-boxed": ('(negative or box is None or inside_box(row["place"], box))',
                                   '(box is None or inside_box(row["place"], box))', "test_longsword_negative_run_open_passes"),
    "stock-enclosed-allowed": ('fail(row, "stock-enclosed", row["flags"] >> 1, "never")', "pass",
                                "test_stock_standing_enclosed_verdict_is_desync"),
    "stock-enclosed-in-air": ('row["deck_present"] and row["ticks"] == 60 and (row["flags"] >> 1) == ENCLOSED',
                               'row["deck_present"] and (row["flags"] >> 1) == ENCLOSED',
                               "test_stock_air_legacy_verdict_is_not_stock_enclosed"),
    "coverage-unchecked": ('fail({"side": side, "player": player, "tick": tilt_start}, "cabin-coverage", len(missing), missing[0], tool=True)',
                           'pass', "test_longsword_cabin_coverage_is_a_tool_error"),
    "coverage-ends-early": ('coverage_end = min([max(tilts, default=tilt_start - 1) + 1, door_start] + deaths)',
                            'coverage_end = tilt_start + 1', "test_longsword_cabin_coverage_is_a_tool_error"),
    "coverage-past-death": ('coverage_end = min([max(tilts, default=tilt_start - 1) + 1, door_start] + deaths)',
                            'coverage_end = min(max(tilts, default=tilt_start - 1) + 1, door_start)', "test_longsword_coverage_stops_at_death"),
    "coverage-negative-unchecked": ('if missing:', 'if missing and not negative:', "test_longsword_negative_requires_cabin_coverage"),
    "coverage-side-only-keys": ('for player in sorted({r["player"] for r in records if r["kind"] == "cabin"}):',
                                'for player in sorted({r["player"] for r in rows if r["kind"] == "cabin"}):',
                                "test_longsword_completely_missing_rider_has_coverage_error"),
    "input-jump-float": ('key in FLOAT_KEYS or (key == "jump" and kind == "survey_vehicle")',
                         'key in FLOAT_KEYS or key == "jump"', "test_parser_keeps_input_jump_integer"),
    "survey-jump-integer": ('key in FLOAT_KEYS or (key == "jump" and kind == "survey_vehicle")',
                            'key in FLOAT_KEYS', "test_parser_keeps_survey_jump_fractional"),
    "survey-drops-driven": ('not (scenario == "longsword" and args.survey)', 'not args.survey', "test_survey_keeps_both_driven_roles"),
    "longsword-failure-exits-zero": ('(r["required"] or r["scenario"] == "longsword")', 'r["required"]', "test_longsword_desync_exits_nonzero"),
    "door-death-first-tilt": ('tilts.get(max(tilts) if tilts else None, {})', 'tilts.get(min(tilts) if tilts else None, {})',
                             "test_longsword_door_death_without_tilt_uses_last_tilt"),
    "longsword-missing-kind-ignored": ('for kind in ("prepare", "end", "tilt", "cabin"):', 'for kind in ("prepare", "tilt", "cabin"):',
                                      "test_longsword_missing_kind_is_a_tool_error"),
    "longsword-other-error-ignored": ('fail(row, "error", row["reason"], "none", tool=True)\n        failures',
                                     'pass\n        failures', "test_longsword_other_error_is_a_tool_error"),
    "all-includes-longsword": ('list(STOCK_SCENARIOS) if args.scenarios == "all"', 'list(SCENARIOS) if args.scenarios == "all"',
                               "test_all_excludes_longsword"),
    "runner-hover-skipped": ('1500, 2400, 2700', '1500, 2700', "test_longsword_runner_matches_pilot_starts"),
    "longsword-preparation-ignored": ('if not prepared:\n                fail', 'if False:\n                fail',
                                      "test_longsword_preparation_requires_standing_inside_on_each_side"),
    "longsword-pretilt-ejection": ('if not prepared or row["tick"] < tilt_start:', 'if False:',
                                   "test_longsword_pretilt_loss_is_not_ejection_evidence"),
    "longsword-phase-skew-ignored": ('fail(row, "longsword-phase-skew", (row["step"], row["door"], row["descending"]), step, tool=True)',
                                     'pass', "test_longsword_two_side_phase_skew_is_tool_error"),
    "longsword-door-without-landing-judged": ('if not negative and door_judged and tilts.get(last["tick"], {}).get("door")',
                                              'if not negative and tilts.get(last["tick"], {}).get("door")',
                                              "test_door_walk_needs_landing"),
    "longsword-door-never-judged": ('if not negative and door_judged and tilts.get(last["tick"], {}).get("door")',
                                    'if False and tilts.get(last["tick"], {}).get("door")',
                                    "test_longsword_host_landing_enables_door_judging_on_both_sides"),
    "longsword-client-tilt-is-coverage": ('if side == "host":\n            metrics["max_total_tilt"] = maximum',
                                         'if True:\n            metrics["max_total_tilt"] = maximum',
                                         "test_longsword_tilt_coverage_uses_the_hosts_counted_programme"),
    "client-seat-gap-hidden": ('fail(row, "driver-seat-missing", row["driver"], "an AI driver")', 'pass', "test_coop_client_missing_driver_is_desync"),
    "client-per-tick-gap-hidden": ('fail(row, "no-extrapolation", row["carries"], "a carry on every moving host tick")', 'pass', "test_coop_moving_host_without_client_write_is_desync"),
    "brush-max-travel": ('pushed = outward > PUSH_LIMIT',
                         'pushed = max(distance(watched[0]["world"], r["world"]) for r in watched) > PUSH_LIMIT',
                         "test_brush_push_is_directional"),
    "extrapolation-counts-writes": ('if row["carries"] == 0:', 'if row["writes"] == 0:',
                                    "test_no_extrapolation_uses_carries"),
    "own-coverage-capture-tick": ('paired = [r for r in own_samples if (player, r["unit"], r["simulation_tick"]) in own_captures]',
                                  'paired = [p for p in pairs if p["mode"] == "own" and p["player"] == player and p["client"]["phase"] == phase and p["tick"] >= p["client"]["board_tick"] + SETTLE_TICKS]',
                                  "test_own_coverage_uses_sample_ticks"),
    "verdicts-unjudged": ("judge_verdicts(pairs, records, fail)\n    # a client's own player", "None\n    # a client's own player",
                          "test_verdict_disagreement_is_desync"),
    "verdicts-no-grace": ("+ 1 > limit:", "+ 1 > 0:", "test_verdict_disagreement_is_desync"),
    "driver-gap-field-optional": ('board_tick driver_missing"', 'board_tick"', "test_parser_requires_driver_missing_field"),
    "driver-missing-index-assumed": ('and row["driver_missing"]:', 'and row["driver"] == -1:',
                                      "test_coop_driver_missing_reads_logged_field_not_driver_index"),
    "step-off-claim-exempt-forever": ("phase_age < allowance", "phase_age < 10**9",
                                      "test_step_off_claim_exemption_expires_and_world_is_compared"),
    "ride-walk-phase-unpinned": ('RIDE_WALK, RIDE_JUMP, RIDE_JUMP_PERIOD, RIDE_JUMPS = 600, 840, 90, 10',
                                 'RIDE_WALK, RIDE_JUMP, RIDE_JUMP_PERIOD, RIDE_JUMPS = 601, 840, 90, 10',
                                 "test_runner_phase_constants_match_network_test_source"),
    "alignment-coverage-ignored": ('metrics["alignment_coverage"][f"own:{player}:{phase}"] = share\n            if share < COVERAGE_LIMIT:',
                                   'metrics["alignment_coverage"][f"own:{player}:{phase}"] = share\n            if False:',
                                   "test_scored_phase_requires_half_aligned_samples"),
    "coverage-limit-zero": ("COVERAGE_LIMIT = .5", "COVERAGE_LIMIT = 0", "test_one_own_pair_is_not_coverage"),
    "agreed-death-called-desync": ("if off_both:", "if False:", "test_agreed_off_deck_death_is_scene_tool_error"),
    "different-decks-use-local-place": ("riding = claim(host_row) == claim(client_row) and ", "riding = ",
                                        "test_pair_on_different_decks_is_compared_in_the_world"),
    "remote-coverage-counts-unsettled": ('p["client"]["seq"] in settled_remote]', 'p["client"]["seq"] >= 0]',
                                         "test_remote_coverage_counts_settled_pairs_only"),
    "own-coverage-counts-unsettled": ('paired = [r for r in own_samples if (player, r["unit"], r["simulation_tick"]) in own_captures]',
                                      'paired = [r for r in first_samples.values() if r["phase"] == phase and (player, r["unit"], r["simulation_tick"]) in own_captures]',
                                      "test_own_coverage_counts_settled_pairs_only"),
    "death-rider-always-first": ("return ended[0]\n", "return 0\n", "test_agreed_off_deck_death_is_scene_tool_error"),
    "unattributed-death-guessed": ("for player in order[order.index(ended[0]):]):\n            return None  # unattributed",
                                   "for player in order[order.index(ended[0]):]):\n            return 0",
                                   "test_death_without_an_identifiable_rider_is_tool_error"),
    "death-riders-not-matched": ("victims[id(r)] == player and", "True and",
                                 "test_deaths_of_different_riders_are_not_agreed"),
    "unexpected-exception-escapes": ('                except Exception as error:\n                    result = {"status": "TOOL_ERROR",',
                                      '                except Exception as error:\n                    raise\n                    result = {"status": "TOOL_ERROR",',
                                      "test_main_turns_unexpected_judge_exception_into_tool_error"),
    "board-clock-unchecked": ('fail({}, "board-clock-disagreement", boards, "one host-issued tick", tool=True)',
                              'pass', "test_board_clock_disagreement_is_tool_error"),
    "clock-files-private": ('clock = output / "clock"', 'clock = output / side / "clock"',
                            "test_coop_clock_transport_is_shared_but_case_scoped"),
    "clock-files-global": ('clock = output / "clock"', 'clock = output.parent / "clock"',
                           "test_coop_clock_transport_is_shared_but_case_scoped"),
    "unknown-applied-host-state-ignored": ('if row["kind"] == "apply" and row["side"] == "client":', 'if False:',
                                           "test_unknown_applied_host_ticks_are_tool_error"),
    "places-limit-loosened": ("PLACE_LIMIT = 0.05", "PLACE_LIMIT = 0.06", "test_place_limit"),
    "height-limit-loosened": ("HEIGHT_LIMIT = -0.1", "HEIGHT_LIMIT = -0.2", "test_height_limit"),
    "settle-never": ("SETTLE_TICKS = 30", "SETTLE_TICKS = 100000", "test_place_limit"),
    "missing-ray-zero": ('fail(row, "contact-lost", None, "a surface")', 'pass', "test_no_hit_is_not_zero_height"),
    "landing-the-host-did-not-see-exempt": ("if windows(record)[1] <= tick <= last and host_off(record):",
                                            "if windows(record)[1] <= tick <= last:",
                                            "test_a_landing_the_host_did_not_see_is_judged"),
    "other-machines-reboard-counts": ("if record[\"kind\"] == \"reboard\" and owner.get(record[\"player\"]) == record[\"side\"] and windows(record):",
                                      "if record[\"kind\"] == \"reboard\" and windows(record):",
                                      "test_a_reboard_by_the_other_machine_is_not_a_landing"),
    "landing-never-exempt": ("def exempt(side, player, tick):", "def exempt(side, player, tick):\n        return False",
                             "test_off_deck_landing_with_a_reboard_is_an_outcome"),
    "other-copy-no-round-trip": ('last = record["tick"] if side == record["side"] else record["tick"] + rtt_ticks',
                                 'last = record["tick"]', "test_client_copy_of_the_hosts_landing_is_exempt_for_a_round_trip"),
    "other-copy-no-round-trip-host": ('last = record["tick"] if side == record["side"] else record["tick"] + rtt_ticks',
                                      'last = record["tick"]',
                                      "test_host_copy_of_the_clients_landing_is_exempt_for_a_round_trip"),
    "other-copy-unbounded": ('last = record["tick"] if side == record["side"] else record["tick"] + rtt_ticks',
                             'last = record["tick"] if side == record["side"] else record["tick"] + 10 ** 6',
                             "test_client_copy_is_judged_after_the_round_trip"),
    "other-copy-unbounded-host": ('last = record["tick"] if side == record["side"] else record["tick"] + rtt_ticks',
                                  'last = record["tick"] if side == record["side"] else record["tick"] + 10 ** 6',
                                  "test_host_copy_is_judged_after_the_round_trip"),
    "exempt-span-ends-late": ('last = record["tick"] if side == record["side"] else record["tick"] + rtt_ticks',
                              'last = record["tick"] + 10 if side == record["side"] else record["tick"] + rtt_ticks',
                              "test_height_a_few_ticks_after_the_reboard_is_judged"),
    "exempt-span-starts-before-the-press": ('start = max(record["landing_tick"], press_now)', 'start = record["landing_tick"]',
                                            "test_height_before_the_cycles_press_is_judged"),
    "echo-tail-unbounded": ("end = min(start + rtt_ticks + SETTLE_TICKS, press - 1)", "end = press - 1",
                            "test_place_error_after_the_echo_window_fails"),
    "echo-crosses-the-press": ("end = min(start + rtt_ticks + SETTLE_TICKS, press - 1)",
                               "end = start + rtt_ticks + SETTLE_TICKS",
                               "test_echo_window_does_not_cross_the_next_jump"),
    "echo-starts-early": ('        start = record["tick"]\n        end = min(', '        start = record["tick"] - 10\n        end = min(',
                          "test_place_error_just_before_the_reboard_is_judged"),
    "echo-starts-early-corrections": ('        start = record["tick"]\n        end = min(',
                                      '        start = record["tick"] - 10\n        end = min(',
                                      "test_corrections_just_before_the_reboard_are_judged"),
    "echo-claim-from-start": ('if echo(row["player"], row["simulation_tick"]) and key not in since:', "if False:",
                              "test_claim_disagreement_inside_the_window_counts_from_its_end"),
    "echo-everywhere": ("def echo(player, tick):", "def echo(player, tick):\n        return True",
                        "test_claim_disagreement_from_before_the_landing_still_fails"),
    "echo-corrections-many": ("if echoed_corrections[(row[\"player\"], cycle)] > 1:", "if False:",
                              "test_one_correction_in_the_window_is_allowed_two_are_not"),
    "convergence-skipped": ('fail(cs or hs or {"side": "client", "player": player}, "reboard-not-taken", end, "riding together")',
                            "pass", "test_a_rider_put_back_but_not_converged_is_a_desync"),
    "floor-loosened": ("if count > LANDED_OFF_LIMIT:", "if count > 3:", "test_landing_off_the_deck_floor"),
    "share-without-echo": ("(end - begin) / (cross - RIDE_JUMP)", "(record[\"tick\"] - begin) / (cross - RIDE_JUMP)",
                           "test_off_deck_landing_with_a_reboard_is_an_outcome"),
    "death-is-tool-error": ('"rider-life-changed":\n                fail(row, "rider-died", row["tick"], "alive")',
                            '"rider-life-changed":\n                fail(row, "error", row["reason"], "none", tool=True)',
                            "test_a_rider_that_died_is_judged_not_a_tool_error"),
    "death-completeness-expected": ("if not any(r[\"kind\"] == kind for r in rows) and not (kind == \"outcome\" and died):",
                                    "if not any(r[\"kind\"] == kind for r in rows):",
                                    "test_a_rider_that_died_is_judged_not_a_tool_error"),
    "error-is-judged": ('elif row["kind"] == "error":\n                fail(row, "error", row["reason"], "none", tool=True)\n            elif row["kind"] == "support_failure":',
                        'elif row["kind"] == "error":\n                fail(row, "error", row["reason"], "none")\n            elif row["kind"] == "support_failure":',
                        "test_another_error_is_a_tool_error"),
    "stand-correction-ignored": ('fail(row, "standing-correction", 1, 0)', 'pass', "test_standing_correction_is_desync"),
    "corrections-machine-wide": ('identity = (row["side"], row["player"], row["unit"])', 'identity = (row["side"],)',
                                 "test_corrections_are_per_player"),
    "corrections-nonrolling": ('tick - 60 < earlier <= tick', 'earlier <= tick', "test_rolling_corrections"),
    "incomplete-log-pass": ('"TOOL_ERROR" if tooling else "DESYNC" if reasons else "PASS", "required": required',
                            '"PASS" if tooling else "DESYNC" if reasons else "PASS", "required": required',
                            "test_incomplete_log_is_tool_error"),
    "align-same-tick": ('predicted = unwrap_tick(record["predicted_low16"], current)', 'predicted = record["host_tick"]',
                        "test_delayed_echo_uses_predicted_tick"),
    "ignore-predicted-wrap": ("return reference - ((reference - low16) & 0xffff)", "return low16", "test_unwrap_tick"),
    "remote-use-local-tick": ('state = authoritative.get((key[0], key[1], record["applied_host_tick"]))',
                              'state = authoritative.get((key[0], key[1], record["tick"]))',
                              "test_remote_uses_applied_host_tick"),
    "ignore-history": ('elif kind == "history":', 'elif kind == "not-history":', "test_history_before_the_echo_is_applied"),
    "self-correction-erases-error": ('for record in sorted(client, key=lambda r: r["seq"]):',
                                     'for record in sorted(client, key=lambda r: -r["seq"]):',
                                     "test_history_before_the_echo_is_applied"),
    "claim-grace-fixed": ('            elif elapsed > allowance:', "            elif elapsed > 15:",
                          "test_claim_uses_logged_grace"),
    "high-latency-hidden": ('"DESYNC" if reasons else "PASS", "required": required',
                            '"DESYNC" if reasons and required else "PASS", "required": required',
                            "test_high_latency_reported_not_required"),
    "gun-enclosed-allowed": ('if verdict == ENCLOSED:', 'if False:', "test_gun_enclosed_verdict_is_desync"),
    "brush-one-side-allowed": ('if pushed_sides == {"host", "client"}:', 'if pushed_sides:', "test_brush_one_machine_is_desync"),
    "driven-hurt-allowed": ('if row["body"] < first[0] or row["shield"] < first[1]:', 'if False:',
                            "test_driven_rider_hurt_is_desync"),
    "loss-as-fraction": ('"HALO_NETWORK_LOSS": str(loss_percent)', '"HALO_NETWORK_LOSS": str(loss_percent / 100)',
                         "test_side_environment_isolation_and_units"),
    "shared-data-root": ('"HALO_DATA_ROOT": str(data)', '"HALO_DATA_ROOT": str(output / "shared")',
                         "test_side_environment_isolation_and_units"),
    "kill-by-name": ("def stop_children(children):",
                     "def stop_children(children):\n    subprocess.run([\"taskkill\", \"/F\", \"/IM\", \"halo.exe\"])",
                     "test_stop_children_only_retained_handles"),
    "longsword-ejection-ignored": ('fail(row, "ejected", row["tick"], "inside")', "pass",
                                   "test_longsword_ejection_is_a_failure"),
    "longsword-streak-zero": ("if streak > EJECT_TICKS and not ejected:", "if streak > 0 and not ejected:",
                              "test_longsword_short_outside_is_not_an_ejection"),
    "longsword-health-fails": ('health[key] = first[0] + first[1] - row["body"] - row["shield"]',
                               'fail(row, "rider-hurt", row["body"], first)', "test_longsword_health_lost_is_reported_not_failed"),
    "longsword-death-ignored": ('else:\n                    fail(row, "rider-died", row["tick"], "alive")',
                                "else:\n                    pass", "test_longsword_death_is_a_failure"),
    "longsword-door-death-fails": ('metrics["died_in_door_walk"].append(f"{side}:{row[\'tick\']}")',
                                   'fail(row, "rider-died", row["tick"], "alive")',
                                   "test_longsword_death_in_the_door_walk_is_reported"),
    "longsword-door-ignored": ('fail(last, "door-blocked", last["tick"], "out and off the deck")', "pass",
                               "test_longsword_door_blocked_is_a_failure"),
    "longsword-tilt-not-required": ("if maximum <= 45.0:", "if False:", "test_longsword_requires_measured_tilt_past_limit"),
    "longsword-tilt-boundary-loose": ("if maximum <= 45.0:", "if maximum < 45.0:", "test_longsword_requires_measured_tilt_past_limit"),
    "longsword-lag-rejected": ('maximum = max(maximum, row["tilt"])',
                               'maximum = max(maximum, row["tilt"])\n            if row.get("error", 0) > 5:\n                fail(row, "tilt-lag", 6, 5, tool=True)',
                               "test_longsword_pilot_lag_is_not_a_tool_error"),
    "longsword-door-tip-is-coverage": ('if row["step"] in (5, 7, 9) and not row["door"] and not row["descending"]:', 'if True:',
                                       "test_longsword_tipping_during_the_door_walk_is_not_tilt_coverage"),
    "longsword-updates-ignored": ("if row[\"updates\"] <= previous:", "if False:",
                                  "test_longsword_uncounted_updates_are_a_tool_error"),
    "longsword-stock-map": ('f"host:{LONGSWORD_MAP}:slayer"', '"host:bloodgulch:slayer"', "test_longsword_environment"),
    "world-reference-is-applied-state": ('            reference = pair["world_host"]\n', '            reference = host_row\n',
                                         "test_remote_world_aligned_to_successor_state"),
    "aligned-residual-suppressed": ('        if error > PLACE_LIMIT:\n', '        if error > PLACE_LIMIT and reference is host_row:\n',
                                    "test_remote_world_genuine_residual_still_fails"),
    "world-reference-nearest": ('        metrics["max_place_error"] = error if',
                                '        error = min(error, distance(host_row["world"], client_row["world"])) if reference is not host_row else error\n'
                                '        metrics["max_place_error"] = error if',
                                "test_remote_world_genuine_residual_still_fails"),
    "place-reference-shifted": ('        riding = claim(host_row) == claim(client_row) and',
                                '        host_row = pair.get("world_host") or host_row\n'
                                '        riding = claim(host_row) == claim(client_row) and',
                                "test_remote_place_pairs_unchanged"),
    "missing-successor-unreported": ('            if pair["world_host"] is None:\n',
                                     '            if pair["world_host"] is None:\n                pair = dict(pair, world_host=host_row)\n'
                                     '            if False:\n',
                                     "test_remote_world_missing_successor_is_tool_error"),
    "successor-any-unit": ('authoritative.get((key[0], key[1], state["host_tick"] + 1))',
                           'next((r for k, r in authoritative.items() if k[0] == key[0] and k[2] == state["host_tick"] + 1), None)',
                           "test_remote_world_missing_successor_is_tool_error"),
    "stale-held-survives": ('                held.pop(key, None)\n                continue\n', '                continue\n',
                            "test_remote_world_missing_successor_is_tool_error"),
    "step-rule-unchecked": ('"one_step": record["simulation_tick"] == applied_tick', '"one_step": True',
                            "test_remote_pairing_sequence"),
    "held-not-consumed": ('(key, state["host_tick"]) not in scored:\n                    scored.add((key, state["host_tick"]))\n'
                          '                    del held[key]\n', 'True:\n', "test_remote_pairing_sequence"),
    "placement-rule-removed": ('and not riding and not pair["placed"]:\n', 'and not riding:\n',
                               "test_remote_world_placement_is_judged_against_the_applied_state"),
    "own-pairs-aligned": ('        reference = host_row\n        if pair["mode"] == "remote" and not riding and not pair["placed"]:\n',
                          '        reference = host_row\n        if not riding:\n'
                          '            pair.setdefault("one_step", True)\n'
                          '            pair.setdefault("world_host", next((r for r in records if r["kind"] == "state" and r["side"] == "host" and '
                          'r["player"] == pair["player"] and r["unit"] == client_row["unit"] and '
                          'r["host_tick"] == pair["host_tick"] + 1), None))\n'
                          '        if not riding:\n',
                          "test_own_path_unchanged"),
}


@pytest.mark.parametrize("control", sorted(NEGATIVE_CONTROLS))
def test_negative_control(control, tmp_path):
    before, after, test_name = NEGATIVE_CONTROLS[control]
    source = Path(ride.__file__).read_text(encoding="utf-8")
    assert source.count(before) == 1, f"negative control no longer applies: {before!r}"
    path = tmp_path / "mutated_runner.py"
    path.write_text(source.replace(before, after, 1), encoding="utf-8")
    spec = importlib.util.spec_from_file_location("mutated_runner", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    test = globals()[test_name]
    original = globals()["ride"]
    globals()["ride"] = module
    try:
        with pytest.raises(AssertionError):
            if test_name == "test_child_own_exit_before_cleanup":
                test(tmp_path, 0)
            elif test_name == "test_place_limit":
                test(.05001, "DESYNC")
            elif test_name == "test_height_limit":
                test(-.10001, "DESYNC")
            elif test_name == "test_rolling_corrections":
                test(60, "PASS")
            elif test_name == "test_landing_off_the_deck_floor":
                test(3, "DESYNC")
            elif test_name == "test_unwrap_tick":
                test(2, 65540, 65538)
            elif test_name in ("test_agreed_off_deck_death_is_scene_tool_error",
                               "test_agreed_death_with_deck_disagreement_remains_desync"):
                test(1)
            elif test_name == "test_main_turns_unexpected_judge_exception_into_tool_error":
                test(tmp_path, pytest.MonkeyPatch())
            elif test_name == "test_longsword_requires_measured_tilt_past_limit":
                test(45.0, "TOOL_ERROR")
            elif test_name == "test_longsword_preparation_requires_standing_inside_on_each_side":
                test("outside")
            elif test_name == "test_longsword_other_refusals_are_reported":
                test(1, "gone")
            elif test_name == "test_longsword_cabin_coverage_is_a_tool_error":
                test("tail")
            elif test_name == "test_longsword_limit_mismatch_is_a_tool_error":
                with pytest.MonkeyPatch.context() as monkeypatch:
                    test(monkeypatch)
            elif test_name.startswith("test_local_") and test_name in ("test_local_environment", "test_local_main", "test_local_launcher", "test_local_negative_main", "test_local_rejects_other_modes"):
                test(tmp_path)
            elif test_name in ("test_side_environment_isolation_and_units", "test_coop_clock_transport_is_shared_but_case_scoped",
                                    "test_parser_requires_driver_missing_field", "test_longsword_environment",
                                    "test_life_record_keeps_engine_cause_and_fails",
                               "test_parser_keeps_input_jump_integer", "test_parser_keeps_survey_jump_fractional",
                               "test_survey_keeps_both_driven_roles", "test_longsword_desync_exits_nonzero", "test_all_excludes_longsword"):
                test(tmp_path)
            else:
                test()
    finally:
        globals()["ride"] = original
