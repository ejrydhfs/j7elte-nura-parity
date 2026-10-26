/*
 * j7elte-power - battery charge-limit daemon for the Samsung Galaxy J7
 *                (SM-J700F/M, "j7elte", Exynos 7580 + SM5703).
 *
 * The 3.10 vendor kernel exposes the standard power_supply class for the
 * "battery" device, so UPower already gets capacity/status/present without
 * any help.  Two things the kernel does not do by itself are handled here:
 *
 *   1. The stock ROM's /system/bin/lpm ("LPMBattery") restores the fuelgauge
 *      capacity_max calibration from /efs/Battery/batt_capacity_max at boot
 *      and persists the value the gauge learns at runtime.  The boot half is
 *      done by restore-capacity-max (shipped with this package).  Persistence
 *      of the learned value is an opt-in feature of this daemon because it
 *      writes to the EFS partition.
 *
 *   2. Charge limiting (e.g. stop at 80%).  The SM5703/sec-battery driver has
 *      no charge_control_end_threshold; the only supported way to stop
 *      charging from userspace is the battery driver's "batt_slate_mode"
 *      attribute.  While it is 1 the driver disables charger current,
 *      disables the charger if the battery data says always_enable, and
 *      reports cable_type = BATTERY, i.e. status "Discharging" and
 *      ac/usb "online" = 0.  That is exactly what a charge limit looks like
 *      to UPower, and it is reverted as soon as the limit is released.
 *
 * Written against the interfaces that actually exist in
 * android_kernel_samsung_universal7580 (3.10.108), not against mainline
 * charge_control_* attributes.
 *
 * License: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/netlink.h>

#define DEFAULT_CONF		"/etc/j7elte-power.conf"
#define DEFAULT_BATTERY_DIR	"/sys/class/power_supply/battery"
#define DEFAULT_EFS_FILE	"/efs/Battery/batt_capacity_max"

/* Same sanity window the kernel store handler uses for batt_capacity_max:
 * sec_battery.c only forwards values > 800 and < 1200 to the fuelgauge, and
 * sm5703_fuelgauge.c then clamps them to [capacity_max - margin, capacity_max
 * + margin], i.e. [910, 1060] with the j7elte device tree (990/70): values
 * below 910 become 920, values above 1060 become 1060.  Keep the same gate
 * for persistence. */
#define CAP_MAX_MIN		801
#define CAP_MAX_MAX		1199

/* batt_slate_mode is the only writable charge control of this kernel. */
#define SLATE_ATTR		"batt_slate_mode"
#define CAP_MAX_ATTR		"batt_capacity_max"
#define CAPACITY_ATTR		"capacity"
#define STATUS_ATTR		"status"

struct config {
	char	battery_dir[PATH_MAX];
	char	efs_file[PATH_MAX];
	int	charge_limit;		/* percent, 0 = disabled */
	int	resume_margin;		/* percent */
	int	poll_interval;		/* seconds, safety poll */
	int	persist_capacity_max;	/* 0/1 */
};

static volatile sig_atomic_t g_stop;

/* Charge limiting state: 1 only when this process wrote slate_mode=1 and has
 * not released it yet.  We never clear a slate_mode we did not set. */
