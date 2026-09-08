/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/*
 * ppsbias: estimate how late the kernel timestamps a PPS edge on a GPIO
 * line.
 *
 * For every pulse n the kernel supplies a timestamp K[n], taken in hardirq
 * context with the REALTIME clock, the same way pps-gpio does.  On the
 * even-numbered pulses the program also estimates the edge time P[n] by
 * polling the line value in a tight loop around the predicted arrival: the edge is the
 * midpoint between the last low read and the first high read, each read
 * placed at the midpoint of the clock readings around it, reducing sensitivity
 * to read duration. Read asymmetry remains a source of systematic error.
 *
 * Polling keeps the interrupt path warm (PCIe link out of L1, a CPU busy),
 * which itself changes the kernel timestamp, so the odd-numbered pulses are
 * left alone.  On an odd pulse P[n] is not measured but computed as the midpoint
 * of its neighbours, (P[n-1] + P[n+1]) / 2, assuming stable pulse spacing
 * and approximately linear clock drift over two seconds.  The bias is K[n] - P[n] on the odd pulses:
 * how late the kernel stamp is on a second when nothing is polling.
 *
 * Usage: ppsbias [-c /dev/gpiochip0] [-l 18] [-w window_us] [-m] [-p /dev/ppsN] [-j] -d seconds
 *
 * -m polls by reading the GPIO input register through /dev/gpiomem0 instead
 *    of the get-values ioctl.  Raspberry Pi 5 (RP1) only.  The read is a bare
 *    MMIO load, so the bracket is narrower and has no syscall asymmetry.  It
 *    needs no ownership of the line, so it works while pps-gpio holds it.
 * -p takes the kernel timestamps from a PPS device (PPS_FETCH) instead of
 *    GPIO edge events, measuring exactly the stamp chrony consumes.  Combine
 *    with -m, or poll a second line wired to the same pulse.
 *
 * -P polls a different line (chip:offset) from the one that supplies edge
 *    events, e.g. one wired in parallel on a GPIO block with faster reads.
 * -s spaces value reads at least that many microseconds apart (busy-wait),
 *    trading bracket width for less bus traffic around the edge.
 *
 * -j prints one JSON line per pulse to stdout as the run proceeds, with the
 *    kernel timestamp, P and K - P, instead of
 *    the plain per-pulse rows.  An odd pulse is printed once the following
 *    even pulse has been measured; it carries poll_interpolated: true.
 *    Fields without a value are omitted.  These lines use every measured
 *    neighbour; the summary additionally rejects brackets wider than four
 *    times the median.
 *
 * Rising edges only.  Per-pulse rows go to stdout, the summary to stderr.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/timepps.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/gpio.h>

#define NS INT64_C(1000000000)

static volatile sig_atomic_t stop_requested;
static int pps_fd = -1;

static void on_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * NS + ts.tv_nsec;
}

static void sleep_until(int64_t t)
{
	struct timespec ts = { .tv_sec = t / NS, .tv_nsec = t % NS };

	while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL) == EINTR)
		if (stop_requested)
			return;
}

enum status { ST_UNPOLLED, ST_OK, ST_EARLY, ST_NOEDGE };

struct sample {
	int64_t kernel;		/* kernel edge timestamp, REALTIME ns */
	int64_t polled;		/* polled edge estimate, or 0 */
	int64_t bracket;	/* interval between the bracketing read instants */
	int64_t read_ns;	/* duration of the first-high read */
	int reads;		/* value reads in the window */
	int events;		/* edge events consumed for this pulse */
	enum status status;
	int idx;		/* pulse index */
};

static const char *status_name(enum status s)
{
	switch (s) {
	case ST_UNPOLLED: return "unpolled";
	case ST_OK: return "ok";
	case ST_EARLY: return "early";
	case ST_NOEDGE: return "noedge";
	}
	return "?";
}

/*
 * -j output: one JSON line per pulse.  src is 'm' (P measured), 'i' (P
 * interpolated from the neighbouring measured pulses) or 0 (no P, in which
 * case only the kernel timestamp is printed).
 */
