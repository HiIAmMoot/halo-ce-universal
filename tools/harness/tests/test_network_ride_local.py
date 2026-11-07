"""Direct local startup uses the engine's local player creation, not a network lobby."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run

SOURCE = "port/linux/game/network_test.c"
CASES = ["local-start", "local-parse", "local-side", "local-only-longsword"]
NEGATIVE_CONTROLS = {
    "local-network-connection": ("game_connection_set(_game_connection_local);", "game_connection_set(_game_connection_network_server);", "local-start"),
    "local-one-player": ("player_spawn_count = 2;", "player_spawn_count = 1;", "local-start"),
    "local-shared-controller": ("player_ui_set_single_player_local_player_controller(1, 1);", "player_ui_set_single_player_local_player_controller(1, 0);", "local-start"),
    "local-wrong-map": ("main_set_map_name(network_test.map_name);", 'main_set_map_name("bloodgulch");', "local-start"),
    "local-no-slayer": ('game_set_game_variant_from_name("slayer");', 'game_set_game_variant_from_name("none");', "local-start"),
    "local-no-load": ("main_menu_switch_to_single_player();", "(void)0;", "local-start"),
    "local-no-close-menu": ("ui_widgets_close_all();\n\tgame_connection_set", "game_connection_set", "local-start"),
    "local-start-not-called": ("\t\t\tnetwork_test_start_local();", "\t\t\t(void)0;", "local-start"),
    "local-start-repeated": ("case _network_test_local:\n\t\tif (!network_test.started)", "case _network_test_local:\n\t\tif (TRUE)", "local-start"),
    "local-parsed-as-host": ("ride_local ? _network_test_local : _network_test_host", "_network_test_host", "local-parse"),
    "local-client-label": ("network_test.mode == _network_test_local && game_connection() == _game_connection_local", "FALSE", "local-side"),
    "local-any-scenario": ("if (!ride.enabled || ride.scenario != _ride_longsword)", "if (FALSE)", "local-only-longsword"),
}


def generated(control=None):
    source = read(SOURCE)
    assert "static void network_test_start_local(" in source, "local startup helper missing"
    if control:
        source = mutated(source, *NEGATIVE_CONTROLS[control][:2])
    types = enum_with(source, "_network_test_off") + enum_with(source, "_ride_straight")
    types += re.search(r"static struct\n\{.*?\} network_test;", source, re.S).group(0)
    code = "\n".join(function(source, name) for name in (
        "network_test_read_settings", "network_test_start_local", "ride_host"))
    update = function(source, "network_test_update")
    branch = update.split("case _network_test_local:", 1)[1].split("case _network_test_browse:", 1)[0]
    code += "\nstatic void local_frame(void) { switch (network_test.mode) { case _network_test_local:" + branch + "} }\n"
    return (("local_types.inc", types), ("local_under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("network_ride_local", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("network_ride_local", generated(control)), NEGATIVE_CONTROLS[control][-1])
    assert status == CHECK_FAILED, output
