# ppsbias

Two small Linux tools for investigating GPIO pulse-per-second (PPS) timing:

- **ppsbias** estimates the delay between a rising GPIO edge and its kernel
  timestamp, using GPIO value polling as a reference. It alternates polled and
  unpolled seconds to estimate bias when the poller is inactive.
- **ppsecho** enables a PPS device's assert echo until interrupted, for use with
  an external time interval counter.

These are measurement tools for a stable **1 Hz, rising-edge** signal.
`ppsbias` does not adjust the system clock or calibrate a time daemon.
GPIO ioctl polling is the general Linux path; direct register polling is
specific to Raspberry Pi 5's RP1 and its `/dev/gpiomem0` mapping.

## Build and install

You need a C compiler, make, Linux GPIO v2 headers, and `sys/timepps.h`
(from pps-tools). No libgpiod library is required.
On Debian / Raspberry Pi OS, the build dependencies can be installed with:

```sh
sudo apt install build-essential linux-libc-dev pps-tools
make
./ppsbias -h
./ppsecho -h
```

Optional installation (defaults to `/usr/local/bin`):

```sh
sudo make install
# sudo make uninstall
```

`CC`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, `LDLIBS`, `PREFIX`, `BINDIR`, and
`DESTDIR` can be overridden. `make clean` removes the binaries.
`make check` runs hardware-free CLI, statistics, and interpolation checks;
it additionally requires Python 3.

## Measure with ppsbias

Use GPIO **line offsets**, not physical header pin numbers. Identify the GPIO
chip and offset for your board before running. Access requires appropriate
permissions on the devices; the examples use `sudo`.

For a GPIO line that is not already claimed by a driver:

```sh
sudo ./ppsbias -c /dev/gpiochip0 -l 18 -d 120 > pulses.txt 2> summary.txt
```

This requests a rising-edge input with realtime event timestamps and polls
its value through the GPIO character-device API. The line must support
GPIO v2 realtime edge events.

To measure timestamps from an existing PPS device on a Pi 5:

```sh
sudo ./ppsbias -m -l 18 -p /dev/pps0 -d 120 -j > pulses.jsonl 2> summary.txt
```

Here `/dev/pps0` must timestamp the same signal as RP1 GPIO 18. `-m` reads
RP1 input registers through `/dev/gpiomem0`, without requesting ownership
of the line. It assumes the RP1 register layout and mapping; use it only on
that hardware. Some kernels do not provide that device. There is no
`/dev/mem` fallback.

Alternatively, wire the same signal to a second, free GPIO line and use
`-P /dev/gpiochipN:OFFSET` to poll it. This can be combined with `-p` or
with GPIO event timestamps on the `-c`/`-l` line. `-P` and `-m` are mutually
exclusive. With `-p` alone, the polling line still needs to be free; a line
owned by `pps-gpio` cannot also be requested by this program.

| Option | Meaning | Default |
| --- | --- | --- |
| `-c chip` | GPIO chip for event capture / ordinary polling | `/dev/gpiochip0` |
| `-l line` | GPIO offset; RP1 offset when using `-m` | `18` |
| `-d seconds` | Duration after acquiring the initial edge | required |
| `-w window_us` | Half-width of the polling window, 1–499999 µs | `1000` |
| `-s spacing_us` | Minimum time between read starts, 0–499999 µs | `0` |
| `-m` | Poll RP1 through `/dev/gpiomem0` | off |
| `-P chip:line` | Poll a separate GPIO input | off |
| `-p ppsdev` | Use PPS assert timestamps instead of GPIO events | off |
| `-j` | JSON Lines output | off |
| `-h` | Help | |

Ctrl-C or SIGTERM ends collection and prints the available summary.
The program first waits up to three seconds for an edge. It attempts to pin
itself to the highest-numbered CPU in its allowed affinity mask; use
`taskset` to select a different CPU. IRQ affinity is not changed. Keep the
poller and the input interrupt on different CPUs when investigating their
interaction, and record the configuration with your results.

## Method and interpretation

Let `K[n]` be the kernel timestamp for pulse `n`. On even-numbered pulses,
`ppsbias` brackets the transition between the last low GPIO read and the
first high read. Each read is assigned the midpoint of the realtime clock
readings around it; the midpoint between those read instants is the edge
estimate `P[n]`.

On odd-numbered pulses, the program does not poll. It estimates:

```text
P[n] = (P[n-1] + P[n+1]) / 2
bias[n] = K[n] - P[n]
```

Positive bias means the kernel timestamp is later than the estimated edge.
Interpolation assumes stable pulse spacing and approximately linear clock
drift over the two-second interval. Polling can affect CPU and bus activity,
so the summary reports polled and unpolled seconds separately.

The summary rejects measured brackets wider than four times the median.
Unpolled estimates require usable measurements on both adjacent pulses and
consecutive positions on the one-second timestamp grid. Empty statistics
are reported as unavailable. Summary times are in microseconds; `sd` is
sample standard deviation and `se` is `sd / sqrt(n)`.

This is an estimate, not an absolute calibration. GPIO read asymmetry,
register synchronisation, source jitter, and polling-induced changes can
bias it. Standard error does not include these systematic effects or
account for correlated samples. Clock steps invalidate the time model and
can disrupt scheduling; avoid stepping the clock during a run. A configured
PPS assert offset is included in fetched timestamps. GPIO event timestamps
and PPS timestamps follow different kernel paths, so use `-p` to examine the
PPS timestamps consumed by a time daemon.

### Output

Plain stdout starts with:

```text
# idx status kernel_ns polled_ns bracket_ns read_ns reads events
```

`status` is `unpolled`, `ok`, `early` (first read was already high), or
`noedge` (no transition found in the window). Zero polling fields on
unsuccessful or unpolled rows are placeholders. Missing kernel events are
reported on stderr and have no stdout row. `events` counts consumed GPIO
events or the PPS sequence advance, not a complete event history.

With `-j`, stdout contains one JSON object per captured pulse. `pulse` and
`kernel_ns` are always present. `polled_ns` and `bias_ns` are included when
an edge estimate is available; interpolated estimates also include
`"poll_interpolated": true`. Successful measured pulses include
`bracket_ns`. Odd pulses are held until the following pulse arrives, so
output can be delayed by a second. Boundary pulses may have no estimate.
JSON estimates are emitted before the summary's wide-bracket filtering.
Timestamps and differences in both stdout formats are integer nanoseconds.
Use a JSON reader that preserves 64-bit integers for epoch timestamps.

## Enable PPS echo

```sh
sudo ./ppsecho /dev/pps0
```

The PPS device must advertise `PPS_ECHOASSERT`, and its driver and board
configuration must provide an echo output. This tool only enables the PPS
mode bit; it does not configure GPIO pins or device-tree overlays.

SIGINT (Ctrl-C) or SIGTERM restores the previous assert-echo setting while
preserving other current PPS parameters. Avoid simultaneous echo controllers:
the mode is shared device state. SIGKILL and crashes cannot perform cleanup.

Feed a common PPS signal to the board and an external counter, and measure
the echo output against that signal. Use electrically compatible signal
levels and a common ground. The measured round trip includes the input
interrupt path, kernel work after timestamping, and output propagation.
An echo handled in an IRQ thread can also include thread scheduling delay;
the raw echo interval is not the PPS timestamp bias.

## License

MIT. See [LICENSE](LICENSE).
