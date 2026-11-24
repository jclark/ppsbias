/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* Compare physical 1 Hz rising GPIO edges with kernel timestamps. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/gpio.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
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
#include "families/family.h"

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
/* Realtime and monotonic receive the same frequency adjustments on Linux.
 * Their offset must stay within the intersection of the measured intervals.
 * Retain that intersection so a slow read cannot conceal a clock step between
 * two precise reads. Allow one nanosecond for clock representation rounding. */
struct clocks { int64_t low, high; };
static struct clocks clocks;
static bool have_clocks;

static int64_t check_clocks(void)
{
	int64_t m0 = now(CLOCK_MONOTONIC), r = now(CLOCK_REALTIME), m1 = now(CLOCK_MONOTONIC);
	struct clocks c = { sub(r, m1), sub(r, m0) };
	if (m1 < m0 || (have_clocks &&
	    (sub(c.low, clocks.high) > 1 || sub(clocks.low, c.high) > 1))) {
		clock_failed = true; fail("clock discontinuity detected");
	}
	if (!have_clocks) clocks = c;
	else {
		if (c.low > clocks.low) clocks.low = c.low;
		if (c.high < clocks.high) clocks.high = c.high;
		if (clocks.high < clocks.low) clocks.high = clocks.low;
	}
	have_clocks = true;
	return add(c.low, sub(c.high, c.low) / 2);
}

/* The supported GPIO controller families, each described in its own file under families/. */
static const struct family *const families[] = { &bcm2835, &rp1, &rockchip, &k3 };

/* The GPIO a pps-gpio device uses. */
struct controller {
	const struct family *family;
	char node[PATH_MAX]; /* filesystem path of the controller's device-tree node */
	size_t root_len;     /* length of the device-tree root's path, a prefix of node */
	unsigned line;
};
static const char *dt_root = "/proc/device-tree", *sys_root = "/sys";

/* snprintf that reports truncation as failure. */
static bool format(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool format(char *buf, size_t cap, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, cap, fmt, ap);
	va_end(ap);
	return n >= 0 && (size_t)n < cap;
}

/* Read a whole small file with stdio; returns its length, or -1. */
static long read_file(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	size_t n = fread(buf, 1, cap, f);
	bool bad = ferror(f) || (n == cap && fgetc(f) != EOF);
	fclose(f);
	return bad ? -1 : (long)n;
}

static uint32_t be32(const char *p)
{
	const unsigned char *u = (const unsigned char *)p;
	return (uint32_t)u[0] << 24 | (uint32_t)u[1] << 16 | (uint32_t)u[2] << 8 | u[3];
}

/* A device-tree property's big-endian cells: returns how many, 0 for an empty property,
 * or -1 if it is absent, unreadable, longer than max cells or not whole cells. */
static long property_cells(const char *node, const char *name, uint32_t *cells, size_t max)
{
	static char buf[16384];
	char path[PATH_MAX];
	if (max > sizeof buf / 4) max = sizeof buf / 4;
	if (!format(path, sizeof path, "%s/%s", node, name)) return -1;
	long n = read_file(path, buf, max * 4);
	if (n < 0 || n % 4) return -1;
	for (long i = 0; i < n / 4; i++) cells[i] = be32(buf + 4 * i);
	return n / 4;
}

/* Move node to its parent; false at root, the device tree's root directory. */
static bool up(char *node, const char *root)
{
	char *slash = strrchr(node, '/');
	if (!strcmp(node, root) || !slash || (size_t)(slash - node) < strlen(root)) return false;
	*slash = 0;
	return true;
}

/* #address-cells or #size-cells for a node's children: from the node or, as Linux allows,
 * the nearest ancestor that has it; otherwise 1, the Linux default. */
static unsigned bus_cells(const char *node, const char *root, const char *name)
{
	char dir[PATH_MAX];
	uint32_t c;
	if (!format(dir, sizeof dir, "%s", node)) return 0;
	do if (property_cells(dir, name, &c, 1) == 1) return c;
	while (up(dir, root));
	return 1;
}

/* An address of na cells. Three cells are a PCI address: *space is the first cell, whose
 * space code says which address space the other two are in. */