static int g_limited;
static long long g_last_slate_write;
static int g_warned_battery_io;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	fputs("j7elte-power: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* Monotonic seconds; wall-clock jumps must not disturb the safety poll. */
static long long now_sec(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return (long long)time(NULL);
	return (long long)ts.tv_sec;
}

/* snprintf wrapper: returns 0, or -1 with errno = ENAMETOOLONG. */
static int build_path(char *out, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(out, n, fmt, ap);
	va_end(ap);
	if (r < 0 || (size_t)r >= n) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

/* Read a sysfs attribute into buf, strip trailing whitespace.
 * Returns 0 on success, -1 on error (errno preserved). */
static int read_small(const char *path, char *buf, size_t len)
{
	ssize_t n;
	size_t i;
	int fd, saved;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	n = read(fd, buf, len - 1);
	saved = errno;
	close(fd);
	if (n < 0) {
		errno = saved;
		return -1;
	}
	buf[n] = '\0';
	for (i = (size_t)n; i > 0; i--) {
		char c = buf[i - 1];

		if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
			break;
		buf[i - 1] = '\0';
	}
	return 0;
}

static int read_int(const char *path, int *out)
{
	char buf[64];
	char *end;
	long v;

	if (read_small(path, buf, sizeof(buf)) < 0)
		return -1;
	errno = 0;
	v = strtol(buf, &end, 10);
	if (errno != 0 || end == buf) {
		errno = EINVAL;
		return -1;
	}
	*out = (int)v;
	return 0;
}

static int write_int(const char *path, int value)
{
	char buf[32];
	ssize_t n, w;
	int fd, saved;

	n = snprintf(buf, sizeof(buf), "%d\n", value);
	if (n <= 0 || (size_t)n >= sizeof(buf)) {
		errno = EINVAL;
		return -1;
	}

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	w = write(fd, buf, (size_t)n);
	saved = errno;
	close(fd);
	if (w != n) {
		errno = (w < 0) ? saved : EIO;
		return -1;
	}
	return 0;
}

/* Write "<value>\n" to an EFS file atomically (tmp + fsync + rename + dir fsync).
 * Only valid for a read-write EFS mount; the caller decides whether to try. */
static int write_file_atomic(const char *path, int value)
{
	char tmp[PATH_MAX], *slash;
	char buf[32];
	ssize_t n, w;
	int fd, dfd, saved;

	if (build_path(tmp, sizeof(tmp), "%s.tmp", path) < 0)
		return -1;

	n = snprintf(buf, sizeof(buf), "%d\n", value);
	if (n <= 0 || (size_t)n >= sizeof(buf)) {
		errno = EINVAL;
		return -1;
	}

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	w = write(fd, buf, (size_t)n);
	if (w != n) {
		saved = errno;
		close(fd);
		unlink(tmp);
		errno = (w < 0) ? saved : EIO;
		return -1;
	}
	if (fsync(fd) < 0) {
		saved = errno;
		close(fd);
		unlink(tmp);
		errno = saved;
		return -1;
	}
	close(fd);

	if (rename(tmp, path) < 0) {
		saved = errno;
		unlink(tmp);
		errno = saved;
		return -1;
	}

	/* Make the rename durable as well (best effort: the value is already
	 * in place). */
	if (build_path(tmp, sizeof(tmp), "%s", path) == 0) {
		slash = strrchr(tmp, '/');
		if (slash != NULL) {
			*slash = '\0';
			dfd = open(tmp, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			if (dfd >= 0) {
				(void)fsync(dfd);
				close(dfd);
			}
		}
	}
	return 0;
}

/* ------------------------------ config ---------------------------------- */

static void config_defaults(struct config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->battery_dir, sizeof(c->battery_dir), "%s", DEFAULT_BATTERY_DIR);
	snprintf(c->efs_file, sizeof(c->efs_file), "%s", DEFAULT_EFS_FILE);
	c->charge_limit = 0;
	c->resume_margin = 5;
	c->poll_interval = 60;
	c->persist_capacity_max = 0;
}

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\n' || end[-1] == '\r'))
		*--end = '\0';
	return s;
}

/* Strip one pair of matching single or double quotes. */
static void strip_quotes(char *s)
{
	size_t n = strlen(s);

	if (n < 2)
		return;
	if ((s[0] == '"' && s[n - 1] == '"') ||
	    (s[0] == '\'' && s[n - 1] == '\'')) {
		memmove(s, s + 1, n - 2);
		s[n - 2] = '\0';
	}
}

static int parse_nonneg(const char *s, int *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || v < 0 || v > INT_MAX)
		return -1;
	*out = (int)v;
	return 0;
}

