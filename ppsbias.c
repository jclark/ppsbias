/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* Compare physical 1 Hz rising GPIO edges with kernel timestamps. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/gpio.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/timepps.h>
#include <time.h>
#include <unistd.h>

#define NS INT64_C(1000000000)
_Static_assert(sizeof(off_t) >= 8, "64-bit mmap offsets required");
static volatile sig_atomic_t stopped;
static bool fatal, clock_failed, verbose;

static void on_stop(int sig) { stopped = sig; }

static void fail(const char *msg) { fprintf(stderr, "%s\n", msg); fatal = true; }

static void syserror(const char *msg) { perror(msg); fatal = true; }

static int64_t add(int64_t a, int64_t b)
{
	int64_t r;
	if (__builtin_add_overflow(a, b, &r)) { fail("timestamp arithmetic overflow"); return 0; }
	return r;
}

static int64_t sub(int64_t a, int64_t b)
{
	int64_t r;
	if (__builtin_sub_overflow(a, b, &r)) { fail("timestamp arithmetic overflow"); return 0; }
	return r;
}

static int64_t timespec_ns(struct timespec t, bool offset)
{
	int64_t s = (int64_t)t.tv_sec, r;
	if ((time_t)s != t.tv_sec || t.tv_nsec <= -NS || t.tv_nsec >= NS ||
	    (!offset && (s < 0 || t.tv_nsec < 0)) || __builtin_mul_overflow(s, NS, &r)) {
		fail("timestamp outside supported ABI/range"); return 0;
	}
	return add(r, t.tv_nsec);
}

static int64_t now(clockid_t id)
{
	struct timespec t;
	if (clock_gettime(id, &t) < 0) { syserror("clock_gettime"); return 0; }
	return timespec_ns(t, false);
}

static int64_t minimum(int64_t a, int64_t b) { return a < b ? a : b; }

static void sleep_until(int64_t t)
{
	struct timespec ts = { .tv_sec = (time_t)(t / NS), .tv_nsec = t % NS };
	if ((int64_t)ts.tv_sec != t / NS) { fail("deadline outside time_t range"); return; }
	while (!stopped && !fatal) {
		int r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		if (!r) return;
		if (r != EINTR) { errno = r; syserror("clock_nanosleep"); return; }
	}
}
/* Realtime-minus-monotonic interval: uncertainty includes clock read latency.
 * Compare successive intervals, allowing 100 us beyond that uncertainty. */
struct clocks { int64_t low, high; };
static struct clocks clocks;
static bool have_clocks;

static int64_t check_clocks(void)
{
	int64_t m0 = now(CLOCK_MONOTONIC), r = now(CLOCK_REALTIME), m1 = now(CLOCK_MONOTONIC);
	struct clocks c = { sub(r, m1), sub(r, m0) };
	if (m1 < m0 || (have_clocks &&
	    (sub(c.low, clocks.high) > 100000 || sub(clocks.low, c.high) > 100000))) {
		clock_failed = true; fail("clock discontinuity detected");
	}
	clocks = c; have_clocks = true;
	return add(c.low, sub(c.high, c.low) / 2);
}

struct model { const char *name, *device, *access; uint64_t physical; size_t span; unsigned count; };
static const struct model models[] = {
	{ "rpi3", "/dev/gpiomem", "bcm", UINT64_C(0x3f200000), 0x1000, 54 },
	{ "rpi4", "/dev/gpiomem", "bcm", UINT64_C(0xfe200000), 0x1000, 58 },
	{ "rpi5", "/dev/gpiomem0", "rp1", UINT64_C(0x1f000d0000), 0x30000, 54 }
};
struct source {
	int fd;
	bool pps, have_seq, canwait;
	uint32_t seq, line_seq;
	int caps, mode;
	int64_t offset;
	void *mapping;
	size_t map_length;
	volatile uint32_t *level;
	uint32_t mask;
	const char *device;
};

