# Estimating Linux PPS bias

This repository contains tools to help estimate the systematic bias in kernel PPS timestamps under Linux.
By bias I mean the delay between the occurrence of the pulse and the time reported by the kernel through a `/dev/ppsN` device.
This bias will translate into inaccuracy in a stratum 1 NTP server using the PPS device as a reference clock.
On modern Raspberry Pis, this bias is typically of the order of 10 µs.
Note that the NTP daemon has no means to detect this inaccuracy and will not be able to report it.
However, NTP daemons can be configured to correct for the bias.
My [blog post](https://satpulse.net/2026/09/06/measuring-systematic-pps-bias-on-the-raspberry-pi-5.html) has more background.

The main program is `ppsbias` which estimates the bias by comparing the kernel PPS timestamps with the time of the pulse estimated by polling the GPIO.

The program has been tested most extensively on a Raspberry Pi 5,
with the estimates produced by the program being confirmed by two independent methods:

- using the PPS echo feature, together with an external time interval counter,
  and bpftrace to estimate the post timestamp part of the measured interval
- comparing the kernel timestamps with a system clock synchronized from the Raspberry Pi 5's PTP hardware clock (PHC),
  with the PHC synchronized via PTP to a PTP grandmaster connected by a back-to-back link

The program has also been confirmed to run on a Raspberry Pi 4 and Raspberry Pi 3B.

The systematic bias is often caused by wakeup latency on the platform I/O path.
Polling can itself affect this by keeping the hardware awake.
`ppsbias` avoids this by polling every other pulse
and estimating the bias from the timestamps of the pulses that are not polled.

In addition to `ppsbias`, there is also a tiny program `ppsecho` which enables the PPS echo feature:
this is for estimating bias with an external time interval counter such as a tinyGTC.

The code was written with AI assistance (Claude Fable 5.1 and Codex GPT 6 Astra).

## Build

```sh
sudo apt install build-essential linux-libc-dev pps-tools
make
sudo make install
```

`make check` runs tests without hardware (requires Python 3).

## ppsbias usage

`ppsbias` outputs the median bias in seconds, suitable for chrony's PPS refclock `offset` option.
The result should always be positive meaning the timestamp is late: `12.7e-6` means 12.7 µs.

Normal usage is to specify the `-m` option with the model of computer being used e.g.

```
ppsbias -m rpi5
```

The other values allowed for `-m` are `rpi4` and `rpi3`.
This assumes that PPS is on pin 12 (GPIO 18) and that the PPS device is `/dev/pps0`.
It will perform the estimate for 10 seconds.
The `-v` verbose option will show more information. `-t 60` will run for 60 seconds.

The full command line syntax is as follows.

```text
ppsbias -m model [-p ppsdev] [-g gpio] [-t seconds] [-e] [-v] [-w ms] [-s ms]
ppsbias -c chip -g gpio [-t seconds] [-e] [-v] [-w ms] [-s ms]
```

Either `-m` or `-c` must be specified.
`-m` is preferred: it reads GPIO registers while the PPS driver supplies timestamps.
It is specific to Raspberry Pi hardware.
`-c` is the portable backup, using GPIO ioctl reads and GPIO-event timestamps.
The events take a similar interrupt path to PPS, giving an order-of-magnitude estimate; `-m` measures the actual PPS timestamps and usually gives tighter polling brackets.

`-m` *model*\
Read GPIO registers for `rpi3`, `rpi4` or `rpi5`.

`-p` *ppsdev*\
Read PPS assert timestamps from *ppsdev*.
The default is `/dev/pps0`.
Requires `-m`.
The timestamps must represent the same GPIO's rising edge.

`-c` *chip*\
Read values and realtime rising-edge events from a GPIO v2 character device.
Requires `-g` and a free GPIO.
Cannot be combined with `-m` or `-p`.

`-g` *gpio*\
GPIO number within the controller, not a physical header pin number.
The default with `-m` is 18.

`-t` *seconds*\
Measure for the specified duration after initial synchronization.
The default is 10; an integer from 1 to 86400 is accepted.

`-e`\
Poll every pulse instead of alternating polled and unpolled pulses.

`-w` *ms*\
Set the polling window either side of the predicted edge.
*ms* is a floating point number in milliseconds.
The default is 1.

`-s` *ms*\
Set the minimum interval between the starts of GPIO reads, in milliseconds.
*ms* is a floating point number in milliseconds.
The default is 0.

`-v`\
Print individual observations and a final statistical summary, including median and maximum polling brackets.

`-h`\
Show help.

With the `-c` option, `ppsbias` cannot share the GPIO with the `pps-gpio` driver.
If `-c` reports that the device is busy, first stop programs using its PPS devices, such as chrony or ntpd.
Then unload the module.

```sh
sudo modprobe -r pps_gpio
```

After using `ppsbias`, reload with:

```
sudo modprobe pps_gpio
```

## ppsecho usage

```sh
sudo ppsecho /dev/pps0
```

The PPS driver must support assert echo and have an echo output pin configured.
ppsecho enables echo until Ctrl-C or SIGTERM, then disables it if it was not already enabled.