/* Missing config file is not an error: built-in defaults apply. */
static int config_load(struct config *c, const char *path)
{
	FILE *f;
	char line[512];
	int lineno = 0;

	f = fopen(path, "re");
	if (f == NULL) {
		if (errno == ENOENT)
			return 0;
		logmsg("cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char *key, *value, *eq;

		lineno++;
		key = trim(line);
		if (*key == '\0' || *key == '#')
			continue;
		eq = strchr(key, '=');
		if (eq == NULL) {
			logmsg("%s:%d: expected key=value, ignoring", path, lineno);
			continue;
		}
		*eq = '\0';
		value = trim(eq + 1);
		key = trim(key);
		strip_quotes(value);

		if (strcmp(key, "battery_dir") == 0) {
			snprintf(c->battery_dir, sizeof(c->battery_dir), "%s", value);
		} else if (strcmp(key, "efs_capacity_max_file") == 0) {
			snprintf(c->efs_file, sizeof(c->efs_file), "%s", value);
		} else if (strcmp(key, "charge_limit") == 0) {
			if (parse_nonneg(value, &c->charge_limit) < 0)
				logmsg("%s:%d: bad charge_limit '%s'", path, lineno, value);
		} else if (strcmp(key, "resume_margin") == 0) {
			if (parse_nonneg(value, &c->resume_margin) < 0)
				logmsg("%s:%d: bad resume_margin '%s'", path, lineno, value);
		} else if (strcmp(key, "poll_interval") == 0) {
			if (parse_nonneg(value, &c->poll_interval) < 0)
				logmsg("%s:%d: bad poll_interval '%s'", path, lineno, value);
		} else if (strcmp(key, "persist_capacity_max") == 0) {
			if (parse_nonneg(value, &c->persist_capacity_max) < 0)
				logmsg("%s:%d: bad persist_capacity_max '%s'", path, lineno, value);
		} else {
			logmsg("%s:%d: unknown key '%s', ignoring", path, lineno, key);
		}
	}
	fclose(f);

	if (c->charge_limit != 0 &&
	    (c->charge_limit < 50 || c->charge_limit > 100)) {
		logmsg("charge_limit=%d out of range 50..100, disabling",
		       c->charge_limit);
		c->charge_limit = 0;
	}
	if (c->resume_margin < 1 || c->resume_margin > 20) {
		logmsg("resume_margin=%d out of range 1..20, using 5",
		       c->resume_margin);
		c->resume_margin = 5;
	}
	if (c->poll_interval < 5 || c->poll_interval > 3600) {
		logmsg("poll_interval=%d out of range 5..3600, using 60",
		       c->poll_interval);
		c->poll_interval = 60;
	}
	return 0;
}

/* ---------------------------- uevent socket ------------------------------ */

static int open_uevent(void)
{
	struct sockaddr_nl sa;
	int fd, flags;

	fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (fd < 0)
		return -1;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = 1;	/* kernel broadcast group */
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}

	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* A netlink uevent payload is a sequence of NUL-terminated "key=value"
 * fields ("add@/devices/...\0ACTION=add\0DEVPATH=...\0SUBSYSTEM=...\0").
 * strstr() stops at the first NUL, so match field by field over the whole
 * received length instead. */
static bool uevent_is_power_supply(const char *buf, size_t len)
{
	static const char tag[] = "SUBSYSTEM=power_supply";
	size_t off = 0;

	while (off < len) {
		size_t n = strnlen(buf + off, len - off);

		if (n == sizeof(tag) - 1 &&
		    memcmp(buf + off, tag, sizeof(tag) - 1) == 0)
			return true;
		off += n + 1;
	}
	return false;
}

/* Drain all pending uevents; return true if any was a power_supply change. */
static bool uevent_drain_power(int fd)
{
	char buf[2048];
	bool seen = false;
	int guard = 0;

	while (guard++ < 256) {
		ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;	/* EAGAIN: queue empty */
		}
		if (n == 0)
			break;
		buf[n] = '\0';
		if (uevent_is_power_supply(buf, (size_t)n))
			seen = true;
	}
	return seen;
}

/* ---------------------------- charge control ----------------------------- */

static int set_slate_mode(const struct config *c, int on)
{
	char path[PATH_MAX];

	if (build_path(path, sizeof(path), "%s/%s", c->battery_dir, SLATE_ATTR) < 0)
		return -1;
	if (write_int(path, on) < 0) {
		logmsg("cannot write %s: %s", path, strerror(errno));
		return -1;
	}
	g_last_slate_write = now_sec();
	return 0;
}

/* Current batt_slate_mode: 1/0, or -1 if the attribute cannot be read. */
static int read_slate_mode(const struct config *c)
{
	char path[PATH_MAX];
	int v;

	if (build_path(path, sizeof(path), "%s/%s", c->battery_dir, SLATE_ATTR) < 0)
		return -1;
	if (read_int(path, &v) < 0)
		return -1;
	return v != 0;
}

