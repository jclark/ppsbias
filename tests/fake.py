#!/usr/bin/env python3
"""Check the compiled fake GPIO/PPS implementation."""
import os
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parent.parent
fake = str(Path(__file__).resolve().with_suffix(''))

def run(args, expected=0, scenario=None, **kwargs):
    env = {**os.environ, 'PPSBIAS_TEST': scenario or ''}
    result = subprocess.run(args, cwd=root, text=True, capture_output=True,
                            timeout=15, env=env, **kwargs)
    assert result.returncode == expected, (args, scenario, result)
    return result

def rows(result):
    return [dict(field.split('=', 1) for field in line.split())
            for line in result.stdout.splitlines()]

def summary(result):
    r = rows(result)
    assert next(iter(r[-1])) == 'median', result
    return r[-1]

for model in ('rpi3', 'rpi4', 'rpi5'):
    args = [fake, '-m', model, '-t', '4', '-v']
    default = run(args)
    assert rows(default)[0]['pps'] == '/dev/pps0'
    assert default.stdout == run([*args, '-p', '/dev/pps0']).stdout
    override = run([*args, '-p', '/dev/pps1'])
    assert rows(override)[0]['pps'] == '/dev/pps1'
    assert summary(default) == summary(override)
for access in (['-c', 'fake', '-g', '18'], ['-m', 'rpi3', '-p', 'fakepps'],
               ['-m', 'rpi4', '-p', 'fakepps'], ['-m', 'rpi5', '-p', 'fakepps']):
    args = [fake, *access]
    for alternate in (False, True):
        mode = [] if alternate else ['-e']
        assert run([*args, '-v', *mode]).stdout == run([*args, '-t', '10', '-v', *mode]).stdout
        for duration in (1, 2, 4, 5, 6):
            expected = 3 if duration == 1 or (alternate and duration == 2) else 0
            result = run([*args, '-t', str(duration), '-v', *mode], expected)
            r = rows(result)
            observations = [x for x in r if 'timestamp' in x]
            assert duration - 1 <= len(observations) <= duration, result
            assert all(re.fullmatch(r'\d+\.\d{9}', x['timestamp']) for x in observations)
            if expected:
                assert 'no usable estimates' in result.stderr and 'median' not in r[-1]
                continue
            s = summary(result)
            assert not result.stderr, result
            assert s['samples'] == str((duration - 2) // 2 if alternate else duration - (duration % 2))
            assert ('stddev' in s) == (int(s['samples']) >= 2)
            assert s['mode'] == ('alternating' if alternate else 'every-pulse')
            population = [float(x['unpolledBias' if alternate else 'bias'])
                          for x in observations if ('unpolledBias' if alternate else 'bias') in x]
            for i, observation in enumerate(observations):
                if 'unpolledBias' in observation or 'unpolledStatus' in observation:
                    assert observation['unpolledTimestamp'] == observations[i - 1]['timestamp']
            assert abs(float(s['mean']) - sum(population) / len(population)) < 1.1e-7
            if alternate:
                assert observations[0].keys() == {'timestamp'}
                assert observations[1]['unpolledStatus'] == 'missing_neighbor'
            scalar = run([*args, '-t', str(duration), *mode]).stdout
            assert scalar == s['median'] + '\n'
    for scenario, expected in [('missing', 0), ('gap', 0), ('sigint', 130),
                               ('sigterm', 143), ('early_signal', 130), ('step', 1),
                               ('small_step', 1), ('small_step_back', 1)]:
        result = run([*args, '-t', '8', '-v', '-e'], expected, scenario)
        if scenario in ('early_signal', 'step', 'small_step', 'small_step_back'):
            assert 'median=' not in result.stdout
        else:
            s = summary(result)
            if scenario in ('missing', 'gap'):
                assert int(s['sourceFailures']) >= 1
            else:
                assert s['completion'] == ('interrupted' if scenario == 'sigint' else 'terminated')
    for scenario in ('small_step', 'small_step_back'):
        result = run([*args, '-t', '4', '-v'], 1, scenario)
        assert 'clock discontinuity detected' in result.stderr
        assert 'median=' not in result.stdout
ioctl = [fake, '-c', 'fake', '-g', '18', '-t', '8', '-v', '-e']
pps = [fake, '-m', 'rpi5', '-p', 'fakepps', '-t', '8', '-v', '-e']
for scenario in ('snapshot', 'offset', 'fallback', 'fresh', 'format_default'):
    r = run(pps, scenario=scenario)
    s = summary(r)
    assert int(s['samples']) == 8
    if scenario == 'offset':
        assert float(s['median']) > .002
    if scenario == 'fallback': assert rows(r)[0]['device'] == '/dev/mem'
for scenario in ('disabled', 'denied', 'fetch_io', 'params', 'format_bad'):
    r = run(pps, 1, scenario)
    assert 'median=' not in r.stdout
for scenario, expected in [('busy', 1), ('io', 1), ('high', 3)]:
    r = run(ioctl, expected, scenario)
    assert 'median=' not in r.stdout
    if scenario == 'busy': assert 'If pps-gpio owns' in r.stderr
    if scenario == 'high': assert 'pollStatus=initial_high' in r.stdout
for args in (pps, ioctl):
    for scenario in ('stopped', 'cleanup'):
        r = run(args, 1, scenario)
        s = summary(r)
        assert s['completion'] == 'failed' and int(s['samples']) > 0 and r.stderr
        scalar = run([arg for arg in args if arg != '-v'], 1, scenario)
        assert scalar.stdout == s['median'] + '\n' and scalar.stderr
for scenario in ('extra', 'slow_output'):
    r = run(ioctl, scenario=scenario)
    assert 'excludedTimestamp=1015.000001000 sourceStatus=multiple_events' in r.stdout
    assert 'resync=1' in r.stdout
r = run([arg for arg in ioctl if arg != '-e'] + ['-t', '6'], 3, 'missing')
assert 'bracket=' in r.stdout and 'sourceStatus=missing_event' in r.stdout
r = run([*ioctl, '-s', '0.5'])
assert float(summary(r)['bracketMax']) > .0004  # valid wide brackets are retained
r = run(ioctl, scenario='wide')
s = summary(r)
assert float(s['bracketMax']) > 100 * float(s['bracketMedian']) and s['samples'] == '8'
r = run(ioctl, scenario='late_read')
assert 'pollStatus=invalid_bracket' in r.stdout
with open('/dev/full', 'w') as full:
    r = subprocess.run(ioctl, stdout=full, stderr=subprocess.PIPE, text=True, timeout=15)
    assert r.returncode == 1 and 'stdout' in r.stderr
print('Fake GPIO/PPS checks passed.')