static void emit_json(const struct sample *s, int64_t p, char src)
{
	printf("{\"pulse\": %d, \"kernel_ns\": %" PRId64, s->idx, s->kernel);
	if (src) {
		printf(", \"polled_ns\": %" PRId64, p);
		if (src == 'i')
			printf(", \"poll_interpolated\": true");
		printf(", \"bias_ns\": %" PRId64, s->kernel - p);
	}
	if (s->status == ST_OK)
		printf(", \"bracket_ns\": %" PRId64, s->bracket);
	printf("}\n");
	fflush(stdout);
}

/* Emit a held odd pulse, interpolating P from its neighbours if both were measured. */
static void emit_pending(struct sample *samples, int pending, const struct sample *right)
{
	const struct sample *odd = &samples[pending];
	const struct sample *left = pending > 0 ? &samples[pending - 1] : NULL;

	if (left && right && left->status == ST_OK && right->status == ST_OK &&
	    right->kernel - odd->kernel > NS / 2 &&
	    right->kernel - odd->kernel < 3 * NS / 2 &&
	    odd->kernel - left->kernel > NS / 2 &&
	    odd->kernel - left->kernel < 3 * NS / 2 &&
	    left->idx == odd->idx - 1 && right->idx == odd->idx + 1)
		emit_json(odd, (left->polled + right->polled) / 2, 'i');
	else
		emit_json(odd, 0, 0);
}

static int request_line(const char *chip, unsigned line, int edges)
{
	struct gpio_v2_line_request req;
	int cfd, r;

	cfd = open(chip, O_RDONLY | O_CLOEXEC);
	if (cfd < 0) {
		fprintf(stderr, "open %s: %s\n", chip, strerror(errno));
		return -1;
	}
	memset(&req, 0, sizeof req);
	req.offsets[0] = line;
	req.num_lines = 1;
	strncpy(req.consumer, "ppsbias", sizeof req.consumer - 1);
	req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
	if (edges)
		req.config.flags |= GPIO_V2_LINE_FLAG_EDGE_RISING |
				    GPIO_V2_LINE_FLAG_EVENT_CLOCK_REALTIME;
	req.event_buffer_size = 16;
	r = ioctl(cfd, GPIO_V2_GET_LINE_IOCTL, &req);
	close(cfd);
	if (r < 0) {
		fprintf(stderr, "request %s line %u: %s\n", chip, line, strerror(errno));
		return -1;
	}
	return req.fd;
}

static int use_mmio;

static int json;
static int poll_fd = -1;	/* line polled for values; may differ from the event line */
static int64_t spacing;	/* minimum interval between value reads, ns */
static int read_value_mmio(int64_t *t0, int64_t *t1);