static bool address(const uint32_t *c, unsigned na, uint32_t *space, uint64_t *value)
{
	if (na < 1 || na > 3) return false;
	*space = na == 3 ? c[0] : 0;
	*value = na == 1 ? c[0] : (uint64_t)c[na - 2] << 32 | c[na - 1];
	return true;
}

static bool length(const uint32_t *c, unsigned ns, uint64_t *value)
{
	if (ns < 1 || ns > 2) return false;
	*value = ns == 1 ? c[0] : (uint64_t)c[0] << 32 | c[1];
	return true;
}

/* The CPU physical address and size of a node's first register range: its reg translated
 * through the ranges of each parent bus, as the Devicetree Specification defines. Returns
 * NULL, or why not. On a bus whose children have three address cells, taken to be PCI, a
 * range matches on the space code as well as the address, as in Linux. */
static const char *translate(const char *node, const char *root, uint64_t *base, uint64_t *size)
{
	static uint32_t c[4096];
	char bus[PATH_MAX], parent[PATH_MAX];
	if (!format(bus, sizeof bus, "%s", node) || !up(bus, root)) return "GPIO controller is not in the device tree";
	unsigned na = bus_cells(bus, root, "#address-cells"), ns = bus_cells(bus, root, "#size-cells");
	uint32_t space;
	long n = property_cells(node, "reg", c, sizeof c / sizeof c[0]);
	if (n < (long)(na + ns) || !address(c, na, &space, base) || !length(c + na, ns, size))
		return "cannot read the GPIO controller's reg property";
	while (strcmp(bus, root)) {
		if (!format(parent, sizeof parent, "%s", bus) || !up(parent, root)) return "device-tree path too long";
		unsigned pna = bus_cells(parent, root, "#address-cells"), entry = na + pna + ns;
		n = property_cells(bus, "ranges", c, sizeof c / sizeof c[0]);
		if (n < 0) return "a bus above the GPIO controller has no ranges property, so its address cannot be translated";
		if (n == 0) { if (pna != 3) space = 0; } /* empty ranges: the same addresses */
		else {
			bool found = false;
			for (long i = 0; !found && i + (long)entry <= n; i += entry) {
				uint32_t child_space, parent_space;
				uint64_t child, to, len;
				if (!address(c + i, na, &child_space, &child) || !address(c + i + na, pna, &parent_space, &to) ||
				    !length(c + i + na + pna, ns, &len))
					return "cannot parse the ranges of a bus above the GPIO controller";
				if ((child_space ^ space) & (na == 3 ? UINT32_C(0x03000000) : UINT32_MAX)) continue;
				if (*base < child || *base - child >= len) continue;
				if (__builtin_add_overflow(to, *base - child, base)) return "GPIO controller address overflows";
				space = parent_space; found = true;
			}
			if (!found) return "GPIO controller address is outside the ranges of a bus above it";
		}
		strcpy(bus, parent);
		na = pna; ns = bus_cells(bus, root, "#size-cells");
	}
	return NULL;
}

/* The gpios property of the pps-gpio device behind a /sys/class/pps entry: ppsN name ->
 * platform device -> its device-tree node. pps-gpio names itself "<device>.<id>". */
static const char *pps_gpios(const char *entry, uint32_t gpios[3])
{
	char path[PATH_MAX], name[128];
	long n = -1;
	if (format(path, sizeof path, "%s/class/pps/%s/name", sys_root, entry))
		n = read_file(path, name, sizeof name - 1);
	if (n <= 0) return "cannot read the PPS device name";
	name[n] = 0;
	name[strcspn(name, "\n")] = 0;
	char *dot = strrchr(name, '.');
	if (dot) *dot = 0;
	if (!format(path, sizeof path, "%s/bus/platform/devices/%s/of_node", sys_root, name) ||
	    property_cells(path, "gpios", gpios, 3) != 3)
		return "PPS device has no device-tree GPIO";
	return NULL;
}

static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }

