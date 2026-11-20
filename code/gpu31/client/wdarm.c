/* SPDX-License-Identifier: MIT */
/*
 * wdarm.c - arm the Exynos hardware watchdog before a risky HW test.
 *
 * Usage: wdarm <timeout_s> <cmd...>
 * Opens /dev/watchdog, sets WDIOC_SETTIMEOUT, then execs cmd.
 * 3.10 semantics (watchdog_dev.c): the device stops on close unless
 * NOWAYOUT/MAGICCLOSE; we keep the fd open across exec, and the child
 * inherits it, so the watchdog fires ~timeout_s after the last feed
 * even if the GPU wedges the test process. Explicitly does NOT feed
 * after exec: one kick at arm time only, then the countdown runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

int main(int argc, char **argv)
{
	int fd, to, rc;
	const char *dev = "/dev/watchdog";

	if (argc < 3) {
		fprintf(stderr, "usage: %s <timeout_s> <cmd...>\n", argv[0]);
		return 1;
	}
	to = atoi(argv[1]);
	if (to <= 0 || to > 600) {
		fprintf(stderr, "wdarm: bad timeout '%s'\n", argv[1]);
		return 1;
	}
	fd = open(dev, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "wdarm: open %s: %s\n", dev,
			strerror(errno));
		return 1;
	}
	rc = ioctl(fd, WDIOC_SETTIMEOUT, &to);
	if (rc) {
		fprintf(stderr, "wdarm: SETTIMEOUT %d: %s\n", to,
			strerror(errno));
		close(fd);
		return 1;
	}
	rc = ioctl(fd, WDIOC_GETTIMEOUT, &to);
	if (!rc)
		printf("wdarm: watchdog armed, timeout=%d s\n", to);
	fflush(stdout);
	/* Keep fd open across exec (no CLOEXEC): the child inherits it
	 * and the countdown fires even if the GPU wedges everything. */
	execvp(argv[2], argv + 2);
	fprintf(stderr, "wdarm: exec %s: %s\n", argv[2], strerror(errno));
	close(fd);
	return 1;
}
