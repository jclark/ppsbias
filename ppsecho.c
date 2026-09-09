/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* Enable PPS echo-on-assert until interrupted. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/timepps.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;

static void on_stop(int sig)
{
	(void)sig;
	stopped = 1;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "";
	pps_handle_t handle;
	pps_params_t params;
	struct sigaction action = { .sa_handler = on_stop };
	sigset_t blocked, previous;
	int fd, caps, enabled = 0, result = 1;

	if (argc == 2 && !strcmp(path, "-h")) {
		printf("usage: ppsecho /dev/ppsN\nEnable assert echo until SIGINT or SIGTERM, then restore its prior setting.\n");
		if (fflush(stdout) == EOF || ferror(stdout)) { perror("stdout"); return 1; }
		return 0;
	}
	if (argc != 2 || !*path || path[0] == '-') {
		fprintf(stderr, "usage: ppsecho /dev/ppsN\n");
		return 2;
	}
	/* Block stop signals across setup and cleanup; sigsuspend avoids a race. */
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGTERM);
	sigemptyset(&action.sa_mask);
	if (sigprocmask(SIG_BLOCK, &blocked, &previous) < 0 ||
	    sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0 ||
	    sigaction(SIGPIPE, &(struct sigaction){ .sa_handler = SIG_IGN }, NULL) < 0) {
		perror("signal setup");
		return 1;
	}
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (time_pps_create(fd, &handle) < 0 ||
	    time_pps_getcap(handle, &caps) < 0 ||
	    time_pps_getparams(handle, &params) < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		goto done;
	}
	if (!(caps & PPS_ECHOASSERT)) {
		fprintf(stderr, "%s: no ECHOASSERT capability\n", path);
		goto done;
	}
	if (!(params.mode & PPS_ECHOASSERT)) {
		params.mode |= PPS_ECHOASSERT;
		if (time_pps_setparams(handle, &params) < 0) {
			fprintf(stderr, "setparams %s: %s\n", path, strerror(errno));
			goto done;
		}
		enabled = 1;
	}
	printf("echo enabled on %s (mode 0x%x) - Ctrl-C to stop\n", path, params.mode);
	if (fflush(stdout) == EOF || ferror(stdout)) {
		perror("stdout");
		goto restore;
	}
	sigdelset(&previous, SIGINT);
	sigdelset(&previous, SIGTERM);
	while (!stopped) {
		if (sigsuspend(&previous) < 0 && errno != EINTR) {
			perror("sigsuspend"); goto restore;
		}
	}
	result = 0;
restore:
	/* Exclusive parameter control is required; this read/modify/write is not atomic. */
	if (enabled) {
		if (time_pps_getparams(handle, &params) < 0) {
			perror("getparams during echo cleanup");
			result = 1;
			goto done;
		}
		params.mode &= ~PPS_ECHOASSERT;
		if (time_pps_setparams(handle, &params) < 0) {
			perror("restore echo setting");
			result = 1;
			goto done;
		}
	}
done:
	if (close(fd) < 0) { perror("close PPS device"); result = 1; }
	return result;
}