static int read_online(const char *base, const char *psy)
{
	char path[PATH_MAX];
	int v;

	if (build_path(path, sizeof(path), "%s/%s/online", base, psy) < 0)
		return 0;
	if (read_int(path, &v) < 0)
		return 0;	/* absent supply / read error -> not online */
	return v != 0;
}

static bool charger_plugged(const struct config *c)
{
	char base[PATH_MAX], *slash;

	snprintf(base, sizeof(base), "%s", c->battery_dir);
	slash = strrchr(base, '/');
	if (slash == NULL)
		return false;
	*slash = '\0';		/* /sys/class/power_supply */

	return read_online(base, "ac") ||
	       read_online(base, "usb") ||
	       read_online(base, "wireless");
}

static void log_battery_error_once(const char *what, const char *path)
{
	if (g_warned_battery_io)
		return;
	logmsg("cannot read %s (%s): %s", what, path, strerror(errno));
	g_warned_battery_io = 1;
}

/* Read capacity/status and drive the limit latch. */
static void evaluate(const struct config *c)
{
	char path[PATH_MAX], status[64];
	int capacity;
	bool plugged;

	if (build_path(path, sizeof(path), "%s/%s", c->battery_dir, CAPACITY_ATTR) < 0)
		return;
	if (read_int(path, &capacity) < 0) {
		log_battery_error_once("capacity", path);
		return;
	}
	g_warned_battery_io = 0;

	plugged = charger_plugged(c);

	/* g_limited can only be set while charge_limit > 0; a fresh process with
	 * limiting disabled never touches batt_slate_mode. */
	if (c->charge_limit <= 0)
		return;

	if (!g_limited) {
		if (plugged && capacity >= c->charge_limit) {
			if (set_slate_mode(c, 1) == 0) {
				g_limited = 1;
				logmsg("capacity %d%% >= limit %d%%, charging stopped",
				       capacity, c->charge_limit);
			}
		}
		return;
	}

	/* Latched: release when the pack has dropped by the resume margin. */
	if (capacity <= c->charge_limit - c->resume_margin) {
		if (set_slate_mode(c, 0) == 0) {
			g_limited = 0;
			logmsg("capacity %d%% <= %d%%, charging resumed",
			       capacity, c->charge_limit - c->resume_margin);
		}
		return;
	}

	/* The battery driver re-enables charging on some temperature/health
	 * recoveries.  If we see it charging again above the limit, re-apply
	 * the latch (rate limited to avoid a write/uevent storm). */
	if (!plugged)
		return;
	if (build_path(path, sizeof(path), "%s/%s", c->battery_dir, STATUS_ATTR) < 0)
		return;
	if (read_small(path, status, sizeof(status)) < 0)
		return;
	if (strcmp(status, "Charging") == 0 &&
	    now_sec() - g_last_slate_write >= 30) {
		logmsg("charging restarted above limit (%d%%), re-applying",
		       capacity);
		set_slate_mode(c, 1);	/* latch stays 1 */
	}
}

/* --------------------- capacity_max persistence (opt-in) ----------------- */

static int g_persisted = -1;

static void persist_capacity_max(const struct config *c)
{
	char path[PATH_MAX];
	int value, saved;

	if (!c->persist_capacity_max)
		return;
	if (build_path(path, sizeof(path), "%s/%s", c->battery_dir, CAP_MAX_ATTR) < 0)
		return;
	if (read_int(path, &value) < 0)
		return;
	if (value < CAP_MAX_MIN || value > CAP_MAX_MAX)
		return;
	if (value == g_persisted)
		return;

	if (write_file_atomic(c->efs_file, value) < 0) {
		saved = errno;
		/* ENOENT/EROFS means /efs is not mounted read-write; retry on
		 * the next poll without flooding the log. */
		if (saved != ENOENT && saved != EROFS && saved != EACCES)
			logmsg("cannot persist capacity_max to %s: %s",
			       c->efs_file, strerror(saved));
		return;
	}
	logmsg("persisted learned capacity_max=%d to %s", value, c->efs_file);
	g_persisted = value;
}

/* --------------------------------- main ---------------------------------- */

