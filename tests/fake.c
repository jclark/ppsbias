/* Exercise the real collection loop with synthetic clocks and GPIO/PPS APIs.
 * Blocking PPS_FETCH deliberately waits for a future event (Linux semantics). */
#define main ppsbias_main
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define poll fake_poll
#define read fake_read
#define clock_gettime fake_clock_gettime
#define clock_nanosleep fake_clock_nanosleep
#define sched_getaffinity fake_sched_getaffinity
#define prctl fake_prctl
#define fflush fake_fflush
#define mmap fake_mmap
#define munmap fake_munmap
#include "ppsbias.c"
#undef main
#include <assert.h>
#include <stdarg.h>
static int64_t tick = 10 * NS, next_edge = 11 * NS, step;
static const int64_t epoch = 1000 * NS;
static const char *scenario;
static uint32_t seq;
static int64_t last_assert = 9 * NS;
static unsigned char registers[0x40000];
static bool extra_done, wide_done;
static int last_idx;
static bool is(const char *s) { return scenario && !strcmp(scenario, s); }
static void advance(int64_t t)
{
	if (t > tick) tick = t;
	if (tick >= 14250000000LL) {
		if (is("sigint")) on_stop(SIGINT);
		if (is("sigterm")) on_stop(SIGTERM);
		if (is("step")) step = 1000000;
		if (is("small_step")) step = 50000;
		if (is("small_step_back")) step = -50000;
	}
	if (tick >= 10200000000LL && is("early_signal")) on_stop(SIGINT);
	uint32_t value = tick % NS < NS / 2 ? 1u << 18 : 0;
	memcpy(registers + 0x34, &value, 4);
	memcpy(registers + 0x10008, &value, 4);
}
static int64_t bias_for(int64_t edge)
{
	int idx = (int)(edge / NS - 11);
	return idx % 2 ? 10000 : 1000;
}
static void pps_update(void)
{
	while (next_edge + bias_for(next_edge) <= tick) {
		int idx = (int)(next_edge / NS - 11);
		if (!(is("missing") && idx == 4) && !(is("stopped") && idx >= 3)) {
			last_assert = next_edge;
			seq++;
			if (idx == 4 && is("gap")) seq++;
		}
		next_edge += NS;
	}
}
int fake_open(const char *path, int flags, ...)
{
	if (strstr(path, "gpiomem")) {
		assert((flags & O_ACCMODE) == O_RDONLY);
		if (is("fallback")) { errno = ENOENT; return -1; }
		if (is("denied")) { errno = EACCES; return -1; }
		return 102;
	}
	if (!strcmp(path, "/dev/mem")) { assert(is("fallback")); return 102; }
	if (strstr(path, "pps")) { assert((flags & O_ACCMODE) == O_RDONLY); return 103; }
	return 100;
}
int fake_fflush(FILE *f)
{
	(void)f;
	if (is("slow_output") && tick > 15 * NS && tick < 15100000000LL)
		advance(15700000000LL);
	return 0;
}
int fake_close(int fd)
{
	assert(fd >= 100 && fd <= 103);
	if (is("cleanup") && (fd == 101 || fd == 103)) { errno = EIO; return -1; }
	return 0;
}
void *fake_mmap(void *a, size_t len, int prot, int flags, int fd, off_t off)
{
	(void)a; assert(len <= sizeof registers && prot == PROT_READ && flags == MAP_SHARED && fd == 102);
	assert(off == (is("fallback") ? (off_t)0x1f000d0000 : 0));
	return registers;
}
int fake_munmap(void *a, size_t len) { assert(a == registers && len > 0); return 0; }
int fake_ioctl(int fd, unsigned long request, ...)
{
	va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
	if (request == GPIO_GET_CHIPINFO_IOCTL) { ((struct gpiochip_info *)arg)->lines = 54; }
	else if (request == GPIO_V2_GET_LINE_IOCTL) {
		assert(fd == 100);
		struct gpio_v2_line_request *r = arg;
		assert(r->offsets[0] == 18 && r->num_lines == 1);
		assert(!(r->config.flags & GPIO_V2_LINE_FLAG_ACTIVE_LOW));
		if (is("busy")) { errno = EBUSY; return -1; }
		r->fd = 101;
	} else if (request == GPIO_V2_LINE_GET_VALUES_IOCTL) {
		assert(fd == 101);
		if (is("io") && tick >= 14 * NS) { errno = EIO; return -1; }
		if (is("wide") && !wide_done && tick >= 14999900000LL) {
			advance(tick + 200000); wide_done = true;
		}
		if (is("late_read") && tick >= 14999900000LL) advance(tick + NS / 100);
		((struct gpio_v2_line_values *)arg)->bits = is("high") ? 1 : tick % NS < NS / 2;
	} else if (request == PPS_GETCAP) {
		*(int *)arg = PPS_CAPTUREASSERT | PPS_TSFMT_TSPEC | (is("snapshot") ? 0 : PPS_CANWAIT);
	} else if (request == PPS_GETPARAMS) {
		struct pps_kparams *p = arg; memset(p, 0, sizeof *p);
		p->api_version = PPS_API_VERS; p->mode = PPS_CAPTUREASSERT | PPS_TSFMT_TSPEC;
		if (is("disabled")) p->mode &= ~PPS_CAPTUREASSERT;
		if (is("fresh") || (is("format_default") && tick < 14 * NS)) p->mode &= ~PPS_TSFMT_TSPEC;
		if (is("format_bad")) p->mode = PPS_CAPTUREASSERT | PPS_TSFMT_NTPFP;
		if (is("offset") || (is("params") && tick >= 14 * NS)) {
			p->mode |= PPS_OFFSETASSERT; p->assert_off_tu.nsec = 2000000;
		}
	} else if (request == PPS_FETCH) {
		struct pps_fdata *f = arg;
		int64_t wait = f->timeout.sec * NS + f->timeout.nsec;
		assert(wait >= 0 && wait <= 3 * NS);
		assert(f->timeout.nsec >= 0 && f->timeout.nsec < NS);
		if (is("fetch_io") && tick >= 14 * NS) { errno = EIO; return -1; }
		pps_update();
		if (wait) {
			int64_t wake = next_edge + bias_for(next_edge);
			if (wake > tick + wait) { advance(tick + wait); errno = ETIMEDOUT; return -1; }
			advance(wake); pps_update();
		}
		memset(&f->info, 0, sizeof f->info);
		f->info.assert_sequence = seq;
		int64_t t = epoch + last_assert + bias_for(last_assert) +
			(last_assert >= 15 * NS ? step : 0) + (is("offset") ? 2000000 : 0);
		f->info.assert_tu.sec = t / NS; f->info.assert_tu.nsec = t % NS;
		f->info.current_mode = PPS_CAPTUREASSERT | PPS_TSFMT_TSPEC;
	} else { assert(!"unexpected ioctl (including PPS writes)"); }
	return 0;
}
int fake_poll(struct pollfd *fds, nfds_t n, int timeout)
{
	assert(n == 1 && timeout >= 0 && timeout <= 3000);
	fds[0].revents = 0;
	int idx = (int)(next_edge / NS - 11);
	if ((is("missing") && idx == 4) || (is("stopped") && idx >= 3)) {
		if (tick + (int64_t)timeout * 1000000 >= next_edge + NS / 2) next_edge += NS;
		advance(tick + (int64_t)timeout * 1000000); return 0;
	}
	int64_t ready = next_edge + bias_for(next_edge);
	if ((is("extra") || is("slow_output")) && !extra_done && next_edge == 16 * NS) ready = 15100000000LL;
	if (ready > tick + (int64_t)timeout * 1000000) {
		advance(tick + (int64_t)timeout * 1000000); return 0;
	}
	advance(ready); fds[0].revents = POLLIN; return 1;
}
ssize_t fake_read(int fd, void *buf, size_t count)
{
	struct gpio_v2_line_event *e = buf;
	assert(fd == 101 && count == sizeof *e); memset(e, 0, sizeof *e);
	e->id = GPIO_V2_LINE_EVENT_RISING_EDGE;
	last_idx = (int)(next_edge / NS - 11);
	e->timestamp_ns = epoch + next_edge + bias_for(next_edge) + step;
	if ((is("extra") || is("slow_output")) && !extra_done && next_edge == 16 * NS) {
		e->timestamp_ns = epoch + 15100000000LL; extra_done = true;
	} else next_edge += NS;
	seq++;
	if (is("gap") && last_idx == 4) seq++;
	e->seqno = e->line_seqno = seq;
	return sizeof *e;
}
int fake_clock_gettime(clockid_t id, struct timespec *ts)
{
	assert(id == CLOCK_REALTIME || id == CLOCK_MONOTONIC);
	int64_t t = tick + (id == CLOCK_REALTIME ? epoch + step : 0);
	ts->tv_sec = t / NS; ts->tv_nsec = t % NS; advance(tick + 100); return 0;
}
int fake_clock_nanosleep(clockid_t id, int flags, const struct timespec *ts, struct timespec *remaining)
{
	assert(id == CLOCK_MONOTONIC && flags == TIMER_ABSTIME); (void)remaining;
	advance((int64_t)ts->tv_sec * NS + ts->tv_nsec); return 0;
}
int fake_sched_getaffinity(pid_t pid, size_t size, cpu_set_t *cpus)
{
	(void)pid; assert(size == sizeof *cpus); CPU_ZERO(cpus); CPU_SET(1, cpus); CPU_SET(3, cpus); return 0;
}
int fake_prctl(int option, ...)
{
	assert(option == PR_SET_TIMERSLACK || option == PR_GET_TIMERSLACK);
	return option == PR_GET_TIMERSLACK ? 1000 : 0;
}
int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	scenario = getenv("PPSBIAS_TEST");
	return ppsbias_main(argc, argv);
}