static int read_value(int64_t *t0, int64_t *t1)
{
	struct gpio_v2_line_values v = { .bits = 0, .mask = 1 };

	if (use_mmio)
		return read_value_mmio(t0, t1);
	*t0 = now_ns();
	if (ioctl(poll_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
		return -1;
	*t1 = now_ns();
	return v.bits & 1;
}

/*
 * RP1 (Raspberry Pi 5) register polling through /dev/gpiomem0, which maps
 * 0x400d0000..0x400fffff: IO_BANK0..2, then SYS_RIO0..2 at +0x10000, then
 * PADS.  RIO_IN (+0x08) holds the synchronised pad inputs of one bank.
 */
static volatile uint32_t *rio_in;
static uint32_t rio_bit;

static int map_gpiomem(const char *path, unsigned line)
{
	static const struct { unsigned first, count; uint32_t rio; } bank[] = {
		{ 0, 28, 0x0000 }, { 28, 6, 0x4000 }, { 34, 20, 0x8000 },
	};
	unsigned b;
	int fd;
	void *base;

	for (b = 0; b < 3; b++)
		if (line >= bank[b].first && line < bank[b].first + bank[b].count)
			break;
	if (b == 3) {
		fprintf(stderr, "line %u is not an RP1 GPIO\n", line);
		return -1;
	}
	fd = open(path, O_RDONLY | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	base = mmap(NULL, 0x30000, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (base == MAP_FAILED) {
		fprintf(stderr, "mmap %s: %s\n", path, strerror(errno));
		return -1;
	}
	rio_in = (volatile uint32_t *)((char *)base + 0x10000 + bank[b].rio + 0x08);
	rio_bit = 1u << (line - bank[b].first);
	return 0;
}

static int read_value_mmio(int64_t *t0, int64_t *t1)
{
	uint32_t v;

	*t0 = now_ns();
	__sync_synchronize();
	v = *rio_in;
	__sync_synchronize();
	*t1 = now_ns();
	return (v & rio_bit) != 0;
}

/* Kernel timestamps from a PPS device instead of GPIO edge events. */
static pps_handle_t pps_handle;
static uint32_t pps_seq;
static int pps_have_seq;

static int open_pps(const char *path)
{
	pps_fd = open(path, O_RDWR);
	if (pps_fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (time_pps_create(pps_fd, &pps_handle) < 0) {
		fprintf(stderr, "time_pps_create %s: %s\n", path, strerror(errno));
		return -1;
	}
	return 0;
}

static int read_pps(int64_t after, int64_t deadline, int64_t *ts)
{
	int n = 0;

	for (;;) {
		pps_info_t info;
		struct timespec to;
		int64_t remain = deadline - now_ns(), t;

		if (remain <= 0 || stop_requested)
			return n;
		to.tv_sec = remain / NS;
		to.tv_nsec = remain % NS;
		if (time_pps_fetch(pps_handle, PPS_TSFMT_TSPEC, &info, &to) < 0) {
			if (errno == EINTR) {
				if (stop_requested)
					return n;
				continue;
			}
			if (errno == ETIMEDOUT)
				return n;
			fprintf(stderr, "time_pps_fetch: %s\n", strerror(errno));
			return n;
		}
		if (pps_have_seq && info.assert_sequence == pps_seq)
			continue;
		if (pps_have_seq) {
			uint32_t advance = (uint32_t)(info.assert_sequence - pps_seq);

			n = advance > (uint32_t)(INT_MAX - n) ? INT_MAX : n + (int)advance;
		} else {
			n = 1;
		}
		pps_seq = info.assert_sequence;
		pps_have_seq = 1;
		t = (int64_t)info.assert_timestamp.tv_sec * NS + info.assert_timestamp.tv_nsec;
		*ts = t;
		if (t > after)
			return n;
	}
}

/*
 * Consume edge events until one with a timestamp later than `after` is
 * found or the deadline passes.  Returns the number of events consumed and
 * stores the last timestamp; 0 means none arrived.
 */
static int read_event(int fd, int64_t after, int64_t deadline, int64_t *ts)
{
	int n = 0;

	if (pps_fd >= 0)
		return read_pps(after, deadline, ts);
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct gpio_v2_line_event ev;
		int64_t remain = deadline - now_ns();
		int r;

		if (remain <= 0 || stop_requested)
			return n;
		r = poll(&pfd, 1, (int)(remain / 1000000) + 1);
		if (r < 0 && errno == EINTR) {
			if (stop_requested)
				return n;
			continue;
		}
		if (r <= 0)
			return n;
		r = read(fd, &ev, sizeof ev);
		if (r != (int)sizeof ev) {
			fprintf(stderr, "event read: %s\n", r < 0 ? strerror(errno) : "short");
			return n;
		}
		if (ev.id != GPIO_V2_LINE_EVENT_RISING_EDGE)
			continue;
		n++;
		*ts = ev.timestamp_ns;
		if ((int64_t)ev.timestamp_ns > after)
			return n;
	}
}

static void poll_window(int64_t center, int64_t half, struct sample *s)
{
	int64_t t0, t1, prev_inst = 0, deadline = center + half;
	int prev = -1, reads = 0;

	s->status = ST_NOEDGE;
	sleep_until(center - half);
	for (;;) {
		int v = read_value(&t0, &t1);
		int64_t inst;

		if (v < 0) {
			fprintf(stderr, "get values: %s\n", strerror(errno));
			exit(1);
		}
		inst = t0 + (t1 - t0) / 2;
		reads++;
		if (prev < 0 && v) {
			s->status = ST_EARLY;
			break;
		}
		if (prev == 0 && v) {
			s->status = ST_OK;
			s->polled = prev_inst + (inst - prev_inst) / 2;
			s->bracket = inst - prev_inst;
			s->read_ns = t1 - t0;
			break;
		}
		prev = v;
		prev_inst = inst;
		if (t1 > deadline || stop_requested)
			break;
		if (spacing)
			while (!stop_requested && now_ns() < t0 + spacing)
				;
	}
	s->reads = reads;
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? -1 : x > y;
}

struct stats {
	int n;
	double mean, sd, se, median;
};

static struct stats compute(int64_t *v, int n)
{
	struct stats st = { .n = n };
	double sum = 0, sq = 0;
	int i;

	if (n == 0)
		return st;
	for (i = 0; i < n; i++)
		sum += v[i];
	st.mean = sum / n;
	for (i = 0; i < n; i++)
		sq += (v[i] - st.mean) * (v[i] - st.mean);
	st.sd = n > 1 ? sqrt(sq / (n - 1)) : 0;
	st.se = n > 1 ? st.sd / sqrt(n) : 0;
	qsort(v, n, sizeof *v, cmp_i64);
	st.median = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
	return st;
}

static void print_stats(const char *label, struct stats st)
{
	if (st.n == 0) {
		fprintf(stderr, "%-44s n    0  unavailable\n", label);
		return;
	}
	fprintf(stderr, "%-44s n %4d  mean %8.3f  median %8.3f  sd %6.3f  se %6.3f us\n",
		label, st.n, st.mean / 1000, st.median / 1000, st.sd / 1000, st.se / 1000);
}

static int64_t number(const char *s, int64_t min, int64_t max)
{
	char *end;
	long long value;

	errno = 0;
	value = strtoll(s, &end, 0);
	if (errno || !*s || *end || value < min || value > max) {
		fprintf(stderr, "invalid numeric argument: %s\n", s);
		exit(2);
	}
	return value;
}

static void *allocate(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size);

	if (!p) {
		perror("calloc");
		exit(1);
	}
	return p;
}

static void usage(int status)
{
	fprintf(status ? stderr : stdout, "usage: ppsbias [-c chip] [-l line] [-w window_us] [-m | -P chip:line] [-p ppsdev] [-s spacing_us] [-j] -d seconds\n");
	fprintf(status ? stderr : stdout,
		"  -c chip        GPIO chip (default /dev/gpiochip0)\n"
		"  -l line        GPIO line offset, not header pin (default 18)\n"
		"  -d seconds     Run duration after initial edge (required)\n"
		"  -w window_us   Polling half-window, 1..499999 (default 1000)\n"
		"  -s spacing_us  Minimum read spacing, 0..499999 (default 0)\n"
		"  -m             Poll RP1 through /dev/gpiomem0 (Pi 5 only)\n"
		"  -P chip:line   Poll a separate GPIO line; incompatible with -m\n"
		"  -p ppsdev      Get timestamps from a PPS device\n"
		"  -j             Emit JSON Lines instead of plain rows\n"
		"  -h             Show help\n");
	exit(status);
}

int main(int argc, char **argv)
{
	const char *chip = "/dev/gpiochip0", *gpiomem = "/dev/gpiomem0", *pps = NULL;
	char *pchip = NULL;
	unsigned line = 18, pline = 0;
	int64_t half = 1000 * 1000;	/* 1 ms each side */
	int duration = 0, opt, fd, i, n = 0, cap;
	int64_t period = NS, start, prev_ts, missing = 0;
	struct sample *samples;
	cpu_set_t cpus;

	while ((opt = getopt(argc, argv, "hc:l:w:d:mp:s:P:j")) != -1) {
		switch (opt) {
		case 'h': usage(0); break;
		case 'c': chip = optarg; break;
		case 'm': use_mmio = 1; break;
		case 'j': json = 1; break;
		case 's': spacing = number(optarg, 0, 499999) * 1000; break;
		case 'p': pps = optarg; break;
		case 'P': {
			char *colon = strrchr(optarg, ':');

			if (!colon || colon == optarg)
				usage(2);
			*colon = 0;
			pchip = optarg;
			pline = number(colon + 1, 0, UINT_MAX);
			break;
		}
		case 'l': line = number(optarg, 0, UINT_MAX); break;
		case 'w': half = number(optarg, 1, 499999) * 1000; break;
		case 'd': duration = number(optarg, 1, INT_MAX - 16); break;
		default: usage(2);
		}
	}
	if (duration <= 0 || optind != argc || (use_mmio && pchip))
		usage(2);

	/*
	 * Kernel stamps come from the PPS device or from edge events on the
	 * -c/-l line.  Values are polled from the -P line if given, else through
	 * gpiomem, else from the -c/-l line itself.
	 */
	fd = -1;
	if (!pps) {
		fd = request_line(chip, line, 1);
		if (fd < 0)
			return 1;
	}
	if (pchip) {
		poll_fd = request_line(pchip, pline, 0);
		if (poll_fd < 0)
			return 1;
	} else if (use_mmio) {
		if (map_gpiomem(gpiomem, line) < 0)
			return 1;
	} else if (fd >= 0) {
		poll_fd = fd;
	} else {
		poll_fd = request_line(chip, line, 0);
		if (poll_fd < 0)
			return 1;
	}
	if (pps && open_pps(pps) < 0)
		return 1;
	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	prctl(PR_SET_TIMERSLACK, 1000);
	/* Prefer the last allowed CPU; respect taskset and cpuset restrictions. */
	if (sched_getaffinity(0, sizeof cpus, &cpus) == 0) {
		for (i = CPU_SETSIZE - 1; i >= 0; i--)
			if (CPU_ISSET(i, &cpus))
				break;
		if (i >= 0) {
			CPU_ZERO(&cpus);
			CPU_SET(i, &cpus);
			if (sched_setaffinity(0, sizeof cpus, &cpus) < 0)
				perror("sched_setaffinity");
		}
	}

	cap = duration + 16;
	samples = allocate(cap, sizeof *samples);

	if (pchip)
		fprintf(stderr, "kernel stamps from %s, polling %s line %u via get-values ioctl\n",
			pps ? pps : "GPIO edge events", pchip, pline);
	else
		fprintf(stderr, "kernel stamps from %s, polling %s line %u via %s\n", pps ? pps : "GPIO edge events",
			chip, line, use_mmio ? gpiomem : "get-values ioctl");
	if (read_event(fd, 0, now_ns() + 3 * NS, &prev_ts) == 0) {
		fprintf(stderr, "no edge within 3 s\n");
		return 1;
	}
	start = prev_ts;
	int pending = -1;	/* samples[] index of an odd pulse awaiting its right neighbour */

	if (!json)
		printf("# idx status kernel_ns polled_ns bracket_ns read_ns reads events\n");
	for (i = 1; !stop_requested && n < cap && prev_ts - start < (int64_t)duration * NS; i++) {
		struct sample s = { 0 };
		int64_t center = prev_ts + period, ts = 0;
		int ev;

		if (!(i % 2))
			poll_window(center, half, &s);
		ev = read_event(fd, center - half, center + half + NS / 2, &ts);
		if (ev == 0 || ts <= center - half) {
			missing++;
			fprintf(stderr, "pulse %d: no edge event (%s)\n", i, status_name(s.status));
			if (json && pending >= 0) {
				emit_pending(samples, pending, NULL);
				pending = -1;
			}
			prev_ts = center;
			continue;
		}
		s.kernel = ts;
		s.events = ev;
		s.idx = i;
		if (!json)
			printf("%d %s %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %d %d\n", i,
		       status_name(s.status), s.kernel, s.polled, s.bracket, s.read_ns, s.reads, s.events);
		samples[n++] = s;
		if (json) {
			if (i % 2) {
				pending = n - 1;
			} else {
				if (pending >= 0)
					emit_pending(samples, pending, &samples[n - 1]);
				pending = -1;
				emit_json(&samples[n - 1], s.polled, s.status == ST_OK ? 'm' : 0);
			}
		}
		prev_ts = ts;
		if (n % 60 == 0)
			fprintf(stderr, "%d pulses\n", n);
	}
	if (json && pending >= 0)
		emit_pending(samples, pending, NULL);
	fflush(stdout);

	/* Analysis. */
	{
		int64_t *diff = allocate(n, sizeof *diff), *br = allocate(n, sizeof *br);
		int64_t *rd = allocate(n, sizeof *rd), *bias = allocate(n, sizeof *bias);
		bool *usable = allocate(n, sizeof *usable);
		int64_t thresh;
		int nd = 0, nb = 0, nr = 0, nbias = 0, early = 0, noedge = 0, polled = 0, wide = 0;
		int multi = 0, gaps = 0;
		struct stats sb, sd, sbias, sr;
		int64_t reads = 0;

		for (i = 0; i < n; i++) {
			if (samples[i].status != ST_UNPOLLED) {
				polled++;
				reads += samples[i].reads;
			}
			if (samples[i].status == ST_EARLY)
				early++;
			if (samples[i].status == ST_NOEDGE)
				noedge++;
			if (samples[i].events > 1)
				multi++;
			if (samples[i].status == ST_OK) {
				br[nb++] = samples[i].bracket;
				rd[nr++] = samples[i].read_ns;
			}
		}
		sb = compute(br, nb);
		sr = compute(rd, nr);
		thresh = (int64_t)(4 * sb.median);

		/* Even pulses: P[n] measured.  Reject implausibly wide brackets. */
		for (i = 0; i < n; i++)
			if (samples[i].status == ST_OK) {
				if (samples[i].bracket > thresh) {
					wide++;
					continue;
				}
				usable[i] = true;
				diff[nd++] = samples[i].kernel - samples[i].polled;
			}
		sd = compute(diff, nd);

		/*
		 * Odd pulses: P[n] = (P[n-1] + P[n+1]) / 2, requiring both
		 * neighbours measured and the three kernel stamps on
		 * consecutive periods.  Bias is K[n] - P[n].
		 */
		for (i = 1; i + 1 < n; i++) {
			int64_t k0, k1, k2, pn;

			if (samples[i].status != ST_UNPOLLED || !usable[i - 1] || !usable[i + 1])
				continue;
			k0 = (samples[i - 1].kernel - start + period / 2) / period;
			k1 = (samples[i].kernel - start + period / 2) / period;
			k2 = (samples[i + 1].kernel - start + period / 2) / period;
			if (k1 != k0 + 1 || k2 != k1 + 1 ||
			    samples[i - 1].idx != samples[i].idx - 1 ||
			    samples[i + 1].idx != samples[i].idx + 1) {
				gaps++;
				continue;
			}
			pn = (samples[i - 1].polled + samples[i + 1].polled) / 2;
			bias[nbias++] = samples[i].kernel - pn;
		}
		sbias = compute(bias, nbias);

		fprintf(stderr, "\n%s line %u, %s stamps, %s polling, %d pulses over %" PRId64 " s, window +/- %" PRId64 " us, spacing %" PRId64 " us\n",
			chip, line, pps ? "PPS" : "edge-event", use_mmio ? "gpiomem" : "ioctl", n,
			(prev_ts - start) / NS, half / 1000, spacing / 1000);
		fprintf(stderr, "polled seconds %d: ok %d, early %d, noedge %d, wide-bracket rejected %d; "
			"unpolled %d; missing events %" PRId64 "; multi-event pulses %d; grid gaps %d\n",
			polled, nb, early, noedge, wide, n - polled, missing, multi, gaps);
		if (polled)
			fprintf(stderr, "value reads per polled window: %.0f\n", (double)reads / polled);
		print_stats("value read duration (first-high read)", sr);
		print_stats("edge bracket", sb);
		print_stats("K - P, polled seconds (path kept warm)", sd);
		print_stats("K - P, unpolled seconds (P interpolated)", sbias);
		free(diff);
		free(br);
		free(rd);
		free(bias);
		free(usable);
	}
	free(samples);
	if (poll_fd >= 0 && poll_fd != fd)
		close(poll_fd);
	if (fd >= 0)
		close(fd);
	if (pps_fd >= 0)
		time_pps_destroy(pps_handle);
	return 0;
}