static void usage(const char *argv0)
{
	printf("usage: %s [--config PATH] [--once]\n", argv0);
	printf("\n");
	printf("  --config PATH  configuration file (default %s)\n", DEFAULT_CONF);
	printf("  --once         evaluate and persist once, then exit\n");
}

int main(int argc, char **argv)
{
	struct config cfg;
	const char *conf_path = DEFAULT_CONF;
	int once = 0;
	int uevent_fd, i;
	long long last_poll;
	struct sigaction sa;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
			conf_path = argv[++i];
		} else if (strcmp(argv[i], "--once") == 0) {
			once = 1;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			logmsg("unknown argument '%s'", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	config_defaults(&cfg);
	if (config_load(&cfg, conf_path) < 0)
		return 1;

	if (access(cfg.battery_dir, F_OK) < 0) {
		logmsg("%s not present: sec_battery driver not probed?",
		       cfg.battery_dir);
		return 1;
	}

	if (cfg.charge_limit == 0 && !cfg.persist_capacity_max && !once) {
		logmsg("no work configured (charge_limit=0, persist_capacity_max=0); "
		       "capacity_max restore is handled by "
		       "j7elte-efs-restore.service");
		return 0;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	/* Seed the persisted value so the first check does not rewrite an
	 * unchanged file. */
	if (cfg.persist_capacity_max) {
		int v;

		if (read_int(cfg.efs_file, &v) == 0)
			g_persisted = v;
	}

	uevent_fd = open_uevent();
	if (uevent_fd < 0)
		logmsg("uevent socket unavailable (%s), falling back to polling",
		       strerror(errno));

	logmsg("started: battery=%s limit=%d%% margin=%d%% poll=%ds persist=%d",
	       cfg.battery_dir, cfg.charge_limit, cfg.resume_margin,
	       cfg.poll_interval, cfg.persist_capacity_max);

	/* A previous instance (or a crash/SIGKILL) may have left the latch
	 * engaged.  Adopt it so the normal resume path can release it;
	 * otherwise charging would stay disabled after a service restart.
	 * With limiting disabled the attribute is never touched. */
	if (cfg.charge_limit > 0 && read_slate_mode(&cfg) == 1) {
		g_limited = 1;
		g_last_slate_write = now_sec();
		logmsg("adopting pre-existing batt_slate_mode=1");
	}

	/* Apply the limit immediately: booting at 90% with limit=80 must stop
	 * charging without waiting for the next uevent. */
	evaluate(&cfg);
	persist_capacity_max(&cfg);

	if (once) {
		if (uevent_fd >= 0)
			close(uevent_fd);
		return 0;
	}

	last_poll = now_sec();

	while (!g_stop) {
		struct pollfd pfd;
		long long elapsed;
		int timeout_ms;
		int r;
		elapsed = now_sec() - last_poll;
		if (elapsed < 0)
			elapsed = 0;
		if (elapsed >= cfg.poll_interval)
			timeout_ms = 0;
		else
			timeout_ms = (int)((cfg.poll_interval - elapsed) * 1000);

		pfd.fd = uevent_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		r = poll(uevent_fd >= 0 ? &pfd : NULL,
			 uevent_fd >= 0 ? 1 : 0,
			 timeout_ms);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			logmsg("poll: %s", strerror(errno));
			break;
		}

		if (r > 0 && (pfd.revents & POLLIN) &&
		    uevent_drain_power(uevent_fd)) {
			evaluate(&cfg);
			persist_capacity_max(&cfg);
			last_poll = now_sec();
			continue;
		}

		/* Safety poll: elapsed time, not poll()'s return value, so a
		 * stream of unrelated uevents cannot starve it. */
		if (now_sec() - last_poll >= cfg.poll_interval) {
			evaluate(&cfg);
			persist_capacity_max(&cfg);
			last_poll = now_sec();
		}
	}

	if (uevent_fd >= 0)
		close(uevent_fd);

	/* Never leave the charger disabled because the service stopped. */
	if (g_limited) {
		if (set_slate_mode(&cfg, 0) == 0)
			logmsg("exiting: charge limit released");
		else
			logmsg("exiting: FAILED to release charge limit; write 0 to "
			       "%s/%s manually", cfg.battery_dir, SLATE_ATTR);
	}
	logmsg("stopped");
	return 0;
}
