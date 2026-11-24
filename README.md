# Estimating and correcting Linux GPIO PPS bias

This repository contains tools to help estimate the systematic bias in kernel PPS timestamps under Linux.
By bias I mean the delay between the occurrence of the pulse and the time reported by the kernel through a `/dev/ppsN` device.
This bias will translate into inaccuracy in a stratum 1 NTP server using the PPS device as a reference clock.
On modern Raspberry Pis, this bias is typically of the order of 10 µs.
The bias is often dramatically affected by power management, such as the CPU frequency governor.
Note that the NTP daemon has no means to detect this inaccuracy and will not be able to report it.
However, NTP daemons can be configured to correct for the bias.
My [blog post](https://satpulse.net/2026/09/06/measuring-systematic-pps-bias-on-the-raspberry-pi-5.html) has more background.

The main program is `ppsbias` which estimates the bias by comparing the kernel PPS timestamps with the time of the pulse estimated by polling the GPIO.

The program has been tested on the following machines:

* Raspberry Pi 5
* Raspberry Pi CM5
* Raspberry Pi 4
* Raspberry Pi CM4
* Raspberry Pi 3B
* Radxa ZERO 3E (RK3566)
* ODROID-M1 (RK3568)
* BeagleY-AI (TI AM67A)
* NXP FRDM-IMX8MPLUS (i.MX 8M Plus)

On machines that support PTP, it is possible to make an independent estimate of the bias by comparing the kernel timestamps with a system clock synchronized with a PTP hardware clock (PHC),
with the PHC synchronized via PTP to a PTP grandmaster connected by a back-to-back link or via a PTP-aware switch.
I have done this on the Raspberry Pi 5, ODROID-M1 and BeagleY-AI, and the results were consistent.
On ARM SBCs with PTP, the ingress and egress latencies are typically uncalibrated,
as shown by the measured path delay exceeding the cable delay,
so the accuracy of the PTP estimate is limited by the difference between the path delay and the cable delay.
It is also limited by the accuracy of the synchronization of the PHC and the system clock.

On the Raspberry Pi 5, I also made another independent estimate by using the PPS echo feature,
together with an external time interval counter,
and bpftrace to estimate the post-timestamp part of the measured interval.
Again the results were consistent.

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

`ppsbias` outputs the median bias in seconds suitable for use in an NTP daemon configuration file.
The result should always be positive meaning the timestamp is late: `12.7e-6` means 12.7 µs.

Normal usage is simply

```
ppsbias
```

It will perform the estimate for 10 seconds.
The `-v` verbose option will show more information. `-t 60` will run for 60 seconds.

The full command line syntax is as follows.

```text
ppsbias [-p ppsdev] [-t seconds] [-e] [-v] [-w ms] [-s ms]
ppsbias -c chip -g gpio [-t seconds] [-e] [-v] [-w ms] [-s ms]
```

The first form is preferred: it reads the level of the PPS device's GPIO from the GPIO controller's registers while the PPS driver supplies timestamps.
It finds the GPIO and its controller from the device tree.
It supports the GPIO controllers of the Raspberry Pi 3, 4 and 5, Rockchip SoCs with GPIO v2 (such as the RK3566, RK3568 and RK3588), TI K3 SoCs (such as the AM67A on the BeagleY-AI) and NXP i.MX SoCs (such as the i.MX 8M Plus).
On the Raspberry Pi the registers are read through `/dev/gpiomem` or `/dev/gpiomem0`, which the Raspberry Pi kernel provides,
or through `/dev/mem` if those are missing, as with a mainline kernel;
on the others they are read through `/dev/mem`, which needs a suitable kernel configuration (see below).
`-c` is the portable backup, using GPIO ioctl reads and GPIO-event timestamps.
The events take a similar interrupt path to PPS, giving an order-of-magnitude estimate; the first form measures the actual PPS timestamps and usually gives much tighter polling brackets.

`-p` *ppsdev*\
Read PPS assert timestamps from *ppsdev*, which must be a GPIO PPS device whose GPIO is given by the device tree.
The default is the only such device;
if there is more than one, `-p` is required.
Cannot be combined with `-c`.

`-c` *chip*\
Read values and realtime rising-edge events from a GPIO v2 character device.
Requires `-g` and a free GPIO.

`-g` *gpio*\
The line number within *chip*, not a physical header pin number.
Requires `-c`.

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
Print the configuration, individual observations and a final statistical summary, including median and maximum polling brackets.

`-h`\
Show help.

### Reading registers through /dev/mem

Without a gpiomem device, `ppsbias` maps the page holding the GPIO controller's level register read-only from `/dev/mem`.
That is always so on Rockchip, TI K3 and i.MX, and on a Raspberry Pi running a mainline kernel.
This needs:

* root, or more precisely `CAP_SYS_RAWIO`, to open `/dev/mem`;
* a kernel with `CONFIG_DEVMEM`, which is not in kernel lockdown (as it may be under UEFI Secure Boot);
* a kernel that allows `/dev/mem` to map registers a driver is using.
  The GPIO controller is always in use by its driver, so a kernel built with `CONFIG_IO_STRICT_DEVMEM` refuses the mapping with "Operation not permitted",
  unless it is booted with the kernel parameter `iomem=relaxed`.
  `CONFIG_STRICT_DEVMEM` without `CONFIG_IO_STRICT_DEVMEM` is fine: it restricts `/dev/mem` to non-RAM ranges.

To check the configuration of the running kernel:

```
grep STRICT_DEVMEM /boot/config-$(uname -r)
```

For example, the kernels of Raspberry Pi OS, the BeagleBoard.org Debian images and Armbian's `current-rockchip64` allow the mapping;
Fedora's kernel (Fedora 45) has `CONFIG_IO_STRICT_DEVMEM` and needs `iomem=relaxed`.

For accurate results, `ppsbias` needs to run on a different CPU from the one handling the PPS interrupt.
Use

```
grep -E 'CPU|pps' /proc/interrupts
```

to see which CPU is handling the PPS interrupt. If several CPUs have non-zero counts, look for which ones increase.
If this shows that, for example, CPU0 is handling the PPS interrupts, then use `taskset -c` to run on another CPU:

```
taskset -c 3 ppsbias
```

Use the `performance` CPU frequency governor while measuring.
`ppsbias` sleeps between pulses, so with a governor such as `ondemand` the CPU may be running slowly when the pulse arrives.
Each GPIO read then takes longer, and the polling brackets are wider.
To switch governor:

```
echo performance | sudo tee /sys/devices/system/cpu/cpufreq/policy*/scaling_governor
```

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

The configuration syntax depends on the NTP implementation.
With chrony it looks like:

```
refclock PPS /dev/pps0 offset 12.7e-6
```

With NTPSec:

```
refclock pps unit 0 time1 12.7e-6
```

With classic ntpd:


```
server 127.127.22.0
fudge 127.127.22.0 time1 12.7e-6
```


## ppsecho usage

```sh
sudo ppsecho /dev/pps0
```

The PPS driver must support assert echo and have an echo output pin configured.
ppsecho enables echo until Ctrl-C or SIGTERM, then disables it if it was not already enabled.
