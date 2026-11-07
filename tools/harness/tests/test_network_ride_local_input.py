"""The real local dequeue must run scripted input, without changing networked dequeue."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

SOURCE = "source/game/player_queues_new.c"
NEGATIVE_CONTROLS = {
    "local-dequeue-no-script": ("network_test_ride_action(local_player_index, action);", "(void)0;"),
    "local-dequeue-network-script": ("if (game_connection() == _game_connection_local)", "if (TRUE)"),
    "local-dequeue-wrong-player": ("network_test_ride_action(local_player_index, action);", "network_test_ride_action(queue_index, action);"),
    "local-dequeue-absent-player": ("if (local_player_index != NONE)\n\t\t\t\t\tnetwork_test_ride_action", "if (TRUE)\n\t\t\t\t\tnetwork_test_ride_action"),
}


def generated(control=None):
    source = read(SOURCE)
    code = function(source, "update_client_dequeue")
    if control:
        code = mutated(code, *NEGATIVE_CONTROLS[control])
    types = structure(read("source/game/players.h"), "player_action")
    types += structure(source, "update_client_queue_datum")
    code = function(source, "update_client_local_player_index") + code
    return (("local_input_types.inc", types), ("local_input_under_test.inc", code))


def test_local_dequeue_scripts_both_players_not_networked():
    status, output = run(build("network_ride_local_input", generated()), "local-input")
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("network_ride_local_input", generated(control)), "local-input")
    assert status == CHECK_FAILED, output
