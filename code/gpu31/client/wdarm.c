/* SPDX-License-Identifier: MIT */
/*
 * wdarm.c - arm the Exynos hardware watchdog before a risky HW test.
 *
 * Usage: wdarm <timeout_s> <cmd...>
 * Opens /dev/watchdog, sets WDIOC_SETTIMEOUT, forks: the child execs
 * cmd (inheriting the watchdog fd), the parent waitpids. The countdown
 * runs from arming with no further feed, so if the kernel wedges, the
 * SoC resets ~timeout_s later with no human needed.
 *
 * LIVE-SOC FACT (Main, 2026-11-20): this driver does NOT disarm on
 * close — a plain close after `wdarm 20 sleep 2` still reset the SoC
 * ~19 s later. Treat arming as a one-shot reset timer: after the child
 * exits the parent ATTEMPTS disarm (WDIOC_SETOPTIONS WDIOS_DISABLECARD,
 * then magic-'V' write before close) and reports whether it worked. If
 * disarm fails, the reset is still coming — schedule the run so a
 * reboot afterwards is acceptable, and capture dmesg BEFORE relying on
 * the disarm. Exit status is the child's (or 1 on tool failure).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/watchdog.h>

static int try_disarm(int fd)
{
	int opt = WDIOS_DISABLECARD;
	int rc = ioctl(fd, WDIOC_SETOPTIONS, &opt);

	if (rc) {
		/* Fall back to the magic-close path (3.10 watchdog_dev.c:
		 * close stops the timer only with magic 'V' written, and
		 * only without WDIOF_MAGICCLOSE/NOWAYOUT). */
		if (write(fd, "V", 1) != 1)
			return -1;
		return 0;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int fd, to, rc, st;
	pid_t pid;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <timeout_s> <cmd...>\n", argv[0]);
		return 1;
	}
	to = atoi(argv[1]);
	/* Live-SoC limit (Main, 2026-11-20): the S3C watchdog on this
	 * part rejects anything above ~19 s (SETTIMEOUT 60 -> EINVAL).
	 * Fail fast with a clear message instead of a kernel error. */
	if (to <= 0 || to > 19) {
		fprintf(stderr, "wdarm: bad timeout '%s' (this SoC caps ~19 s)\n",
			argv[1]);
		return 1;
	}
	const char *dev = "/dev/watchdog";

	fd = open(dev, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "wdarm: open /dev/watchdog: %s\n",
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

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "wdarm: fork: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (pid == 0) {
		/* Child: run the test with the watchdog fd inherited. */
		execvp(argv[2], argv + 2);
		fprintf(stderr, "wdarm: exec %s: %s\n", argv[2],
			strerror(errno));
		_exit(127);
	}
	/* Parent: the watchdog is our timeout; a wedged kernel takes us
	 * (and the SoC) down with it, which is the point. */
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(st))
		printf("wdarm: child exited %d; attempting disarm...\n",
		       WEXITSTATUS(st));
	else if (WIFSIGNALED(st))
		printf("wdarm: child killed by signal %d; attempting disarm...\n",
		       WTERMSIG(st));
	else
		printf("wdarm: child done (status 0x%x); attempting disarm...\n",
		       st);
	fflush(stdout);
	rc = try_disarm(fd);
	if (write(fd, "V", 1) != 1 && rc)
		printf("wdarm: WARNING: disarm failed (%s) — reset still coming in ~%d s; capture dmesg NOW\n",
		       strerror(errno), to);
	else if (!rc)
		printf("wdarm: disarm ioctl accepted (verify: no reset follows)\n");
	else
		printf("wdarm: WARNING: SETOPTIONS refused, magic-close written; reset may still follow\n");
	fflush(stdout);
	close(fd);
	if (WIFEXITED(st))
		return WEXITSTATUS(st);
	return 1;
}
