/* Check discontinuity detection without changing the host clocks. */
#define clock_gettime scripted_clock_gettime
#define main ppsbias_main
#include "ppsbias.c"
#undef main
#include <assert.h>

static int index_in_read;
static int64_t scripted[3];
int scripted_clock_gettime(clockid_t id, struct timespec *t)
{
	assert(id == (index_in_read == 1 ? CLOCK_REALTIME : CLOCK_MONOTONIC));
	int64_t n = scripted[index_in_read++];
	t->tv_sec = n / NS; t->tv_nsec = n % NS;
	return 0;
}
static void check(int64_t m0, int64_t r, int64_t m1)
{
	scripted[0] = m0; scripted[1] = 1000 * NS + r; scripted[2] = m1;
	index_in_read = 0;
	check_clocks();
	assert(index_in_read == 3);
}
static void reset(void) { have_clocks = fatal = clock_failed = false; }
int main(void)
{
	/* Ordinary read delay is uncertainty, not a discontinuity. */
	reset();
	check(1000, 1001, 1002);
	check(2000, 2050, 2200);
	check(3000, 3001, 3002);
	assert(!fatal);
	/* A broad intermediate read must not erase the earlier precise bound. */
	for (int direction = -1; direction <= 1; direction += 2) {
		reset();
		check(1000, 1001, 1002);
		check(2000, 2050, 2200);
		check(3000, 3001 + direction * 10, 3002);
		assert(fatal && clock_failed);
	}
	/* Frequency slewing shared by realtime and monotonic is harmless. */
	reset();
	for (int i = 0; i < 100; i++) {
		int64_t t = NS + i * INT64_C(1000000037);
		check(t, t + 1, t + 2);
	}
	assert(!fatal);
	return 0;
}