/* The one PPS device that has a device-tree GPIO, as /dev/ppsN; reports on stderr if there is not one. */
static bool default_pps(char *path, size_t cap)
{
	char dir[PATH_MAX], names[16][NAME_MAX + 1];
	size_t count = 0;
	DIR *d = format(dir, sizeof dir, "%s/class/pps", sys_root) ? opendir(dir) : NULL;
	struct dirent *e;
	while (d && (e = readdir(d))) {
		uint32_t gpios[3];
		if (e->d_name[0] == '.' || pps_gpios(e->d_name, gpios)) continue;
		if (count < sizeof names / sizeof names[0]) strcpy(names[count], e->d_name);
		count++;
	}
	if (d) closedir(d);
	if (count == 1) {
		if (format(path, cap, "/dev/%s", names[0])) return true;
		fprintf(stderr, "PPS device name too long\n");
		return false;
	}
	if (!count) { fprintf(stderr, "no GPIO PPS device found\n"); return false; }
	size_t listed = count < sizeof names / sizeof names[0] ? count : sizeof names / sizeof names[0];
	qsort(names, listed, sizeof names[0], cmp_name);
	fprintf(stderr, "more than one GPIO PPS device:");
	for (size_t i = 0; i < listed; i++) fprintf(stderr, " /dev/%s", names[i]);
	fprintf(stderr, "%s\nuse -p to choose one\n", listed < count ? " ..." : "");
	return false;
}

/* Find the GPIO controller and line that a PPS device uses: the controller is the gpiochip
 * whose device-tree node has the phandle in the PPS device's gpios property, and its family
 * comes from the node's compatible strings. Returns NULL, or why not. */
static const char *locate(const char *pps, struct controller *ctl)
{
	static char why[512];
	char real[PATH_MAX], root[PATH_MAX], dir[PATH_MAX], link[PATH_MAX], path[PATH_MAX], compatible[256];
	const char *dev = realpath(pps, real) ? real : pps;
	const char *entry = strrchr(dev, '/') ? strrchr(dev, '/') + 1 : dev;
	uint32_t gpios[3];
	const char *error = pps_gpios(entry, gpios);
	if (error) return error;
	if (gpios[2] & 1) return "PPS GPIO is active low";
	if (!realpath(dt_root, root)) return "cannot read the device tree";
	DIR *d = format(dir, sizeof dir, "%s/bus/gpio/devices", sys_root) ? opendir(dir) : NULL;
	if (!d) return "cannot read the GPIO controllers";
	bool found = false;
	struct dirent *e;
	while (!found && (e = readdir(d))) {
		uint32_t phandle;
		found = !strncmp(e->d_name, "gpiochip", 8) && format(link, sizeof link, "%s/%s/of_node", dir, e->d_name) &&
			property_cells(link, "phandle", &phandle, 1) == 1 && phandle == gpios[0] && realpath(link, ctl->node);
	}
	closedir(d);
	if (!found) return "cannot find the GPIO controller of the PPS GPIO";
	ctl->root_len = strlen(root);
	if (strncmp(ctl->node, root, ctl->root_len) || ctl->node[ctl->root_len] != '/')
		return "PPS GPIO controller is outside the device tree";
	long n = format(path, sizeof path, "%s/compatible", ctl->node) ? read_file(path, compatible, sizeof compatible - 1) : -1;
	if (n <= 0) return "cannot read the PPS GPIO controller's compatible property";
	compatible[n] = 0;
	ctl->family = NULL;
	uint32_t count = 0;
	for (size_t f = 0; !ctl->family && f < sizeof families / sizeof families[0]; f++)
		for (const struct compatible *c = families[f]->compatible; !ctl->family && c->name; c++)
			for (long i = 0; !ctl->family && i < n; i += (long)strlen(compatible + i) + 1)
				if (!strcmp(compatible + i, c->name)) { ctl->family = families[f]; count = c->lines; }
	if (!ctl->family) {
		for (long i = 0; i < n - 1; i++) if (!compatible[i]) compatible[i] = ' ';
		format(why, sizeof why, "PPS GPIO controller %s is not supported (compatible: %s)",
		       ctl->node + ctl->root_len, compatible);
		return why;
	}
	if (!count && (!ctl->family->lines_property ||
	    property_cells(ctl->node, ctl->family->lines_property, &count, 1) != 1)) {
		format(why, sizeof why, "cannot read the PPS GPIO controller's %s property",
		       ctl->family->lines_property ? ctl->family->lines_property : "line count");
		return why;
	}
	if (gpios[1] >= count) return "PPS GPIO is outside the controller range";
	ctl->line = gpios[1];
	return NULL;
}

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
	uint64_t physical; /* address mapped through /dev/mem */
};

