#!/usr/bin/env python3
"""Check command-line parsing in the built programs."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent

def run(args, expected=0):
    result = subprocess.run(args, cwd=root, text=True, capture_output=True, timeout=15)
    assert result.returncode == expected, (args, result)
    return result

for tool in ('ppsbias', 'ppsecho'):
    assert 'usage:' in run([f'./{tool}', '-h']).stdout
    run([f'./{tool}', '--invalid'], 2)
run(['./ppsecho'], 2)
run(['./ppsecho', 'a', 'b'], 2)
base = ['./ppsbias', '-c', '/dev/null', '-g', '18', '-t', '1']
for opts in ([], ['-t', '1'], ['-c', '/dev/null', '-t', '1'],
             ['-m', 'bad', '-p', '/dev/null', '-t', '1'],
             ['-m', 'rpi5', '-p', '', '-t', '1'],
             ['-p', '/dev/null', '-t', '1'],
             ['-m', 'rpi3', '-p', '/dev/null', '-g', '54', '-t', '1'],
             ['-m', 'rpi4', '-p', '/dev/null', '-g', '58', '-t', '1'],
             ['-m', 'rpi5', '-p', '/dev/null', '-g', '54', '-t', '1']):
    run(['./ppsbias', *opts], 2)
for opts in (['-m', 'rpi3'], ['-p', '/dev/null'], ['-d', '1'], ['-l', '18'],
             ['-P', 'a:18'], ['-j'], ['-a'], ['extra']):
    run([*base, *opts], 2)
for option, invalid in {
    '-g': ['-1', '+1', '0x12', '4294967296', '1.0'],
    '-t': ['0', '86401', '1.5', '1e2', ' 1', '999999999999999999999'],
    '-w': ['0', '500', '499.999001', '0.0000009', '', '1ms', 'nan', 'inf', '1e999', '1e-999'],
    '-s': ['-1', '500', '', '1ms', 'nan', 'inf'],
}.items():
    for value in invalid:
        run([*base, option, value], 2)
# These pass CLI validation and then fail on the deliberately invalid device.
for opts in (['-g', '0'], ['-g', '4294967295'], ['-t', '86400'],
             ['-w', '0.000001'], ['-w', '499.999000'],
             ['-w', '250', '-s', '499.999'], ['-s', '1.999999']):
    run([*base, *opts], 1)
for value in ('1.0000001', '.1', '1.', '+1', '1e-3'):
    for option in ('-w', '-s'):
        run([*base, option, value], 1)
for opts in (['-s', '2'], ['-s', '3'], ['-w', '0.000001', '-s', '0.000002']):
    result = run([*base, *opts], 2)
    assert '-s must be less than twice -w' in result.stderr

print('Command-line checks passed.')