static size_t register_offset(const struct model *m, unsigned gpio, uint32_t *mask)
{
	unsigned bit = gpio % 32;
	size_t off = 0x34 + 4 * (gpio / 32);
	if (!strcmp(m->access, "rp1")) {
		unsigned bank = gpio < 28 ? 0 : gpio < 34 ? 1 : 2;
		bit = gpio - (bank == 0 ? 0 : bank == 1 ? 28 : 34);
		off = 0x10008 + bank * 0x4000;
	}
	*mask = UINT32_C(1) << bit;
	return off;
}

static void map_registers(struct source *s, const struct model *m, unsigned gpio)
{
	uint64_t physical = 0;
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0) { fail("cannot determine page size"); return; }
	s->device = m->device;
	int fd = open(s->device, O_RDONLY | O_SYNC | O_CLOEXEC);
	if (fd < 0 && errno == ENOENT) {
		s->device = "/dev/mem"; physical = m->physical;
		fd = open(s->device, O_RDONLY | O_SYNC | O_CLOEXEC);
	}
	if (fd < 0) { syserror(s->device); return; }
	size_t displacement = physical % (uint64_t)page;
	off_t offset = (off_t)(physical - displacement);
	s->map_length = ((m->span + displacement + page - 1) / page) * page;
	s->mapping = mmap(NULL, s->map_length, PROT_READ, MAP_SHARED, fd, offset);
	if (s->mapping == MAP_FAILED) { s->mapping = NULL; syserror(s->device); }
	if (close(fd) < 0) syserror("close memory device");
	if (s->mapping)
		s->level = (volatile uint32_t *)((char *)s->mapping + displacement + register_offset(m, gpio, &s->mask));
}

static void request_line(struct source *s, const char *chip, unsigned gpio)
{
	struct gpiochip_info info = {0};
	struct gpio_v2_line_request req = {0};
	int fd = open(chip, O_RDONLY | O_CLOEXEC);
	s->device = chip;
	if (fd < 0) { syserror(chip); return; }
	if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) syserror("GPIO_GET_CHIPINFO_IOCTL");
	else if (gpio >= info.lines) fail("GPIO outside chip range");
	if (!fatal) {
		req.offsets[0] = gpio; req.num_lines = 1; req.event_buffer_size = 16;
		strcpy(req.consumer, "ppsbias");
		req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EVENT_CLOCK_REALTIME;
		if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
			int e = errno;
			fprintf(stderr, "GPIO %u on %s: %s\n", gpio, chip, strerror(e));
			if (e == EBUSY) fprintf(stderr, "GPIO %u on %s is already in use.\nIf pps-gpio owns this input, stop programs using its PPS device,\nthen unload it with: sudo modprobe -r pps_gpio\n", gpio, chip);
			fatal = true;
		} else s->fd = req.fd;
	}
	if (close(fd) < 0) syserror("close gpiochip");
}

/* Linux stores the driver's initial parameter mask verbatim. A fresh source
 * may omit format bits even though PPS_FETCH returns timespecs. The Linux PPS
 * API defaults an unspecified format to TSPEC; no parameter write is needed. */
static int pps_relevant_mode(int mode)
{
	int format = mode & (PPS_TSFMT_TSPEC | PPS_TSFMT_NTPFP);
	return (mode & PPS_CAPTUREASSERT) | (format ? format : PPS_TSFMT_TSPEC);
}

static void pps_parameters(struct source *s, bool initial)
{
	pps_params_t p;
	if (time_pps_getparams(s->fd, &p) < 0) { syserror("PPS_GETPARAMS"); return; }
	int relevant = pps_relevant_mode(p.mode);
	int64_t offset = p.mode & PPS_OFFSETASSERT ? timespec_ns(p.assert_offset, true) : 0;
	if (!(s->caps & PPS_CAPTUREASSERT) || !(s->caps & PPS_TSFMT_TSPEC) ||
	    relevant != (PPS_CAPTUREASSERT | PPS_TSFMT_TSPEC))
		fail("PPS requires enabled assert capture and timespec timestamps");
	if (!initial && (relevant != pps_relevant_mode(s->mode) || offset != s->offset))
		fail("PPS capture, format, or effective assert offset changed");
	if (initial) { s->mode = p.mode; s->offset = offset; }
}