static void map_registers(struct source *s, const struct controller *c)
{
	const struct family *f = c->family;
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0) { fail("cannot determine page size"); return; }
	uint64_t physical = 0, span = f->window;
	int fd = -1;
	if (f->device) {
		s->device = f->device;
		fd = open(s->device, O_RDONLY | O_SYNC | O_CLOEXEC);
	}
	if (!f->device || (fd < 0 && errno == ENOENT)) {
		char root[PATH_MAX];
		uint64_t size;
		const char *why = realpath(dt_root, root) ? translate(c->node, root, &physical, &size) : "cannot read the device tree";
		if (why) { fail(why); return; }
		if (!f->device) span = size;
		s->device = "/dev/mem"; s->physical = physical;
		fd = open(s->device, O_RDONLY | O_SYNC | O_CLOEXEC);
	}
	if (fd < 0) { syserror(s->device); return; }
	size_t level = f->level(c->line, &s->mask);
	if (level + 4 > span || f->extent > span) {
		fail("GPIO controller registers are smaller than expected");
		close(fd); return;
	}
	size_t displacement = physical % (uint64_t)page;
	off_t offset = (off_t)(physical - displacement);
	s->map_length = ((span + displacement + page - 1) / page) * page;
	s->mapping = mmap(NULL, s->map_length, PROT_READ, MAP_SHARED, fd, offset);
	if (s->mapping == MAP_FAILED) { s->mapping = NULL; syserror(s->device); }
	if (close(fd) < 0) syserror("close memory device");
	if (!s->mapping) return;
	const volatile char *regs = (const volatile char *)s->mapping + displacement;
	s->level = (volatile uint32_t *)(regs + level);
	const char *why = f->check ? f->check(regs) : NULL;
	if (why) fail(why);
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

/* Seconds, fixed point to the nanosecond, as for timestamp(). */
static void seconds(const char *key, double ns) { printf(" %s=%.9f", key, ns / NS); }

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

