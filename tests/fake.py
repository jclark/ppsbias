#!/usr/bin/env python3
"""Check the compiled fake GPIO/PPS implementation."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
fake = str(Path(__file__).resolve().with_suffix(''))
fixture = tempfile.TemporaryDirectory()
subprocess.run([str(Path(__file__).resolve().parent / 'fixture.sh'), fixture.name], check=True)
os.environ['PPSBIAS_TEST_ROOT'] = fixture.name

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

def fixture_with(*keep):
    """A fixture whose only /sys/class/pps entries are those named."""
    d = tempfile.TemporaryDirectory()
    subprocess.run([str(Path(__file__).resolve().parent / 'fixture.sh'), d.name], check=True)
    for entry in (Path(d.name) / 'sys/class/pps').iterdir():
        if entry.name not in keep:
            shutil.rmtree(entry)
    return d

def run_in(root, args, expected=0, scenario=None):
    saved = os.environ['PPSBIAS_TEST_ROOT']
    os.environ['PPSBIAS_TEST_ROOT'] = root
    try:
        return run(args, expected, scenario)
    finally:
        os.environ['PPSBIAS_TEST_ROOT'] = saved

# Without -p, the PPS device is the only one with a device-tree GPIO.
single = fixture_with('pps0', 'ptp')
default = run_in(single.name, [fake, '-t', '4', '-v'])
assert rows(default)[0]['pps'] == '/dev/pps0', default
assert default.stdout == run_in(single.name, [fake, '-t', '4', '-v', '-p', '/dev/pps0']).stdout
assert summary(default) == summary(run([fake, '-t', '4', '-v', '-p', '/dev/pps1']))
r = run([fake, '-t', '4'], 1)
assert 'more than one GPIO PPS device' in r.stderr and '/dev/pps0 /dev/pps1' in r.stderr and '-p' in r.stderr, r
assert '/dev/ptp' not in r.stderr and not r.stdout, r
none = fixture_with('ptp')
r = run_in(none.name, [fake, '-t', '4'], 1)
assert 'no GPIO PPS device found' in r.stderr and not r.stdout, r
with tempfile.TemporaryDirectory() as empty:
    r = run_in(empty, [fake, '-t', '4'], 1)
    assert 'no GPIO PPS device found' in r.stderr, r
# The backend comes from the controller node's compatible strings. The registers are read through
# the backend's gpiomem device or, without one, through /dev/mem at the translated address of the
# node's first register range.
for pps, backend, device, controller, address in [
        ('fakepps', 'rp1', '/dev/gpiomem0', '/axi/pcie@1000120000/rp1/gpio@d0000', None),
        ('ppsbcm', 'bcm2835', '/dev/gpiomem', '/soc/gpio@7e200000', None),
        ('pps2711', 'bcm2835', '/dev/gpiomem', '/soc4/gpio@7e200000', None),
        ('ppsrk', 'rockchip', '/dev/mem', '/pinctrl/gpio@fdd60000', '0xfdd60000'),
        ('ppsk3', 'k3', '/dev/mem', '/bus@f0000/gpio@601000', '0x601000'),
        ('ppsmxc', 'mxc', '/dev/mem', '/soc@0/bus@30000000/gpio@30200000', '0x30200000')]:
    r = run([fake, '-p', pps, '-t', '4', '-v'])
    first = rows(r)[0]
    assert first['access'] == backend and first['device'] == device, first
    assert first['controller'] == controller and first['gpio'] == '18', first
    assert first.get('address') == address, first
    assert 'median' in summary(r) and not r.stderr, r
# Without the gpiomem device, the Pi's registers are read through /dev/mem.
for pps, address in [('fakepps', '0x1f000d0000'), ('ppsbcm', '0x3f200000'), ('pps2711', '0xfe200000')]:
    first = rows(run([fake, '-p', pps, '-t', '4', '-v'], 0, 'fallback'))[0]
    assert first['device'] == '/dev/mem' and first['address'] == address, first
r = run([fake, '-p', 'ppsrk', '-t', '4'], 1, 'rk_v1')
assert 'unsupported Rockchip GPIO controller version' in r.stderr, r
r = run([fake, '-p', 'ppsk3', '-t', '4'], 1, 'ti_pid')
assert 'unsupported TI GPIO controller PID' in r.stderr, r
for pps, why in [('ppslow', 'active low'), ('nosuchpps', 'cannot read the PPS device name'),
                 ('ptp', 'no device-tree GPIO'), ('ppsnone', 'cannot find the GPIO controller'),
                 ('ppsbad', 'not supported (compatible: brcm,brcmstb-gpio)'),
                 ('ppsbcm54', 'outside the controller range'), ('ppsrk32', 'outside the controller range'),
                 ('ppsk3big', 'outside the controller range'), ('ppsnoranges', 'no ranges property')]:
    r = run([fake, '-p', pps, '-t', '4'], 1)
    assert why in r.stderr and not r.stdout, (pps, r)
# -m is obsolete: it is ignored, with a warning.
r = run([fake, '-m', 'rpi5', '-p', 'fakepps', '-t', '4'])
assert r.stderr == 'warning: -m is no longer needed and is ignored\n', r
assert r.stdout == run([fake, '-p', 'fakepps', '-t', '4']).stdout
for access in (['-c', 'fake', '-g', '18'], ['-p', 'fakepps'], ['-p', 'ppsbcm'], ['-p', 'ppsk3']):
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
pps = [fake, '-p', 'fakepps', '-t', '8', '-v', '-e']
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
# Warn on the median, in both output modes, without changing success or stdout.
for access in (['-c', 'fake', '-g', '18'], ['-p', 'fakepps']):
    for mode in ([], ['-e']):
        for spacing in ('0.0014', '0.0015', '0.0016', '0.01'):
            args = [fake, *access, *mode, '-t', '4', '-s', spacing]
            detailed = run([*args, '-v'])
            scalar = run(args)
            s = summary(detailed)
            assert scalar.stdout == s['median'] + '\n'
            assert scalar.stderr == detailed.stderr
            if float(spacing) > .0015:
                assert 'warning' in detailed.stderr.lower() and 'taskset' in detailed.stderr, detailed
                reported_us = [float(value) for value in
                               re.findall(r'(\d+(?:\.\d+)?)\s*(?:us|µs)', detailed.stderr)]
                median_us = float(s['bracketMedian']) * 1e6
                assert any(abs(value - median_us) < .001 for value in reported_us), detailed
            else:
                assert not detailed.stderr, detailed
r = run(ioctl, scenario='wide')
s = summary(r)
assert float(s['bracketMax']) > 100 * float(s['bracketMedian']) and s['samples'] == '8'
assert not r.stderr  # One broad bracket does not trigger a median warning.
r = run(ioctl, scenario='late_read')
assert 'pollStatus=invalid_bracket' in r.stdout
with open('/dev/full', 'w') as full:
    r = subprocess.run(ioctl, stdout=full, stderr=subprocess.PIPE, text=True, timeout=15)
    assert r.returncode == 1 and 'stdout' in r.stderr
print('Fake GPIO/PPS checks passed.')