static void open_pps(struct source *s, const char *path)
{
	s->pps = true;
	s->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (s->fd < 0) { syserror(path); return; }
	pps_handle_t handle;
	if (time_pps_create(s->fd, &handle) < 0 || time_pps_getcap(s->fd, &s->caps) < 0) {
		syserror(path); return;
	}
	pps_parameters(s, true);
	s->canwait = (s->caps & PPS_CANWAIT) != 0;
}

static void cleanup(struct source *s)
{
	if (s->mapping && munmap(s->mapping, s->map_length) < 0) syserror("munmap");
	/* Linux time_pps_destroy is close(handle); close exactly once, also on setup failure. */
	if (s->fd >= 0 && close(s->fd) < 0) syserror("close timestamp source");
}

struct event { int64_t kernel, physical; bool present; const char *status; };

static const char *sequence_status(uint32_t previous, uint32_t next, bool pps)
{
	uint32_t delta = next - previous;
	if (delta == 1) return NULL;
	if (!delta) return "duplicate_event";
	if (!pps) return "gpio_queue_gap";
	return delta <= INT32_MAX ? "pps_sequence_gap" : "sequence_reset";
}

static void decode_pps(struct source *s, const pps_info_t *info, struct event *e)
{
	uint32_t seq = (uint32_t)info->assert_sequence;
	if (!s->have_seq) { s->seq = seq; s->have_seq = true; return; }
	if (seq == s->seq) return;
	e->status = sequence_status(s->seq, seq, true);
	s->seq = seq;
	e->kernel = timespec_ns(info->assert_timestamp, false);
	e->physical = sub(e->kernel, s->offset); e->present = !fatal;
}

/* Snapshot first, including after GPIO polling. A blocking PPS_FETCH waits
 * for a future event, not necessarily the assertion already stored by PPS. */
static struct event next_event(struct source *s, int64_t deadline)
{
	struct event e = {0};
	while (!stopped && !fatal) {
		int64_t before = now(CLOCK_MONOTONIC);
		bool drain = before >= deadline;
		check_clocks();
		if (s->pps) {
			pps_info_t info;
			struct timespec zero = {0};
			pps_parameters(s, false);
			if (fatal) break;
			int r = time_pps_fetch(s->fd, PPS_TSFMT_TSPEC, &info, &zero);
			if (!r) decode_pps(s, &info, &e);
			else if (errno != EINTR && errno != ETIMEDOUT) syserror("PPS snapshot");
			if (!e.present && !fatal && !stopped && !drain) {
				int64_t start = now(CLOCK_MONOTONIC);
				int64_t remain = sub(deadline, start);
				if (remain <= 0) continue;
				if (s->canwait) {
					struct timespec to = { .tv_sec = (time_t)(remain / NS), .tv_nsec = remain % NS };
					r = time_pps_fetch(s->fd, PPS_TSFMT_TSPEC, &info, &to);
					if (!r) decode_pps(s, &info, &e);
					else if (errno != EINTR && errno != ETIMEDOUT) syserror("PPS fetch");
				}
				if (!e.present && !fatal && now(CLOCK_MONOTONIC) - start < NS / 100)
					sleep_until(minimum(add(start, NS / 100), deadline));
			}
			pps_parameters(s, false);
		} else {
			struct pollfd p = { .fd = s->fd, .events = POLLIN };
			int64_t remain = sub(deadline, now(CLOCK_MONOTONIC));
			if (remain <= 0) drain = true;
			int timeout = drain ? 0 : (int)minimum((remain + 999999) / 1000000, INT_MAX);
			int r = poll(&p, 1, timeout);
			if (r < 0) { if (errno != EINTR) syserror("GPIO event poll"); }
			else if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) fail("GPIO event device lost");
			else if (r && (p.revents & POLLIN)) {
				struct gpio_v2_line_event ev;
				ssize_t bytes = read(s->fd, &ev, sizeof ev);
				if (bytes < 0) { if (errno != EINTR) syserror("GPIO event read"); }
				else if (bytes != sizeof ev) fail("short GPIO event read");
				else if (ev.id != GPIO_V2_LINE_EVENT_RISING_EDGE || ev.timestamp_ns > INT64_MAX) fail("invalid GPIO rising event");
				else {
					if (s->have_seq) {
						e.status = sequence_status(s->seq, ev.seqno, false);
						const char *ls = sequence_status(s->line_seq, ev.line_seqno, false);
						if (ls) e.status = ls;
					}
					s->seq = ev.seqno; s->line_seq = ev.line_seqno; s->have_seq = true;
					e.kernel = e.physical = (int64_t)ev.timestamp_ns; e.present = true;
				}
			}
		}
		check_clocks();
		if (e.present || fatal || drain) break;
	}
	if (fatal) e.status = clock_failed ? "clock_step" : "error";
	return e;
}

