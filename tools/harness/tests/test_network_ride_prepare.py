"""Co-op riding preparation: a palette marine, client seat gap and host-issued clock."""
from pathlib import Path
import re
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_test.c"
CASES = ["ride-coop-driver-actor", "ride-coop-driver-client", "ride-coop-driver-no-variant", "ride-coop-driver-failure", "ride-palette-driver", "ride-driver-choice", "ride-local-driver", "ride-tilt-yaw", "ride-coop-prepare", "ride-host-clock", "ride-clock-torn", "ride-caller-readiness", "ride-caller-startup", "ride-players-ready", "ride-caller-remote"]
CASES += ["ride-coop-scripts-scoped", "ride-coop-script-disable-suspended",
          "ride-coop-control-inhibited-scored", "ride-coop-inhibited-then-enabled-before-hook",
          "ride-coop-delivered-then-disabled-before-hook", "ride-control-hook-off", "ride-prepare-arms-own-shield",
          "ride-prepare-no-shield-riding-off"]


def generated(control=None):
    source = read(SOURCE)
    hs = read("source/hs/hs_runtime.c")
    if control:
        entry = NEGATIVE_CONTROLS[control]
        before, after, _ = entry[-3:]
        if len(entry) == 4:
            hs = mutated(hs, before, after)
        else:
            source = mutated(source, before, after)
    types = "#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS 128\n"
    types += "".join(line + "\n" for line in re.findall(r"^#define RIDE_\w+ .*$", source, re.M))
    types += enum_with(source, "_ride_straight") + enum_with(source, "RIDE_BOARD_TICK")
    types += enum_with(source, "_ride_phase_prepare")
    types += structure(source, "ride_player")
    code = function(source, "ride_assign_driver")
    code += function(source, "ride_players_ready") + function(source, "ride_clock_tick") + function(source, "ride_prepare")
    code += function(source, "ride_phase_tick") + function(source, "ride_phase")
    for name in ("network_test_ride_control_delivered", "ride_suspend_scripts", "ride_monitor_controls"):
        code += function(source, name)
    caller = function(source, "network_test_ride_before_objects")
    call = re.search(r"ride_clock_tick\(ride_host\(\),[^;]+", caller).group(0)
    gate = re.search(r"if \(([^\n]+ride_local_ready\(\)[^\n]*)\)\n\s+return;", caller).group(1)
    code += f"\nstatic boolean ride_ungated_caller_tick(long tick, FILE *clock) {{ return {call}; }}\n"
    code += f"\nstatic boolean ride_caller_tick(long tick, FILE *clock) {{ if ({gate}) return FALSE; return {call}; }}\n"
    # Real guards and preparation/scored path, omitting only the later vehicle-motion loop.
    code += caller[:caller.index("\tfor (index = 0; index < ride_vehicle_count(); index++)")] + "}\n"
    code = function(hs, "hs_runtime_sleep_scripts") + function(hs, "hs_runtime_update") + code
    return (("ride_prepare_types.inc", types), ("ride_prepare_under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("network_ride_prepare", generated()), case)
    assert status == 0, output


NEGATIVE_CONTROLS = {
    "prepare-shield-missing": ("if (vehicle_riding_enabled() && !ride_host() && player->local_player_index != NONE)\n\t\t\tnetwork_distributed_own_reboard(player->local_player_index, player->unit_index, ride.vehicles[0]);",
                               "", "ride-prepare-arms-own-shield"),
    "prepare-shield-ignores-switch": ("if (vehicle_riding_enabled() && !ride_host()", "if (!ride_host()", "ride-prepare-no-shield-riding-off"),
    "control-inhibited-unlogged": ('ride_log("control_inhibited", "tick=%ld phase=%s player=%d", tick,',
                                   'ride_log("ignored", "tick=%ld phase=%s player=%d", tick,', "ride-coop-control-inhibited-scored"),
    "control-read-at-hook": ('if (!rider->control_delivered && !rider->control_inhibited)',
                             'if (!player_input_enabled() && !rider->control_inhibited)', "ride-coop-inhibited-then-enabled-before-hook"),
    "control-hook-ungated": ('if (!ride.enabled || !ride.prepared)\n\t\treturn;',
                             'if (FALSE)\n\t\treturn;', "ride-control-hook-off"),
    "control-hook-unprepared": ('if (!ride.enabled || !ride.prepared)\n\t\treturn;',
                                'if (!ride.enabled)\n\t\treturn;', "ride-control-hook-off"),
    "control-monitor-unwired": ('\t\tride_monitor_controls(tick);', '', "ride-coop-control-inhibited-scored"),
    "control-observation-stale": (' || rider->control_tick != tick)', ')', "ride-coop-control-inhibited-scored"),
    "scripts-suspend-test-off": ('if (!ride.enabled || !ride.prepared || ride.scripts_suspended || !ride_host() ||',
                                 'if (!ride.prepared || ride.scripts_suspended || !ride_host() ||', "ride-coop-scripts-scoped"),
    "scripts-suspend-other-scenarios": ('(ride.scenario != _ride_coop && ride.scenario != _ride_coop_double))\n\t{\n\t\treturn;',
                                        'FALSE)\n\t{\n\t\treturn;', "ride-coop-scripts-scoped"),
    "scripts-suspend-unwired": ('\t\tride_suspend_scripts();', '', "ride-coop-script-disable-suspended"),
    "scripts-not-slept": ('hs', '\t\tthread->sleep_until = NONE - 1;', '\t\tthread->sleep_until = 0;', "ride-coop-script-disable-suspended"),
    "coop-driver-no-actor": ("actor = actor_create_for_unit(FALSE, ride.driver, variant, NONE, NONE, FALSE, NONE, FALSE,\n\t\t\tactor_default_state_alert, actor_default_state_none, NONE, 0);", "actor = 88;", "ride-coop-driver-actor"),
    "coop-driver-not-braindead": ("actor_braindead(actor, TRUE);", "actor_braindead(actor, FALSE);", "ride-coop-driver-actor"),
    "coop-driver-wrong-variant": ("variant = entry->index;", "variant = 0;", "ride-coop-driver-actor"),
    "coop-driver-client-creates": ("if (!ride_host())\n\t\t\treturn FALSE;", "if (FALSE)\n\t\t\treturn FALSE;", "ride-coop-driver-client"),
    "coop-driver-variant-unmatched": ("if (variant == NONE)\n\t\t\treturn FALSE;", "if (variant == NONE)\n\t\t\tvariant = 2;", "ride-coop-driver-no-variant"),
    "coop-driver-actor-failure-seats": ("if (actor == NONE)\n\t\t\treturn FALSE;", "if (FALSE)\n\t\t\treturn FALSE;", "ride-coop-driver-failure"),
    "local-pilot-reversed": ("client = player->local_player_index != 0;", "client = player->local_player_index != 1;", "ride-local-driver"),
    "pilot-identify-seats": ("return !seat_driver || ride_seat(ride.driver, ride.vehicles[0]);",
                              "return ride_seat(ride.driver, ride.vehicles[0]);", "ride-driver-choice"),
    "pilot-choice-client-reversed": ("if (ride.scenario == _ride_longsword && !ride_host())",
                                      "if (FALSE)", "ride-driver-choice"),
    "caller-readiness-constant": ('ride_clock_tick(ride_host(), ride_local_ready(), tick, clock, ride.session, &ride.board_tick)',
                                  'ride_clock_tick(ride_host(), TRUE, tick, clock, ride.session, &ride.board_tick)', "ride-caller-readiness"),
    "remote-unit-ignored": ("if (player->unit_index == NONE)\n\t\t\treturn FALSE;", "if (FALSE)\n\t\t\treturn FALSE;", "ride-players-ready"),
    "remote-unit-dead-ignored": ("if (player->unit_index == NONE)\n\t\t\treturn FALSE;\n\t\tunit = unit_get(player->unit_index);\n\t\tif (TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ||", "if (player->unit_index == NONE)\n\t\t\treturn FALSE;\n\t\tunit = unit_get(player->unit_index);\n\t\tif (FALSE ||", "ride-players-ready"),
    "remote-player-count-ignored": ("return count >= 2;", "return count >= 0;", "ride-players-ready"),
    "host-remote-check-not-called": ("tick <= 0 || !ride_local_ready() || (ride_host() && !ride_players_ready())", "tick <= 0 || !ride_local_ready()", "ride-caller-remote"),
    "clock-torn-read-accepted": ("if (terminator != '\\n')", "if (FALSE)", "ride-clock-torn"),
    "caller-tick-zero-accepted": ("tick <= 0 || !ride_local_ready()", "!ride_local_ready()", "ride-caller-startup"),
    "marine-palette-ignored": ('definition = entry->reference.index;', 'definition = 0;', "ride-palette-driver"),
    "marine-absent-accepted": ('if (definition == NONE)\n\t\t\treturn FALSE;', 'if (definition == NONE)\n\t\t\tdefinition = 2;', "ride-palette-driver"),
    "host-driver-optional": ('ride_host() && ride.driver == NONE', 'FALSE', "ride-coop-prepare"),
    "host-player-driver-accepted": ('ride.driver != NONE && unit_get(ride.driver)->unit.player_index != NONE', 'FALSE', "ride-coop-prepare"),
    "client-seat-gap-aborts": ('ride_host() && ride.driver == NONE', 'ride.driver == NONE', "ride-coop-prepare"),
    "client-seat-gap-not-logged": ('&& ride.driver == NONE));', '&& FALSE));', "ride-coop-prepare"),
    "client-chooses-clock": ("if (host)\n", "if (TRUE)\n", "ride-host-clock"),
    "clock-not-published": ('fprintf(clock, "%s %ld\\n", session, received_tick)', "0", "ride-host-clock"),
    "clock-session-ignored": ("strcmp(received_session, session) || received_tick < tick ||", "received_tick < tick ||", "ride-host-clock"),
    "clock-late-accepted": ("received_tick < tick ||", "", "ride-host-clock"),
    "clock-local-input-ignored": ("!local_ready || !clock", "!clock", "ride-host-clock"),
    "tilt-yaw-host-only": ("\t\tride.tilt_yaw = 0.f;\n\n\t\tfor (player = 0;", "\t\tif (ride_host()) ride.tilt_yaw = 0.f;\n\n\t\tfor (player = 0;", "ride-tilt-yaw"),
    "clock-unbounded": ("received_tick > RIDE_READY_TIMEOUT + RIDE_PREPARE_TICKS", "FALSE", "ride-host-clock"),
}


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("network_ride_prepare", generated(control)), NEGATIVE_CONTROLS[control][-1])
    assert status == CHECK_FAILED, output
