#!/usr/bin/env python3
"""Hardware-free CLI and interpolation regression checks."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent

def run(args, expected=0):
    result = subprocess.run(args, cwd=root, text=True, capture_output=True, timeout=10)
    assert result.returncode == expected, (args, result)
    return result

for tool in ('ppsbias', 'ppsecho'):
    assert 'usage:' in run([f'./{tool}', '-h']).stdout
    run([f'./{tool}', '--invalid'], 2)
run(['./ppsecho', 'a', 'b'], 2)
for args in ([], ['-d', '0'], ['-d', '-1'], ['-d', '1x'],
             ['-d', '999999999999999999999'], ['-d', '1', '-w', '0'],
             ['-d', '1', '-s', '-1'], ['-d', '1', '-m', '-P', '/dev/null:18'],
             ['-d', '1', '-P', ':18'], ['-d', '1', '-P', '/dev/null:x'],
             ['-d', '1', 'extra']):
    run(['./ppsbias', *args], 2)
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'check.c'
    source.write_text(r'''
#define main ppsbias_main
#include "ppsbias.c"
#undef main
#include <assert.h>
int main(void) {
    int64_t values[] = { 10, 30, 20 };
    struct stats st = compute(values, 3);
    assert(st.n == 3 && st.mean == 20 && st.median == 20 && st.sd == 10);
    assert(compute(values, 0).n == 0);
    struct sample samples[] = {
        { .idx = 2, .status = ST_OK, .polled = 2 * NS, .kernel = 2 * NS + 100 },
        { .idx = 3, .status = ST_UNPOLLED, .kernel = 3 * NS + 500 },
        { .idx = 4, .status = ST_OK, .polled = 4 * NS, .kernel = 4 * NS + 100 }
    };
    emit_pending(samples, 1, &samples[2]);
    samples[2].idx = 6;
    emit_pending(samples, 1, &samples[2]);
    emit_pending(samples, 1, NULL);
    return 0;
}
''')
    binary = str(Path(tmp) / 'check')
    run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra',
         '-Werror', '-I', str(root), str(source), '-lm', '-o', binary])
    rows = [json.loads(row) for row in run([binary]).stdout.splitlines()]
    assert rows[0]['bias_ns'] == 500 and rows[0]['poll_interpolated'] is True
    assert all('polled_ns' not in row for row in rows[1:])
print('CLI, statistics, and interpolation checks passed (no hardware accessed).')