struct sample {
	struct event event;
	bool polled, bracket_ok, valid;
	int64_t polled_ns;
	double polled_fraction, bracket;
	const char *poll_status;
};

static void poll_window(struct source *src, int64_t center, int64_t half,
			int64_t spacing, int64_t run_end, struct sample *s)
{
	int64_t offset = check_clocks();
	int64_t lo = sub(center, half), hi = add(center, half);
	int64_t begin = sub(lo, offset), end = minimum(sub(hi, offset), run_end);
	int64_t previous = 0, previous_start = 0, read_at = begin;
	double previous_fraction = 0;
	bool have_low = false, began = false;
	s->polled = true; s->poll_status = "no_edge";
	sleep_until(minimum(begin, run_end));
	check_clocks();
	while (!stopped && !fatal) {
		int64_t mono = now(CLOCK_MONOTONIC);
		if (mono >= end) { if (!began) s->poll_status = "late"; break; }
		if (mono < read_at) continue;
		int64_t t0 = now(CLOCK_REALTIME);
		if (t0 < lo) continue;
		began = true;
		if (t0 >= hi) { s->poll_status = "invalid_bracket"; break; }
		int v;
		if (src->level) {
			__sync_synchronize(); v = (*src->level & src->mask) != 0; __sync_synchronize();
		} else {
			struct gpio_v2_line_values values = { .mask = 1 };
			if (ioctl(src->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
				syserror("GPIO value read"); s->poll_status = "io_error"; break;
			}
			v = (values.bits & 1) != 0;
		}
		int64_t t1 = now(CLOCK_REALTIME);
		if (t1 < t0 || (have_low && t0 < previous_start)) {
			clock_failed = true; fail("reversed realtime GPIO read ordering");
			s->poll_status = "invalid_bracket"; break;
		}
		if (t1 > hi || (end == run_end && now(CLOCK_MONOTONIC) > run_end)) {
			s->poll_status = "invalid_bracket"; break;
		}
		int64_t midpoint = add(t0, sub(t1, t0) / 2);
		double fraction = (sub(t1, t0) % 2) * 0.5;
		if (v) {
			if (!have_low) s->poll_status = "initial_high";
			else if (midpoint < previous || (midpoint == previous && fraction <= previous_fraction)) s->poll_status = "invalid_bracket";
			else {
				s->bracket = (double)sub(midpoint, previous) + fraction - previous_fraction;
				double edge_delta = previous_fraction + s->bracket / 2;
				s->polled_ns = add(previous, (int64_t)edge_delta);
				s->polled_fraction = edge_delta - (int64_t)edge_delta;
				s->bracket_ok = true; s->poll_status = NULL;
			}
			break;
		}
		previous = midpoint; previous_fraction = fraction; previous_start = t1; have_low = true;
		read_at = add(mono, spacing);
	}
	if (stopped && !s->bracket_ok) s->poll_status = "interrupted";
	check_clocks();
}

static bool output_ok(void)
{
	if (fflush(stdout) == EOF || ferror(stdout)) { syserror("stdout"); return false; }
	return true;
}

static void timestamp(const char *key, int64_t t)
{
	/* Kernel epoch values are nonnegative on supported measurement dates. */
	printf("%s=%" PRId64 ".%09" PRId64, key, t / NS, t % NS);
}

static void escaped(const char *s)
{
	for (const unsigned char *p = (const unsigned char *)s; *p; p++)
		if (*p <= ' ' || *p == '%' || *p == 127) printf("%%%02X", *p);
		else putchar(*p);
}

static void difference(const char *key, double ns) { printf(" %s=%.1fe-6", key, ns / 1000); }

static const char *interpolate(const struct sample *left, const struct sample *odd,
			      const struct sample *right, double *bias)
{
	if (!left || !left->event.present || !right->event.present) return "missing_neighbor";
	if (!left->valid || !right->valid) return "invalid_neighbor";
	if (!odd->valid) return "invalid_pairing";
	int64_t a = sub(odd->event.physical, left->event.physical);
	int64_t b = sub(right->event.physical, odd->event.physical);
	if (a <= NS / 2 || a >= 3 * NS / 2 || b <= NS / 2 || b >= 3 * NS / 2) return "invalid_pairing";
	*bias = (double)sub(odd->event.kernel, left->polled_ns) - ((double)sub(right->polled_ns, left->polled_ns) + right->polled_fraction + left->polled_fraction) / 2;
	return NULL;
}

static void record(const struct sample *s, const char *unavailable, bool interpolated, double bias)
{
	if (!verbose) return;
	if (s->event.present) timestamp("timestamp", s->event.kernel);
	else printf("sourceStatus=%s", s->event.status);
	if (s->polled && s->valid) difference("bias", ((double)sub(s->event.kernel, s->polled_ns) - s->polled_fraction));
	if (s->bracket_ok) difference("bracket", (double)s->bracket);
	if (s->poll_status) printf(" pollStatus=%s", s->poll_status);
	if (s->event.present && s->event.status) printf(" sourceStatus=%s", s->event.status);
	if (unavailable) printf(" unpolledStatus=%s", unavailable);
	if (interpolated) difference("unpolledBias", bias);
	putchar('\n'); output_ok();
}

static struct event synchronize(struct source *src, int64_t deadline, bool resync)
{
	int64_t offset = check_clocks(), start = now(CLOCK_MONOTONIC);
	while (!fatal && !stopped && now(CLOCK_MONOTONIC) < deadline) {
		struct event e = next_event(src, deadline);
		if (!e.present) break;
		int64_t mono = sub(e.physical, offset);
		if (mono < start || mono >= deadline || e.status) {
			if (verbose) { timestamp("timestamp", e.kernel); printf(" sourceStatus=%s\n", e.status ? e.status : "stale_event"); output_ok(); }
			continue;
		}
		if (resync && verbose) { timestamp("timestamp", e.kernel); printf(" resync=1\n"); output_ok(); }
		return e;
	}
	return (struct event){0};
}
struct stats { double mean, median, sd; size_t n; };

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static struct stats compute(double *v, size_t n)
{
	struct stats s = { .n = n };
	if (!n) return s;
	for (size_t i = 0; i < n; i++) s.mean += v[i] / n;
	for (size_t i = 0; i < n; i++) s.sd += (v[i] - s.mean) * (v[i] - s.mean);
	if (n > 1) s.sd = sqrt(s.sd / (n - 1));
	qsort(v, n, sizeof *v, cmp_double);
	s.median = n % 2 ? v[n / 2] : v[n / 2 - 1] / 2 + v[n / 2] / 2;
	return s;
}

/* Integer options stay integral; timing options are rounded to nanoseconds. */
static bool number(const char *p, uint64_t scale, uint64_t min, uint64_t max, uint64_t *out)
{
	char *end;
	errno = 0;
	if (scale > 1) {
		double value = strtod(p, &end);
		if (errno || end == p || *end || !isfinite(value) ||
		    value < (double)min / scale || value > (double)max / scale) return false;
		*out = (uint64_t)llround(value * scale);
	} else {
		if (*p < '0' || *p > '9') return false;
		unsigned long long value = strtoull(p, &end, 10);
		if (errno || *end || value < min || value > max) return false;
		*out = value;
	}
	return true;
}

static int usage(int status)
{
	FILE *f = status ? stderr : stdout;
	fprintf(f, "usage: ppsbias -c chip -g gpio [-t seconds] [-e] [-v] [-w window_ms] [-s spacing_ms]\n"
		"       ppsbias -m rpi3|rpi4|rpi5 [-p ppsdev] [-g gpio] [-t seconds] [-e] [-v] [-w window_ms] [-s spacing_ms]\n"
		"  -c chip       GPIO v2 realtime rising events and values; requires -g; no -m/-p\n"
		"  -m model      Read-only BCM (rpi3/rpi4) or RP1 (rpi5) registers\n"
		"  -p ppsdev     PPS assert source for the same rising edge; default /dev/pps0; with -m only\n"
		"  -g gpio       Controller GPIO number (not header pin); default 18 with -m\n"
		"  -t seconds    Integer duration after fresh synchronization, 1..86400 (default 10)\n"
		"  -w window_ms  Polling half-window, 0.000001..499.999 ms (default 1)\n"
		"  -s spacing_ms Minimum read-start spacing, 0..499.999 ms (default 0)\n"
		"                -s must be less than twice -w\n"
		"  -e            Poll every pulse; default alternates unpolled/polled slots\n"
		"  -v            Immediate key=value observations and expanded median summary\n"
		"  -h            Help\n"
		"Default stdout: median correction in seconds, e.g. 12.5e-6 (positive = late).\n");
	if (!status && !output_ok()) return 1;
	return status;
}

static void configuration(const struct source *src, const struct model *model,
			  const char *pps, unsigned gpio, bool alternate, int64_t half, int64_t spacing)
{
	cpu_set_t cpus;
	bool affinity = sched_getaffinity(0, sizeof cpus, &cpus) == 0;
	if (!affinity) perror("sched_getaffinity (optional)");
	if (prctl(PR_SET_TIMERSLACK, 1000UL, 0UL, 0UL, 0UL) < 0) perror("PR_SET_TIMERSLACK (optional)");
	long slack = prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL);
	if (slack < 0) perror("PR_GET_TIMERSLACK (optional)");
	if (!verbose) return;
	printf("access=%s device=", model ? model->access : "ioctl"); escaped(src->device);
	printf(" gpio=%u timestampSource=%s mode=%s", gpio, src->pps ? "pps" : "gpio", alternate ? "alternating" : "every-pulse");
	difference("window", half); difference("spacing", spacing);
	printf(" affinity=");
	if (!affinity) printf("unknown");
	else {
		bool first = true;
		for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &cpus)) {
			printf("%s%d", first ? "" : ",", i); first = false;
		}
	}
	if (slack < 0) printf(" timerSlack=unknown"); else difference("timerSlack", slack);
	if (model) {
		printf(" model=%s pps=", model->name); escaped(pps);
		printf(" ppsMode=0x%x ppsCaps=0x%x", src->mode, src->caps);
		difference("assertOffset", src->offset);
		printf(" ppsWait=%s", src->canwait ? "blocking" : "snapshot");
	}
	putchar('\n'); output_ok();
}

