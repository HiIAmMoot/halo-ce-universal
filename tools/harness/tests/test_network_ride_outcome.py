"""Consumed-launch episodes, independent of received ticks and support chatter."""
from pathlib import Path
import sys
import re
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import read, function, structure, mutated, build, run, CHECK_FAILED

CASES = ['chatter', 'ten-episodes', 'off-deck', 'driven-recovery', 'uncommanded']
CASES += ['remote-takeoff', 'remote-chatter', 'remote-no-support', 'owner-observation']
CONTROLS = {
    'remote-not-observed': ('ride_outcome_launch(rider, tick);\n\treturn TRUE;', 'return FALSE;', 'remote-takeoff'),
    'chatter-is-launch': ('upward < RIDE_OBSERVED_LAUNCH', 'FALSE', 'remote-chatter'),
    'remote-no-prior-support': ('!rider->physical_previous || ', '', 'remote-no-support'),
    'owner-observed': ('rider->local || ', '', 'owner-observation'),
    'landing-without-launch': ('if (!rider->episode_open || tick <= rider->launch_tick)', 'if (tick <= rider->launch_tick)', 'chatter'),
    'landing-not-terminal': ('rider->landings++;\n\t\trider->episode_open = FALSE;\n\t\treturn 1;', 'rider->landings++;\n\t\treturn 1;', 'ten-episodes'),
    'recovery-is-landing': ('rider->episode_recoveries++;', 'rider->episode_recoveries++; rider->landings++;', 'driven-recovery'),
    'duplicate-launch': ('if (rider->launch_tick == tick)', 'if (FALSE)', 'ten-episodes'),
}

def generated(control=None):
    source = read('port/linux/game/network_test.c')
    types = structure(source, 'ride_player')
    if 'static void ride_outcome_launch(' in source:
        code = function(source, 'ride_outcome_launch') + function(source, 'ride_outcome_sample')
        if 'static boolean ride_outcome_observe(' in source:
            code += function(source, 'ride_outcome_observe')
            code = re.search(r'^#define RIDE_OBSERVED_LAUNCH .*$', source, re.M).group(0) + '\n' + code
        else:
            types += '\n' if 'boolean local;' in types else ''
            types = types.replace('short jumps;', 'short jumps; boolean local;')
            code += 'static boolean ride_outcome_observe(struct ride_player *r,long t,boolean s,real v){return FALSE;}\n'
    else:
        # exercise the old transition counter verbatim against alternating ticks
        old = function(source, 'ride_rider_log')
        first = old[old.index('\tif (riding && rider->previous_ticks < RIDING_TICKS)'):old.index('\t/* (support after a recovery')]
        second = old[old.index('\tif (phase == _ride_phase_jump && rider->previous_ticks'):old.index('\tif (phase >= _ride_phase_step_off')]
        types = types.replace('short jumps;', 'short jumps; short episode_recoveries; short support_losses;')
        code = 'static void ride_outcome_launch(struct ride_player *rider, long tick) {}\n'
        code += 'static void ride_outcome_sample(struct ride_player *rider,long tick,boolean grounded,boolean riding,boolean recovery) { short phase=_ride_phase_jump;\n'
        code += first + second + '\nrider->previous_ticks = riding ? 60 : 59; }'
    if control:
        before, after, _ = CONTROLS[control]
        code = mutated(code, before, after)
    return (('types.inc', types), ('under_test.inc', code))

@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('network_ride_outcome', generated()), case)
    assert status == 0, output

@pytest.mark.parametrize('control', CONTROLS)
def test_control(control):
    status, output = run(build('network_ride_outcome', generated(control)), CONTROLS[control][2])
    assert status == CHECK_FAILED, output