static void record(const struct sample *s, const char *unavailable, bool interpolated,
		   double bias, int64_t unpolled_timestamp)
{
	if (!verbose) return;
	if (s->event.present) timestamp("timestamp", s->event.kernel);
	else printf("sourceStatus=%s", s->event.status);
	if (s->polled && s->valid) seconds("bias", ((double)sub(s->event.kernel, s->polled_ns) - s->polled_fraction));
	if (s->bracket_ok) seconds("bracket", (double)s->bracket);
	if (s->poll_status) printf(" pollStatus=%s", s->poll_status);
	if (s->event.present && s->event.status) printf(" sourceStatus=%s", s->event.status);
	if (unavailable || interpolated) {
		putchar(' '); timestamp("unpolledTimestamp", unpolled_timestamp);
	}
	if (unavailable) printf(" unpolledStatus=%s", unavailable);
	if (interpolated) seconds("unpolledBias", bias);
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
	fprintf(f, "usage: ppsbias [-p ppsdev] [-t seconds] [-e] [-v] [-w window_ms] [-s spacing_ms]\n"
		"       ppsbias -c chip -g gpio [-t seconds] [-e] [-v] [-w window_ms] [-s spacing_ms]\n"
		"  -p ppsdev     pps-gpio device for assert timestamps; its GPIO's level is read from\n"
		"                the controller's registers; default: the only pps-gpio device\n"
		"  -c chip       GPIO v2 realtime rising events and values instead; requires -g; no -p\n"
		"  -g gpio       Line on chip (not header pin); with -c only\n"
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

static void configuration(const struct source *src, const struct controller *ctl, const char *pps,
			  unsigned gpio, bool alternate, int64_t half, int64_t spacing)
{
	cpu_set_t cpus;
	bool affinity = sched_getaffinity(0, sizeof cpus, &cpus) == 0;
	if (!affinity) perror("sched_getaffinity (optional)");
	if (prctl(PR_SET_TIMERSLACK, 1000UL, 0UL, 0UL, 0UL) < 0) perror("PR_SET_TIMERSLACK (optional)");
	long slack = prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL);
	if (slack < 0) perror("PR_GET_TIMERSLACK (optional)");
	if (!verbose) return;
	printf("access=%s device=", ctl ? ctl->family->name : "ioctl"); escaped(src->device);
	if (src->mapping && !strcmp(src->device, "/dev/mem")) printf(" address=0x%" PRIx64, src->physical);
	if (ctl) { printf(" controller="); escaped(ctl->node + ctl->root_len); }
	printf(" gpio=%u", gpio);
	printf(" timestampSource=%s mode=%s", src->pps ? "pps" : "gpio", alternate ? "alternating" : "every-pulse");
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
	if (ctl) {
		printf(" pps="); escaped(pps);
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
	const char *chip = NULL, *pps = NULL;
	uint64_t gpio = 0, duration = 10, half = 1000000, spacing = 0;
	bool gpio_given = false, model_given = false, alternate = true;
	int opt;
	while ((opt = getopt(argc, argv, "hc:m:p:g:t:w:s:ev")) != -1) {
		uint64_t *dest = NULL, scale = 1, min = 0, max = UINT32_MAX;
		switch (opt) {
		case 'h': return usage(0);
		case 'c': chip = optarg; break;
		case 'm': model_given = true; break; /* obsolete: the device tree identifies the controller */
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
	if (optind != argc || (chip && (pps || !gpio_given || !*chip)) || (!chip && (gpio_given || (pps && !*pps))))
		return usage(2);
	if (spacing >= 2 * half) {
		fprintf(stderr, "-s must be less than twice -w\n"); return 2;
	}
	if (model_given) fprintf(stderr, "warning: -m is no longer needed and is ignored\n");
	static char default_path[PATH_MAX];
	static struct controller ctl;
	if (!chip) {
		if (!pps) {
			if (!default_pps(default_path, sizeof default_path)) return 1;
			pps = default_path;
		}
		const char *why = locate(pps, &ctl);
		if (why) { fprintf(stderr, "%s: %s\n", pps, why); return 1; }
		gpio = ctl.line;
	}
	struct sigaction action = { .sa_handler = on_stop, .sa_flags = SA_RESTART }, ignore = { .sa_handler = SIG_IGN };
	if (sigemptyset(&action.sa_mask) < 0 || sigemptyset(&ignore.sa_mask) < 0 ||
	    sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ||
	    sigaction(SIGPIPE, &ignore, NULL) < 0) { perror("signal setup"); return 1; }
	struct source src = { .fd = -1 };
	size_t cap = (size_t)duration + 4, count = 0, nb = 0;
	double *values = calloc(cap, sizeof *values), *brackets = calloc(cap, sizeof *brackets);
	if (!values || !brackets) syserror("calloc");
	if (!fatal) {
		if (!chip) { map_registers(&src, &ctl); if (!fatal) open_pps(&src, pps); }
		else request_line(&src, chip, (unsigned)gpio);
	}
	if (!fatal) configuration(&src, chip ? NULL : &ctl, pps, (unsigned)gpio, alternate, half, spacing);
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
		record(&s, unavailable, interp, bias, previous.event.kernel);
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
		struct stats b = compute(brackets, nb);
		if (nb && b.median > 1500)
			fprintf(stderr, "warning: median polling bracket is %.3f us (above 1.5 us); bias estimate may be inaccurate\n"
				"  try taskset -c to run on a CPU other than the PPS interrupt CPU\n", b.median / 1000);
		if (!verbose) printf("%.1fe-6\n", stats.median / 1000);
		else {
			printf("median=%.1fe-6", stats.median / 1000); difference("mean", stats.mean);
			printf(" samples=%zu mode=%s completion=%s", count, alternate ? "alternating" : "every-pulse",
			       fatal ? "failed" : stopped == SIGINT ? "interrupted" : stopped == SIGTERM ? "terminated" : "complete");
			if (count > 1) difference("stddev", stats.sd);
			if (nb) { difference("bracketMedian", b.median); difference("bracketMax", brackets[nb - 1]); }
			printf(" pollFailures=%u sourceFailures=%u unpolledUnavailable=%u\n", poll_failures, source_failures, unpolled_unavailable);
		}
		if (!output_ok()) result = 1;
	}
	free(values); free(brackets);
	return result;
}