static void excluded(int64_t t, const char *key, const char *status)
{
	if (verbose) {
		timestamp("excludedTimestamp", t); printf(" %s=%s\n", key, status); output_ok();
	}
}

int main(int argc, char **argv)
{
	const char *chip = NULL, *model_name = NULL, *pps = NULL;
	const struct model *model = NULL;
	uint64_t gpio = 18, duration = 10, half = 1000000, spacing = 0;
	bool gpio_given = false, alternate = true;
	int opt;
	while ((opt = getopt(argc, argv, "hc:m:p:g:t:w:s:ev")) != -1) {
		uint64_t *dest = NULL, scale = 1, min = 0, max = UINT32_MAX;
		switch (opt) {
		case 'h': return usage(0);
		case 'c': chip = optarg; break;
		case 'm': model_name = optarg; break;
		case 'p': pps = optarg; break;
		case 'e': alternate = false; break;
		case 'v': verbose = true; break;
		case 'g': dest = &gpio; gpio_given = true; break;
		case 't': dest = &duration; min = 1; max = 86400; break;
		case 'w': dest = &half; scale = 1000000; min = 1; max = 499999000; break;
		case 's': dest = &spacing; scale = 1000000; max = 499999000; break;
		default: return usage(2);
		}
		if (dest && !number(optarg, scale, min, max, dest)) {
			fprintf(stderr, "invalid numeric argument: %s\n", optarg); return 2;
		}
	}
	if (model_name) {
		for (size_t i = 0; i < sizeof models / sizeof models[0]; i++)
			if (!strcmp(model_name, models[i].name)) model = &models[i];
		if (!model) { fprintf(stderr, "unknown model: %s\n", model_name); return 2; }
	}
	if (optind != argc || (!!chip == !!model) ||
	    (chip && (pps || !gpio_given || !*chip)) || (model && ((pps && !*pps) || gpio >= model->count)))
		return usage(2);
	if (spacing >= 2 * half) {
		fprintf(stderr, "-s must be less than twice -w\n"); return 2;
	}
	if (model && !pps) pps = "/dev/pps0";
	struct sigaction action = { .sa_handler = on_stop, .sa_flags = SA_RESTART }, ignore = { .sa_handler = SIG_IGN };
	if (sigemptyset(&action.sa_mask) < 0 || sigemptyset(&ignore.sa_mask) < 0 ||
	    sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ||
	    sigaction(SIGPIPE, &ignore, NULL) < 0) { perror("signal setup"); return 1; }
	struct source src = { .fd = -1 };
	size_t cap = (size_t)duration + 4, count = 0, nb = 0;
	double *values = calloc(cap, sizeof *values), *brackets = calloc(cap, sizeof *brackets);
	if (!values || !brackets) syserror("calloc");
	if (!fatal) {
		if (model) { map_registers(&src, model, (unsigned)gpio); if (!fatal) open_pps(&src, pps); }
		else request_line(&src, chip, (unsigned)gpio);
	}
	if (!fatal) configuration(&src, model, pps, (unsigned)gpio, alternate, half, spacing);
	unsigned poll_failures = 0, source_failures = 0, unpolled_unavailable = 0;
	unsigned parity = 1, missing = 0;
	struct sample left = {0}, previous = {0};
	bool have_left = false, have_previous = false, source_stopped = false;
	int64_t run_end = 0, center = 0;
	if (!fatal && !stopped) {
		struct event sync = synchronize(&src, add(now(CLOCK_MONOTONIC), 3 * NS), false);
		if (!sync.present && !fatal && !stopped) fail("no fresh rising assertion within 3 seconds");
		if (sync.present) {
			run_end = add(now(CLOCK_MONOTONIC), (int64_t)duration * NS);
			center = add(sync.physical, NS);
		}
	}
	while (!fatal && !stopped && now(CLOCK_MONOTONIC) < run_end) {
		int64_t offset = check_clocks();
		/* Don't create a slot whose acquisition interval hasn't begun by the end. */
		if (sub(sub(center, NS / 2), offset) >= run_end) { sleep_until(run_end); check_clocks(); break; }
		if (count == cap || nb == cap) {
			if (cap > SIZE_MAX / sizeof *values / 2) { fail("sample storage size overflow"); break; }
			size_t next_cap = cap * 2;
			double *grown = realloc(values, next_cap * sizeof *values);
			if (!grown) { syserror("realloc"); break; }
			values = grown;
			grown = realloc(brackets, next_cap * sizeof *brackets);
			if (!grown) { syserror("realloc"); break; }
			brackets = grown;
			cap = next_cap;
		}
		struct sample s = {0};
		if (!alternate || !(parity % 2)) poll_window(&src, center, half, spacing, run_end, &s);
		int64_t full_end = sub(add(center, NS / 2), offset);
		int64_t slot_end = minimum(full_end, run_end);
		if (!fatal && !stopped) s.event = next_event(&src, slot_end);
		if (stopped) break;
		/* Expiry of a shortened final slot is not a source failure. */
		if (!fatal && !stopped && !s.event.status && slot_end < full_end &&
		    (!s.event.present || sub(s.event.physical, offset) >= run_end))
			break;
		if (fatal) s.event.status = clock_failed ? "clock_step" : "error";
		else if (!s.event.present) s.event.status = "missing_event";
		else if (!s.event.status && (s.event.physical <= sub(center, NS / 2) ||
		         s.event.physical >= add(center, NS / 2) || sub(s.event.physical, offset) >= run_end))
			s.event.status = s.event.physical < sub(center, NS / 2) ? "stale_event" : "timestamp_mismatch";
		s.valid = s.event.present && !s.event.status && (!s.polled || s.bracket_ok);
		if (s.polled && !s.bracket_ok) poll_failures++;
		if (s.bracket_ok) brackets[nb++] = (double)s.bracket;
		bool bad_source = s.event.status != NULL;
		if (bad_source) source_failures++;
		bool interp = false;
		double bias = 0;
		const char *unavailable = NULL;
		if (alternate && have_previous && !previous.polled && previous.event.present) {
			unavailable = interpolate(have_left ? &left : NULL, &previous, &s, &bias);
			interp = unavailable == NULL;
			if (!interp) unpolled_unavailable++;
		}
		size_t before_count = count;
		if (interp) values[count++] = bias;
		if (!alternate && s.valid) values[count++] = ((double)sub(s.event.kernel, s.polled_ns) - s.polled_fraction);
		record(&s, unavailable, interp, bias);
		/* Continue watching the slot after immediate output. This detects extra
		 * events and drains GPIO queues without assigning stale events to a later
		 * slot. Roll back estimates visibly if new evidence invalidates them. */
		bool resync = bad_source && s.event.present;
		if (!bad_source && s.event.present && !stopped && !fatal) {
			struct event extra = next_event(&src, slot_end);
			if (extra.present || fatal) {
				const char *why = fatal ? (clock_failed ? "clock_step" : "error") :
					(extra.status ? extra.status : "multiple_events");
				excluded(s.event.kernel, "sourceStatus", why);
				if (interp) { excluded(previous.event.kernel, "unpolledStatus", "invalid_neighbor"); unpolled_unavailable++; }
				count = before_count; s.valid = false; s.event.status = why;
				source_failures++; resync = true;
			}
		}
		if (!s.event.present && slot_end == full_end && !stopped && !fatal) missing++;
		else if (s.event.present) missing = 0;
		if (!fatal && missing >= 3 && now(CLOCK_MONOTONIC) < run_end) {
			source_stopped = true;
			fail("timestamp source stopped: three consecutive missing slots");
		}
		if (s.event.present && !s.event.status) center = add(s.event.physical, NS);
		else center = add(center, NS);
		left = previous; have_left = have_previous;
		previous = s; have_previous = true; parity++;
		if (resync && !fatal && !stopped && now(CLOCK_MONOTONIC) < run_end) {
			if (!previous.polled && previous.event.present) unpolled_unavailable++;
			have_previous = have_left = false;
			int64_t deadline = minimum(add(now(CLOCK_MONOTONIC), 3 * NS), run_end);
			struct event sync = synchronize(&src, deadline, true);
			if (sync.present) { center = add(sync.physical, NS); parity = 1; missing = 0; }
			else if (!fatal && !stopped && now(CLOCK_MONOTONIC) < run_end) {
				source_stopped = true;
				fail("timestamp source stopped: reacquisition timed out");
			}
		}
	}
	if (alternate && have_previous && !previous.polled && previous.event.present) unpolled_unavailable++;
	bool have_result = count && (!fatal || source_stopped);
	cleanup(&src);
	int result = fatal ? 1 : stopped ? 128 + stopped : count ? 0 : 3;
	if (!fatal && !stopped && !count) fprintf(stderr, "no usable estimates; require matching physical rising GPIO/PPS edges at 1 Hz\n");
	if (have_result) {
		struct stats stats = compute(values, count);
		if (!verbose) printf("%.1fe-6\n", stats.median / 1000);
		else {
			printf("median=%.1fe-6", stats.median / 1000); difference("mean", stats.mean);
			printf(" samples=%zu mode=%s completion=%s", count, alternate ? "alternating" : "every-pulse",
			       fatal ? "failed" : stopped == SIGINT ? "interrupted" : stopped == SIGTERM ? "terminated" : "complete");
			if (count > 1) difference("stddev", stats.sd);
			if (nb) { struct stats b = compute(brackets, nb); difference("bracketMedian", b.median); difference("bracketMax", brackets[nb - 1]); }
			printf(" pollFailures=%u sourceFailures=%u unpolledUnavailable=%u\n", poll_failures, source_failures, unpolled_unavailable);
		}
		if (!output_ok()) result = 1;
	}
	free(values); free(brackets);
	return result;
}
