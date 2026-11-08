"""The host's map riding value, reliable and only when a map chooses."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run
from test_aim_assist import message_ids

CONTROLS = {'authority': ('machine_index != NONE ||', 'FALSE ||', 'admission'),
            'send-none': ('map == NONE', 'FALSE', 'replication'),
            'unchanged-spam': ('sent_map == map', 'FALSE', 'replication')}


def generated(fault=None):
    code = re.sub(r'^#include .*$', '', read('port/linux/game/network_riding_map.c'), flags=re.M)
    if fault:
        before, after, _ = CONTROLS[fault]
        code = mutated(code, before, after)
    return (('under_test.inc', code), ('message_ids.inc', message_ids()))


@pytest.mark.parametrize('case', ['replication', 'admission', 'reset'])
def test_case(case):
    status, output = run(build('riding_map_message', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault', sorted(CONTROLS))
def test_negative_control(fault):
    status, output = run(build('riding_map_message', generated(fault)), CONTROLS[fault][2])
    assert status == CHECK_FAILED and 'check failed:' in output, output


def test_message_ids():
    import network_format
    messages = {k: int(v) for k, v in network_format.items().items() if k.startswith('enum _distributed_message_')}
    assert messages['enum _distributed_message_vehicle_riding_map'] == 96
    assert len(set(messages.values())) == len(messages), messages
