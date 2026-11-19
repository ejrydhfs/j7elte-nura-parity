/*
 * j7elte-gpsd - minimal BCM4752 GPS bridge for Samsung Galaxy J7 (j7elte)
 *
 * The vendor stack (/system/bin/gpsd, Broadcom "glgps") drives the BCM4752
 * over UART0 (/dev/ttySAC0) with Broadcom's proprietary binary "GLL" protocol
 * and synthesises NMEA itself; the Android HAL (gps.default.so) is only a
 * socket client of that daemon (/dev/socket/gps) and never touches the UART.
 *
 * This daemon does the two things that are actually evidenced by the vendor
 * configuration and the kernel driver:
 *   1. power sequencing through /sys/class/sec/gps/GPS_PWR_EN/value
 *      (gpio_export_link() in drivers/gps/sec_gps_bcm4752.c),
 *   2. raw 8N1 UART setup on /dev/ttySAC0 at the vendor baud rate
 *      (BaudRate="921600", GpioDelayMs="250" in /system/etc/gps.xml).
 *
 * Everything the chip emits is then inspected.  Only complete NMEA 0183
 * sentences are forwarded to consumers (a PTY for gpsd, and a Unix stream
 * socket for anything else).  Non-NMEA bytes are counted and, optionally,
 * dumped to a raw log so that the still-unknown Broadcom init exchange can be
 * characterised on real hardware.
 *
 * --init replays the part of Broadcom's BRI bring-up that was recovered
 * byte-for-byte from /system/bin/gpsd (autobaud burst, Bri_GetOutOfLegacy,
 * Bri_RequestHwVersion, Bri_RequestDebugSfr).  Nothing outside those
 * verified frames is written to the UART unless the user passes
 * --esw DIR, which additionally uploads the 4752_B2 RAM patch extracted
 * from their own /system/bin/gpsd (see docs/reports/gps-esw-upload.md)
 * and, with --glme, sends the GLME switch.  See
 * parity/reports/gps-init-impl.md for the evidence and the residual gap.
 *
 * License: MIT
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
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
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEV       "/dev/ttySAC0"
#define DEFAULT_BAUD      921600
#define DEFAULT_GPIO      "/sys/class/sec/gps/GPS_PWR_EN/value"
#define DEFAULT_RUNDIR    "/run/j7elte-gpsd"
#define DEFAULT_PTY_LINK  "tty"
#define DEFAULT_SOCK      "nmea.sock"
#define DEFAULT_PWR_DELAY 250      /* GpioDelayMs in /system/etc/gps.xml */
#define MAX_CLIENTS       8
#define NMEA_MAX          128      /* NMEA 0183 is 82 chars incl. CRLF */
#define RX_CHUNK          4096

static struct {
	const char *dev;
	int baud;
	const char *gpio;
	int power_delay_ms;
	const char *rundir;
	const char *sock_name;
	bool use_power;
	bool foreground;
	bool verbose;
	bool probe_baud;
	bool do_init;
	bool glme;
	const char *esw_dir;
	const char *raw_log;
} cfg = {
	.dev = DEFAULT_DEV,
	.baud = DEFAULT_BAUD,
	.gpio = DEFAULT_GPIO,
	.power_delay_ms = DEFAULT_PWR_DELAY,
	.rundir = DEFAULT_RUNDIR,
	.sock_name = DEFAULT_SOCK,
	.use_power = true,
	.foreground = false,
	.verbose = false,
	.probe_baud = false,
	.do_init = false,
	.glme = false,
	.esw_dir = NULL,
	.raw_log = NULL,
};

static volatile sig_atomic_t g_stop;
static int g_sigpipe[2] = { -1, -1 };

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
	if (g_sigpipe[1] >= 0) {
		ssize_t r = write(g_sigpipe[1], "x", 1);
		(void)r;
	}
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	struct timespec ts;
	struct tm tm;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d.%03ld j7elte-gpsd: ",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* --- power sequencing ------------------------------------------------- */

static int power_set(int on)
{
	int fd, ret;
	char v = on ? '1' : '0';

	if (!cfg.use_power)
		return 0;

	fd = open(cfg.gpio, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		logmsg("cannot open %s: %s (is the gps driver built in?)",
		       cfg.gpio, strerror(errno));
		return -1;
	}
	ret = write(fd, &v, 1);
	close(fd);
	if (ret != 1) {
		logmsg("cannot write %s: %s", cfg.gpio, strerror(errno));
		return -1;
	}
	return 0;
}

/* --- serial port ------------------------------------------------------ */

static speed_t baud_to_speed(int baud)
{
	switch (baud) {
	case 4800:    return B4800;
	case 9600:    return B9600;
	case 19200:   return B19200;
	case 38400:   return B38400;
	case 57600:   return B57600;
	case 115200:  return B115200;
	case 230400:  return B230400;
	case 460800:  return B460800;
#ifdef B500000
	case 500000:  return B500000;
#endif
#ifdef B576000
	case 576000:  return B576000;
#endif
	case 921600:  return B921600;
#ifdef B1000000
	case 1000000: return B1000000;
#endif
#ifdef B1500000
	case 1500000: return B1500000;
#endif
#ifdef B2000000
	case 2000000: return B2000000;
#endif
	default:      return (speed_t)-1;
	}
}

static int serial_open(const char *path, int baud)
{
	struct termios tio;
	speed_t spd = baud_to_speed(baud);
	int fd;

	if (spd == (speed_t)-1) {
		logmsg("unsupported baud rate %d", baud);
		return -1;
	}

	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		logmsg("cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	if (tcgetattr(fd, &tio) < 0) {
		logmsg("tcgetattr(%s): %s", path, strerror(errno));
		close(fd);
		return -1;
	}

	cfmakeraw(&tio);
	tio.c_cflag |= (CLOCAL | CREAD);
	tio.c_cflag &= ~CRTSCTS;   /* gps.xml sets no FlowControl */
	tio.c_cflag &= ~(PARENB | PARODD | CSTOPB);
	tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS8;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	if (cfsetispeed(&tio, spd) < 0 || cfsetospeed(&tio, spd) < 0) {
		logmsg("cfset*speed(%d): %s", baud, strerror(errno));
		close(fd);
		return -1;
	}
	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		logmsg("tcsetattr(%s): %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	tcflush(fd, TCIOFLUSH);

	logmsg("opened %s at %d 8N1, no flow control", path, baud);
	return fd;
}

/* --- output: PTY + Unix socket ---------------------------------------- */

struct out_ctx {
	int pty_master;
	char pty_slave_path[128];
	int listen_fd;
	int clients[MAX_CLIENTS];
	int nclients;
	int raw_fd;
};

static int mkdir_p(const char *path)
{
	char *tmp = strdup(path);
	char *p;
	int rc = 0;

	if (!tmp)
		return -1;
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
				rc = -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		rc = -1;
	free(tmp);
	return rc;
}

static int pty_setup(struct out_ctx *o)
{
	struct termios tio;
	char link_tmp[256], link_dst[256];
	const char *slave_path;
	int slave;
	size_t n;

	o->pty_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (o->pty_master < 0) {
		logmsg("posix_openpt: %s", strerror(errno));
		return -1;
	}
	if (grantpt(o->pty_master) < 0 || unlockpt(o->pty_master) < 0) {
		logmsg("grantpt/unlockpt: %s", strerror(errno));
		close(o->pty_master);
		o->pty_master = -1;
		return -1;
	}
	slave_path = ptsname(o->pty_master);
	if (!slave_path) {
		logmsg("ptsname: %s", strerror(errno));
		close(o->pty_master);
		o->pty_master = -1;
		return -1;
	}
	n = strlen(slave_path);
	if (n >= sizeof(o->pty_slave_path)) {
		logmsg("pty path too long");
		close(o->pty_master);
		o->pty_master = -1;
		return -1;
	}
	memcpy(o->pty_slave_path, slave_path, n + 1);

	/*
	 * Keep the slave open ourselves: otherwise the master would see EIO
	 * whenever no consumer (gpsd) has the slave open yet, and we would
	 * lose the first sentences after a consumer restart.
	 */
	slave = open(o->pty_slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (slave < 0) {
		logmsg("open(%s): %s", o->pty_slave_path, strerror(errno));
		close(o->pty_master);
		o->pty_master = -1;
		return -1;
	}
	if (tcgetattr(slave, &tio) == 0) {
		cfmakeraw(&tio);
		tio.c_cflag |= (CLOCAL | CREAD);
		tcsetattr(slave, TCSANOW, &tio);
	}
	/* The slave fd is intentionally leaked into the process for its
	 * lifetime; it must never be the last close. */

	/* stable, self-describing path: <rundir>/tty -> /dev/pts/N */
	snprintf(link_dst, sizeof(link_dst), "%s/%s", cfg.rundir, DEFAULT_PTY_LINK);
	snprintf(link_tmp, sizeof(link_tmp), "%s/.tty.%d", cfg.rundir, (int)getpid());
	unlink(link_tmp);
	if (symlink(o->pty_slave_path, link_tmp) == 0) {
		if (rename(link_tmp, link_dst) < 0) {
			logmsg("rename(%s): %s", link_dst, strerror(errno));
			unlink(link_tmp);
		}
	}

	logmsg("NMEA PTY ready: %s (stable link %s -> %s)",
	       o->pty_slave_path, link_dst, o->pty_slave_path);
	return 0;
}

static int sock_setup(struct out_ctx *o)
{
	struct sockaddr_un sa;
	char path[256];

	if (!cfg.sock_name)
		return 0;
	snprintf(path, sizeof(path), "%s/%s", cfg.rundir, cfg.sock_name);

	o->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (o->listen_fd < 0) {
		logmsg("socket(): %s", strerror(errno));
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(sa.sun_path)) {
		logmsg("socket path too long: %s", path);
		close(o->listen_fd);
		o->listen_fd = -1;
		return -1;
	}
	memcpy(sa.sun_path, path, strlen(path) + 1);
	unlink(path);
	if (bind(o->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		logmsg("bind(%s): %s", path, strerror(errno));
		close(o->listen_fd);
		o->listen_fd = -1;
		return -1;
	}
	chmod(path, 0660);
	if (listen(o->listen_fd, 4) < 0) {
		logmsg("listen(%s): %s", path, strerror(errno));
		close(o->listen_fd);
		o->listen_fd = -1;
		unlink(path);
		return -1;
	}
	logmsg("NMEA stream socket: %s", path);
	return 0;
}

static void out_init(struct out_ctx *o)
{
	memset(o, 0, sizeof(*o));
	o->pty_master = -1;
	o->listen_fd = -1;
	o->raw_fd = -1;
	o->nclients = 0;

	if (mkdir_p(cfg.rundir) < 0)
		logmsg("mkdir_p(%s): %s", cfg.rundir, strerror(errno));

	if (pty_setup(o) < 0)
		logmsg("no PTY output; NMEA will only go to the socket/log");

	if (sock_setup(o) < 0)
		logmsg("no Unix socket output");

	if (cfg.raw_log) {
		o->raw_fd = open(cfg.raw_log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		if (o->raw_fd < 0)
			logmsg("cannot open raw log %s: %s", cfg.raw_log, strerror(errno));
		else
			logmsg("raw UART log: %s", cfg.raw_log);
	}
}

static int write_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static void out_nmea(struct out_ctx *o, const char *line, size_t len)
{
	int i;

	if (o->pty_master >= 0) {
		if (write_all(o->pty_master, line, len) < 0 && cfg.verbose)
			logmsg("pty write: %s", strerror(errno));
	}
	for (i = 0; i < o->nclients; i++) {
		if (write_all(o->clients[i], line, len) < 0) {
			/* drop slow/dead client */
			close(o->clients[i]);
			o->clients[i] = o->clients[--o->nclients];
			i--;
		}
	}
}

static void out_accept(struct out_ctx *o)
{
	int fd;

	while (o->nclients < MAX_CLIENTS) {
		fd = accept4(o->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0)
			break;
		o->clients[o->nclients++] = fd;
		if (cfg.verbose)
			logmsg("socket client connected (%d total)", o->nclients);
	}
}

static void out_close(struct out_ctx *o)
{
	int i;
	char path[256];

	for (i = 0; i < o->nclients; i++)
		close(o->clients[i]);
	o->nclients = 0;
	if (o->listen_fd >= 0) {
		close(o->listen_fd);
		if (cfg.sock_name) {
			snprintf(path, sizeof(path), "%s/%s", cfg.rundir, cfg.sock_name);
			unlink(path);
		}
	}
	if (o->pty_master >= 0)
		close(o->pty_master);
	if (o->raw_fd >= 0)
		close(o->raw_fd);
	if (cfg.rundir) {
		snprintf(path, sizeof(path), "%s/%s", cfg.rundir, DEFAULT_PTY_LINK);
		unlink(path);
	}
}

/* --- NMEA framing ----------------------------------------------------- */

struct nmea_ctx {
	char buf[NMEA_MAX * 2];
	size_t len;
	unsigned long total_bytes;
	unsigned long nmea_sentences;
	unsigned long nmea_bad;
	unsigned long dropped_bytes;
};

static unsigned char nmea_checksum(const char *s, size_t n)
{
	unsigned char cs = 0;
	size_t i;

	for (i = 0; i < n; i++)
		cs ^= (unsigned char)s[i];
	return cs;
}

static void nmea_emit(struct out_ctx *o, struct nmea_ctx *c, char *line, size_t len)
{
	char out[NMEA_MAX + 4];
	size_t outlen;

	/* Strip CR/LF, re-terminate canonically with CRLF. */
	while (len && (line[len - 1] == '\r' || line[len - 1] == '\n'))
		len--;
	if (len == 0 || line[0] != '$')
		return;
	if (len > NMEA_MAX) {
		/* not a valid NMEA 0183 sentence (max 82 chars) */
		c->dropped_bytes += len;
		return;
	}

	outlen = len;
	memcpy(out, line, len);

	/* Verify the checksum when present; never drop on mismatch, the
	 * consumer (gpsd) does its own validation and we want a truthful
	 * raw view of what the receiver sent. */
	{
		char *star = memchr(out, '*', outlen);
		if (star && (size_t)(star - out) + 3 <= outlen) {
			unsigned int want;
			if (sscanf(star + 1, "%2x", &want) == 1) {
				if (nmea_checksum(out + 1, (size_t)(star - out) - 1) != (unsigned char)want)
					c->nmea_bad++;
			}
		}
	}

	out[outlen++] = '\r';
	out[outlen++] = '\n';
	out_nmea(o, out, outlen);
	c->nmea_sentences++;

	if (cfg.verbose) {
		out[outlen - 2] = '\0';
		logmsg("NMEA %s", out);
	}
}

static void nmea_feed(struct out_ctx *o, struct nmea_ctx *c,
		      const char *data, size_t n)
{
	size_t i;

	c->total_bytes += n;

	if (o->raw_fd >= 0) {
		ssize_t r = write(o->raw_fd, data, n);
		(void)r;
	}

	for (i = 0; i < n; i++) {
		char ch = data[i];

		if (ch == '\n') {
			nmea_emit(o, c, c->buf, c->len);
			c->len = 0;
			continue;
		}
		if (c->len >= sizeof(c->buf)) {
			/* runaway binary data: resynchronise on the next '$' */
			c->dropped_bytes += c->len;
			c->len = 0;
		}
		/* Keep printable bytes plus the trailing CR of CRLF. */
		if ((unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7f) {
			c->buf[c->len++] = ch;
		} else if (ch == '\r') {
			if (c->len)
				c->buf[c->len++] = ch;
		} else {
			/* Binary byte inside an unterminated line: this is not
			 * NMEA.  Discard the partial line and count it. */
			c->dropped_bytes += c->len + 1;
			c->len = 0;
		}
	}
}

/* --- BCM4752 BRI handshake (reverse-engineered from /system/bin/gpsd) ---
 *
 * Every byte below was recovered from Broadcom's glgps binary and is cited
 * with its address there (file offsets == runtime addresses, it is a PIE
 * linked at 0).  See parity/reports/gps-init-impl.md for the full table.
 *
 * Frame shapes (from the receive parser GlMeSrdAsicinit::
 * Bri_CheckForReceivedPacket_Sardine @0x2a789c and the request templates in
 * .rodata at 0x398b00..0x398b5c):
 *
 *   host -> chip request   0xFF 00 FD <cmd> <arg> FC                    (6 B)
 *   host -> chip request   0xFE 00 FD <cmd> <arg> <d0..d3> FC          (10 B)
 *   chip -> host response  0xFE 00 FD <cmd> <sub> <d0..d3> FC          (10 B)
 *
 * 0xFC is a constant frame terminator, there is no CRC and no sequence
 * number on this link (those TXcrc/RXcrc/TXseq/RXseq counters belong to the
 * separate glgps <-> Android-HAL socket protocol at 0x169090).  With
 * transport mode 1 the byte-escaping in FUN_002a599c is disabled ([0x10e]=0)
 * and none of the frames below contain the escape byte 0xE0 anyway.
 */

#define BRI_FRAME_MAX   16
#define BRI_RESP_MS     300    /* bri_wait4bri timer: 0x12c ms @0x2a1786 */
#define BRI_RETRY_MAX   4      /* INIT_STAGE A: "trial %u", cmp #3 @0x2a6596 */
#define BRI_BACKOFF_MS(n) (50 + 20 * (n))  /* 0x32 + 20*n @0x2a65fe */

/* GlMeSrdAsicinit::Bri_SendAutobaud @0x2a5ae4: memset(buf, 0x80, 16) in
 * the vendor code, then one 16-byte transport write. */
static const unsigned char bri_autobaud[16] = {
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};

/* GlMeSrdAsicinit::Bri_GetOutOfLegacy @0x2a618c, template copied from
 * .rodata 0x398b2c (ldr [0x398b2c]/[0x398b30] + ldrh [0x398b34]). */
static const unsigned char bri_get_out_of_legacy[10] = {
	0xfe, 0x00, 0xfd, 0x00, 0x00, 0x55, 0xaa, 0x00, 0xff, 0xfc,
};

/* GlMeSrdAsicinit::Bri_RequestHwVersion @0x2a6784, template at 0x398b38. */
static const unsigned char bri_request_hw_version[6] = {
	0xff, 0x00, 0xfd, 0x00, 0x00, 0xfc,
};

/* GlMeSrdAsicinit::Bri_RequestDebugSfr @0x2a6104, template at 0x398b04. */
static const unsigned char bri_request_debug_sfr[6] = {
	0xff, 0x00, 0xfd, 0x1f, 0x95, 0xfc,
};

/* GlMeSrdAsicinit::Bri_RequestNvRamWord7 @0x2a70b8, templates at .rodata
 * 0x398b40 / 0x398b4c / 0x398b54 (three frames in one function).  glgps
 * sends all three; on this unit they are the exchanges the chip actually
 * answers (see docs/reports/gps-toolchain.md). */
static const unsigned char bri_nvram_word7[10] = {
	0xfe, 0x00, 0xfd, 0x00, 0x1b, 0x07, 0x00, 0x00, 0x00, 0xfc,
};
static const unsigned char bri_nvram_read[6] = {
	0xff, 0x00, 0xfd, 0x00, 0x1b, 0xfc,
};
static const unsigned char bri_request_status[6] = {
	0xff, 0x00, 0xfd, 0x40, 0x1a, 0xfc,
};

/* Bytes read off the UART by the BRI handshake; kept only so the exit
 * summary can distinguish "the chip is mute" from "the chip only spoke
 * during init". */
static unsigned long bri_rx_bytes;

struct bri_rx {
	unsigned char buf[BRI_FRAME_MAX];
	size_t len;
	int in_frame;
	unsigned long frames;
	unsigned long discarded;
};

/*
 * Consume one received byte.  Returns 1 when a complete 0xFE .. 0xFC frame
 * sits in r->buf (r->len bytes), 0 otherwise.  The parser in glgps accepts
 * exactly the 10-byte 0xFE form, validates buf[1..2] == 00 FD and buf[9] ==
 * 0xFC, and derives cmd = buf[3], sub = buf[4] @0x2a7a5a.
 */
static int bri_rx_byte(struct bri_rx *r, unsigned char b)
{
	if (!r->in_frame) {
		if (b == 0xfe) {
			r->buf[0] = b;
			r->len = 1;
			r->in_frame = 1;
		} else {
			r->discarded++;
		}
		return 0;
	}

	r->buf[r->len++] = b;

	/* header check as soon as three bytes are in */
	if (r->len == 3 && (r->buf[1] != 0x00 || r->buf[2] != 0xfd)) {
		r->in_frame = 0;
		r->discarded += r->len;
		r->len = 0;
		if (b == 0xfe) {	/* resync on this byte */
			r->buf[0] = b;
			r->len = 1;
			r->in_frame = 1;
		}
		return 0;
	}

	if (b == 0xfc) {
		r->in_frame = 0;
		r->frames++;
		return 1;
	}
	if (r->len >= BRI_FRAME_MAX) {
		r->discarded += r->len;
		r->len = 0;
		r->in_frame = 0;
	}
	return 0;
}

static int bri_send(int fd, const unsigned char *buf, size_t len)
{
	size_t off = 0;

	/* Drop anything stale from the previous step before asking again. */
	(void)tcflush(fd, TCIFLUSH);

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd p = { fd, POLLOUT, 0 };
				if (poll(&p, 1, 1000) <= 0)
					return -1;
				continue;
			}
			logmsg("init: write(%s): %s", cfg.dev, strerror(errno));
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

/*
 * Wait up to timeout_ms for one complete response frame.  Returns 1 with the
 * frame copied to out/outlen, 0 on timeout, -1 on a fatal read error.
 * Non-frame bytes are counted (they are the chip still talking legacy).
 */
static int bri_wait(int fd, struct bri_rx *rx, int timeout_ms,
		    unsigned char *out, size_t *outlen)
{
	struct timespec t0, now;
	long elapsed = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (elapsed <= timeout_ms) {
		struct pollfd p = { fd, POLLIN, 0 };
		int r = poll(&p, 1, timeout_ms - (int)elapsed);

		if (r < 0) {
			if (errno == EINTR) {
				clock_gettime(CLOCK_MONOTONIC, &now);
				elapsed = (now.tv_sec - t0.tv_sec) * 1000L +
					  (now.tv_nsec - t0.tv_nsec) / 1000000L;
				continue;
			}
			logmsg("init: poll(%s): %s", cfg.dev, strerror(errno));
			return -1;
		}
		if (r == 0)
			return 0;

		if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
			unsigned char chunk[256];
			ssize_t n = read(fd, chunk, sizeof(chunk));
			ssize_t i;

			if (n <= 0) {
				if (n < 0 && (errno == EAGAIN || errno == EINTR))
					continue;
				logmsg("init: read(%s): %s", cfg.dev,
				       n == 0 ? "EOF" : strerror(errno));
				return -1;
			}
			bri_rx_bytes += (unsigned long)n;
			for (i = 0; i < n; i++) {
				if (bri_rx_byte(rx, chunk[i])) {
					memcpy(out, rx->buf, rx->len);
					*outlen = rx->len;
					return 1;
				}
			}
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (now.tv_sec - t0.tv_sec) * 1000L +
			  (now.tv_nsec - t0.tv_nsec) / 1000000L;
	}
	return 0;
}

static void bri_log_frame(const char *what, const unsigned char *f, size_t n)
{
	char hex[3 * BRI_FRAME_MAX + 1];
	size_t i;

	for (i = 0; i < n && i < BRI_FRAME_MAX; i++)
		snprintf(hex + 3 * i, 4, "%02x ", f[i]);
	hex[n < BRI_FRAME_MAX ? 3 * n : 3 * BRI_FRAME_MAX] = '\0';

	if (n >= 10)
		logmsg("%s: %s (cmd=0x%02x sub=0x%02x data=%02x %02x %02x %02x)",
		       what, hex, f[3], f[4], f[5], f[6], f[7], f[8]);
	else
		logmsg("%s: %s", what, hex);
}

/* 32-bit little-endian payload word of a 10-byte response frame. */
static unsigned long bri_word(const unsigned char *f, size_t n)
{
	if (n < 10)
		return 0;
	return (unsigned long)f[5] | (unsigned long)f[6] << 8 |
	       (unsigned long)f[7] << 16 | (unsigned long)f[8] << 24;
}

/*
 * Send one request and wait for one response frame.
 * Returns 1 on a reply, 0 on timeout, -1 on a fatal write/read error.
 */
static int bri_step(int fd, struct bri_rx *rx, const char *what,
		    const unsigned char *req, size_t reqlen, int timeout_ms,
		    unsigned char *frame, size_t *flen)
{
	int got;

	if (bri_send(fd, req, reqlen) < 0)
		return -1;
	logmsg("init: %s sent", what);
	got = bri_wait(fd, rx, timeout_ms, frame, flen);
	if (got > 0)
		bri_log_frame("init: reply", frame, *flen);
	else if (got == 0)
		logmsg("init: no reply to %s within %d ms", what, timeout_ms);
	return got;
}

/* --- ESW / RAM-patch upload -------------------------------------------- *
 *
 * The 4752_B2 patch is not part of this program: it is Broadcom firmware
 * that the user extracts from their own /system/bin/gpsd with
 * code/gps/esw/esw_dump --dump <dir>/b2 (see docs/reports/gps-esw.md).
 * Four files are expected in --esw DIR:
 *
 *   idx bank  addr    size  file                    injected by esw_dump
 *     0    0  0x7000  4060  b2-4752_B2-0.bin        .rodata
 *     1    0  0x7fec     4  b2-4752_B2-1.bin        .rodata
 *     2    1  0x8000  6777  b2-4752_B2-2.bin        .rodata
 *     3    0  0x7fd2     4  b2-4752_B2-3.bin        synthesised (zeros)
 *
 * Wire format (verified on the SM-J700M, docs/reports/gps-esw-upload.md):
 *
 *   data  fe <nwords> fd 61 17 de c0 <alo> <ahi> <bytes...> fc
 *         nwords = ceil(bytes/4), <= 255; a record is sent in <= 1020-byte
 *         chunks, an unaligned tail is folded into a 4-byte frame at
 *         addr+chunk-4; the UART write is paced in <= 160-byte pieces.
 *   crc   fe 00 fd 21 18 <alo> <ahi> <len_lo> <len_hi> fc
 *         one request per <= 4096-byte record piece; the chip answers
 *         fe 00 fd 2f 15 <4 bytes> fc.  The 4 bytes are chip-computed, are
 *         not the host CRC-32, and the vendor daemon only logs them.
 *   bank  fe 00 fd 61 16 <bank> 00 00 00 fc   (item 0x16 write; no reply)
 *         read back as item 0x94 (ff 00 fd 1f 94 fc).
 */
#define ESW_PIECE_MAX   4096   /* record piece / CRC boundary, [this+4] @0x2a8e24 */
#define ESW_CHUNK_MAX   1020   /* 0x3fc frame payload limit @0x2a7208 */
#define ESW_UART_PIECE  160    /* 0xa0 write() piece @0x2a72a4 */

struct esw_block {
	int bank;
	unsigned addr;
	size_t len;
	const char *name;
};

static const struct esw_block esw_blocks[] = {
	{ 0, 0x7000, 4060, "b2-4752_B2-0.bin" },
	{ 0, 0x7fec,    4, "b2-4752_B2-1.bin" },
	{ 1, 0x8000, 6777, "b2-4752_B2-2.bin" },
	{ 0, 0x7fd2,    4, "b2-4752_B2-3.bin" },
};

/* CRC-32/IEEE, only used to log (and thus cross-check) what was read from
 * disk against the digests in docs/reports/gps-esw.md §1. */
static unsigned long esw_crc32(const unsigned char *p, size_t n)
{
	unsigned long c = 0xffffffffUL;
	size_t i;
	int k;

	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320UL & (0UL - (c & 1UL)));
	}
	return c ^ 0xffffffffUL;
}

/* Write in <=160-byte pieces with a 1 ms gap, like the vendor transport. */
static int esw_send_paced(int fd, const unsigned char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		size_t n = len - off;
		ssize_t w;

		if (n > ESW_UART_PIECE)
			n = ESW_UART_PIECE;
		w = write(fd, buf + off, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd p = { fd, POLLOUT, 0 };
				if (poll(&p, 1, 1000) <= 0)
					return -1;
				continue;
			}
			logmsg("esw: write(%s): %s", cfg.dev, strerror(errno));
			return -1;
		}
		off += (size_t)w;
		if (off < len) {
			struct timespec ts = { 0, 1000000L };
			nanosleep(&ts, NULL);
		}
	}
	return 0;
}

static int esw_data_frame(int fd, unsigned addr, const unsigned char *data,
			  size_t nbytes)
{
	unsigned char hdr[9];
	unsigned char ft = 0xfc;
	unsigned nwords = (unsigned)((nbytes + 3) / 4);

	if (nwords > 255) {
		logmsg("esw: frame too large (%zu bytes)", nbytes);
		return -1;
	}
	hdr[0] = 0xfe;
	hdr[1] = (unsigned char)nwords;
	hdr[2] = 0xfd;
	hdr[3] = 0x61;
	hdr[4] = 0x17;
	hdr[5] = 0xde;
	hdr[6] = 0xc0;
	hdr[7] = (unsigned char)(addr & 0xff);
	hdr[8] = (unsigned char)((addr >> 8) & 0xff);

	if (esw_send_paced(fd, hdr, sizeof(hdr)) < 0)
		return -1;
	if (nbytes && esw_send_paced(fd, data, nbytes) < 0)
		return -1;
	return esw_send_paced(fd, &ft, 1);
}

/* Ask for the chip-side CRC of one record piece; log the 2f15 reply. */
static int esw_crc_request(int fd, struct bri_rx *rx, unsigned addr,
			   unsigned len)
{
	unsigned char req[10];
	unsigned char frame[BRI_FRAME_MAX];
	size_t flen = 0;
	int got;

	req[0] = 0xfe;
	req[1] = 0x00;
	req[2] = 0xfd;
	req[3] = 0x21;
	req[4] = 0x18;
	req[5] = (unsigned char)(addr & 0xff);
	req[6] = (unsigned char)((addr >> 8) & 0xff);
	req[7] = (unsigned char)(len & 0xff);
	req[8] = (unsigned char)((len >> 8) & 0xff);
	req[9] = 0xfc;

	got = bri_step(fd, rx, "ESW CRC request 21 18", req, sizeof(req),
		       BRI_RESP_MS, frame, &flen);
	if (got < 0)
		return -1;
	if (got > 0 && flen >= 10 && frame[3] == 0x2f && frame[4] == 0x15) {
		logmsg("esw: CRC ack %04x/%u = 0x%08lx", addr, len,
		       bri_word(frame, flen));
		return 1;
	}
	logmsg("esw: no 2f15 ack for %04x/%u", addr, len);
	return 0;
}

/* Item 0x16 = patch bank; read it back as item 0x94. */
static int esw_bank_switch(int fd, struct bri_rx *rx, int bank)
{
	unsigned char req[10];
	unsigned char frame[BRI_FRAME_MAX];
	size_t flen = 0;
	int got, attempt;

	req[0] = 0xfe;
	req[1] = 0x00;
	req[2] = 0xfd;
	req[3] = 0x61;
	req[4] = 0x16;
	req[5] = (unsigned char)bank;
	req[6] = req[7] = req[8] = 0x00;
	req[9] = 0xfc;
	got = bri_step(fd, rx, "ESW bank select (item 0x16)", req, sizeof(req),
		       BRI_RESP_MS, frame, &flen);
	if (got < 0)
		return -1;

	for (attempt = 0; attempt < 3; attempt++) {
		unsigned char rd[6] = { 0xff, 0x00, 0xfd, 0x1f, 0x94, 0xfc };

		got = bri_step(fd, rx, "ESW bank readback (item 0x94)", rd,
			       sizeof(rd), BRI_RESP_MS, frame, &flen);
		if (got < 0)
			return -1;
		if (got > 0 && flen >= 10 && frame[3] == 0x1f &&
		    frame[4] == 0x94) {
			unsigned long v = bri_word(frame, flen);

			if (v == (unsigned long)bank) {
				logmsg("esw: bank %d selected", bank);
				return 0;
			}
			logmsg("esw: bank readback 0x%lx, wanted %d", v, bank);
			continue;
		}
		break;
	}
	logmsg("esw: bank %d not confirmed by item 0x94", bank);
	return 0;
}

/* Send one record: <=4096-byte pieces, <=1020-byte frames, CRC per piece. */
static int esw_send_block(int fd, struct bri_rx *rx,
			  const struct esw_block *b, const unsigned char *data)
{
	size_t done = 0;
	int acks = 0, fails = 0;

	while (done < b->len) {
		size_t plen = b->len - done;
		size_t sent = 0;

		if (plen > ESW_PIECE_MAX)
			plen = ESW_PIECE_MAX;

		while (sent < plen) {
			size_t chunk = plen - sent;
			size_t aligned, tail;

			if (chunk > ESW_CHUNK_MAX)
				chunk = ESW_CHUNK_MAX;
			aligned = chunk & ~(size_t)3;
			tail = chunk - aligned;

			logmsg("esw: frame %04x +%zu bytes (aligned %zu tail %zu)",
			       b->addr + (unsigned)done + (unsigned)sent,
			       chunk, aligned, tail);
			if (aligned &&
			    esw_data_frame(fd, b->addr + (unsigned)done +
					   (unsigned)sent,
					   data + done + sent, aligned) < 0)
				return -1;
			if (tail) {
				unsigned a = (b->addr + (unsigned)done +
					      (unsigned)sent + (unsigned)chunk - 4) &
					     0xffff;

				if (esw_data_frame(fd, a,
						   data + done + sent + chunk - 4,
						   4) < 0)
					return -1;
			}
			sent += chunk;
		}

		{
			int r = esw_crc_request(fd, rx,
						 b->addr + (unsigned)done,
						 (unsigned)plen);

			if (r < 0)
				return -1;
			if (r > 0)
				acks++;
			else
				fails++;
		}
		done += plen;
	}
	logmsg("esw: %s: %zu bytes, %d/%d pieces acknowledged",
	       b->name, b->len, acks, acks + fails);
	return fails ? 1 : 0;
}

static int esw_read_block(const char *dir, const struct esw_block *b,
			  unsigned char **out)
{
	char path[512];
	FILE *f;
	long sz;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", dir, b->name);
	f = fopen(path, "rb");
	if (!f) {
		logmsg("esw: cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	if (fseek(f, 0, SEEK_END) < 0 || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) < 0) {
		logmsg("esw: %s: seek failed", path);
		fclose(f);
		return -1;
	}
	if ((size_t)sz != b->len) {
		logmsg("esw: %s is %ld bytes, expected %zu", path, sz, b->len);
		fclose(f);
		return -1;
	}
	*out = malloc(b->len ? b->len : 1);
	if (!*out) {
		fclose(f);
		return -1;
	}
	n = fread(*out, 1, b->len, f);
	fclose(f);
	if (n != b->len) {
		logmsg("esw: %s: short read (%zu/%zu)", path, n, b->len);
		free(*out);
		*out = NULL;
		return -1;
	}
	logmsg("esw: %s: %zu bytes, crc32 0x%08lx", b->name, b->len,
	       esw_crc32(*out, b->len));
	return 0;
}

/*
 * Upload the four 4752_B2 records in table order, switching the patch bank
 * when it changes (bank 0 for blocks 0/1/3, bank 1 for block 2).
 * Returns 0 when every record was sent, 1 when some CRC ack was missing,
 * -1 on a fatal I/O or file error.
 */
static int esw_upload(int fd, struct bri_rx *rx)
{
	int cur_bank = -1;
	int rc = 0;
	size_t i;

	for (i = 0; i < sizeof(esw_blocks) / sizeof(esw_blocks[0]); i++) {
		const struct esw_block *b = &esw_blocks[i];
		unsigned char *data = NULL;
		int r;

		if (esw_read_block(cfg.esw_dir, b, &data) < 0) {
			rc = -1;
			break;
		}
		if (b->bank != cur_bank) {
			if (esw_bank_switch(fd, rx, b->bank) < 0) {
				free(data);
				rc = -1;
				break;
			}
			cur_bank = b->bank;
		}
		logmsg("esw: loading %s at 0x%04x (bank %d)",
		       b->name, b->addr, b->bank);
		r = esw_send_block(fd, rx, b, data);
		free(data);
		if (r < 0) {
			rc = -1;
			break;
		}
		if (r > 0)
			rc = 1;
	}
	return rc;
}

/*
 * Cold-start handshake.  The vendor order is
 *   "initial" @0x2a1728 -> cfg+autobaud @0x2a5b58 -> [BRI wait] ->
 *   NvRamWord7 @0x2a70b8 -> wait4version @0x2a680c (HwVersion) ->
 *   wait4asic_ready @0x2a6234 (DebugSfr) -> esw_wait4version (GetOutOfLegacy).
 *
 * On j7elte hardware the chip answers nothing at all until it has seen the
 * 16 x 0x80 autobaud burst, never volunteers the unsolicited BRI announce
 * frame, and gates Bri_RequestHwVersion on the preceding NvRamWord7 exchange
 * (the vendor code asserts transport->[4] == 1 at 0x2a6784 before sending
 * it).  GetOutOfLegacy and DebugSfr stay unanswered even then.  The steps are
 * therefore sent in the order that the hardware actually answers, and a
 * silent chip is logged rather than treated as fatal.
 *
 * Returns 0 once the sequence has been played, -1 only on a fatal I/O error.
 */
static int asic_init(int fd)
{
	struct bri_rx rx;
	unsigned char frame[BRI_FRAME_MAX];
	size_t flen = 0;
	int attempt, got, answered = 0, sent = 0;
	unsigned long version;

	memset(&rx, 0, sizeof(rx));

	if (bri_send(fd, bri_autobaud, sizeof(bri_autobaud)) < 0)
		return -1;
	logmsg("init: autobaud burst sent (%zu x 0x80) at %d baud",
	       sizeof(bri_autobaud), cfg.baud);

	got = bri_wait(fd, &rx, BRI_RESP_MS, frame, &flen);
	if (got > 0) {
		bri_log_frame("init: BRI frame", frame, flen);
	} else if (got == 0) {
		logmsg("init: no unsolicited BRI frame within %d ms "
		       "(rx state: %lu bytes discarded); sending requests anyway",
		       BRI_RESP_MS, rx.discarded);
	}

	/* Bri_RequestNvRamWord7 @0x2a70b8 (templates 0x398b40/4c/54). */
	sent++;
	got = bri_step(fd, &rx, "Bri_RequestNvRamWord7",
		       bri_nvram_word7, sizeof(bri_nvram_word7), BRI_RESP_MS,
		       frame, &flen);
	if (got < 0)
		return -1;
	answered += got > 0;

	sent++;
	got = bri_step(fd, &rx, "NvRamWord7 read", bri_nvram_read,
		       sizeof(bri_nvram_read), BRI_RESP_MS, frame, &flen);
	if (got < 0)
		return -1;
	if (got > 0) {
		answered++;
		logmsg("init: NvRamWord7 word 7 = 0x%08lx", bri_word(frame, flen));
	}

	sent++;
	got = bri_step(fd, &rx, "status request 0x40/0x1a",
		       bri_request_status, sizeof(bri_request_status),
		       BRI_RESP_MS, frame, &flen);
	if (got < 0)
		return -1;
	if (got > 0) {
		answered++;
		logmsg("init: status 0x40/0x1a = 0x%08lx", bri_word(frame, flen));
	}

	/* bri_wait4version @0x2a680c sends Bri_RequestHwVersion on entry. */
	sent++;
	got = bri_step(fd, &rx, "Bri_RequestHwVersion",
		       bri_request_hw_version, sizeof(bri_request_hw_version),
		       BRI_RESP_MS, frame, &flen);
	if (got < 0)
		return -1;
	if (got > 0) {
		answered++;
		if (flen >= 10 && frame[3] == 0x00) {
			version = bri_word(frame, flen);
			/* "GlMeSrdAsicinit::Got BRI HW version(0x%lx)" */
			logmsg("init: BRI HW version 0x%08lx (asic type byte 0x%02x)",
			       version, frame[4]);
		}
	}

	/* bri_wait4asic_ready @0x2a6234 sends Bri_RequestDebugSfr on entry. */
	sent++;
	got = bri_step(fd, &rx, "Bri_RequestDebugSfr",
		       bri_request_debug_sfr, sizeof(bri_request_debug_sfr), 500,
		       frame, &flen);
	if (got < 0)
		return -1;
	if (got > 0) {
		answered++;
		/* the stage checks [0x13c] == 0x95, i.e. sub == 0x95 */
		if (flen >= 10 && frame[4] == 0x95)
			logmsg("init: DebugSfr acknowledged");
	}

	/* ESW / RAM patch: only when the user supplied the extracted blocks.
	 * The vendor loads the patch here, then switches to GLME. */
	if (cfg.esw_dir) {
		int er = esw_upload(fd, &rx);

		if (er < 0) {
			logmsg("init: ESW upload failed");
			return -1;
		}
		logmsg("init: ESW upload finished (%s)",
		       er ? "some CRC acks missing" : "all pieces acknowledged");
		if (cfg.glme) {
			/* Observed on this unit: the chip stops answering BRI
			 * after this frame and no e0 banner arrives; one run
			 * coincided with a device reboot.  Opt-in only. */
			sent++;
			got = bri_step(fd, &rx, "Bri_GetOutOfLegacy (after patch)",
				       bri_get_out_of_legacy,
				       sizeof(bri_get_out_of_legacy), 1000,
				       frame, &flen);
			if (got < 0)
				return -1;
			if (got > 0)
				answered++;
			logmsg("init: GLME switch %s",
			       got > 0 ? "answered" : "not answered (BRI may be gone)");
		} else {
			logmsg("init: stopping after the patch load "
			       "(no GLME switch; --glme enables it)");
		}
		logmsg("init: link check: %d of %d exchanges answered",
		       answered, sent);
		return 0;
	}

	/* INIT_STAGE A: "Resending Bri_GetOutOfLegacy %u!" with 50+20n ms
	 * backoff, up to four trials (@0x2a6508, template @0x2a618c). */
	for (attempt = 0; attempt < BRI_RETRY_MAX; attempt++) {
		sent++;
		got = bri_step(fd, &rx, "Bri_GetOutOfLegacy",
			       bri_get_out_of_legacy,
			       sizeof(bri_get_out_of_legacy),
			       BRI_BACKOFF_MS(attempt), frame, &flen);
		if (got < 0)
			return -1;
		if (got > 0) {
			answered++;
			break;
		}
	}

	logmsg("init: link check: %d of %d exchanges answered", answered, sent);
	logmsg("init: stopping here.  The next vendor stages "
	       "(Bri_ProgramSw/HwFlowControl @0x2a6aa0/0x2a6cfc, ESW/patch "
	       "loading, RF/clock programming) need either the escape-byte "
	       "search result or the ESW image, neither of which is available "
	       "(see parity/reports/gps-init-impl.md)");
	return 0;
}

/* --- main ------------------------------------------------------------- */

static void usage(FILE *f)
{
	fprintf(f,
"Usage: j7elte-gpsd [options]\n"
"\n"
"Bridge the BCM4752 GPS on /dev/ttySAC0 (Samsung Galaxy J7, j7elte) to NMEA\n"
"consumers through a PTY and a Unix stream socket.  Power is sequenced via\n"
"/sys/class/sec/gps/GPS_PWR_EN/value exactly like the vendor glgps.\n"
"\n"
"  -d, --device PATH     UART device (default " DEFAULT_DEV ")\n"
"  -b, --baud N          baud rate (default 921600, from /system/etc/gps.xml)\n"
"  -g, --gpio PATH       GPS power sysfs attribute (default " DEFAULT_GPIO ")\n"
"  -p, --power-delay MS  delay after power-on (default 250)\n"
"  -r, --rundir DIR      runtime directory (default " DEFAULT_RUNDIR ")\n"
"  -s, --socket NAME     Unix socket name inside rundir, 'none' disables\n"
"  -R, --raw-log PATH    append every received byte to PATH (diagnostics)\n"
"  -P, --probe-baud      if no NMEA arrives, retry 9600/115200/4800\n"
"  -i, --init            send the recovered BCM4752 BRI handshake first\n"
"  -E, --esw DIR         after the handshake, upload the BCM4752 patch\n"
"                        blocks b2-4752_B2-{0..3}.bin from DIR (implies -i;\n"
"                        extract them with code/gps/esw/esw_dump --dump)\n"
"  -G, --glme            after a successful upload send the GLME switch\n"
"                        (experimental: the chip leaves BRI silently)\n"
"  -n, --no-power        do not touch the power GPIO\n"
"  -f, --foreground      accepted for init-script compatibility (never forks)\n"
"  -v, --verbose         log every NMEA sentence and UART state\n"
"  -h, --help            this text\n"
"  -V, --version         print version\n");
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "device",      required_argument, NULL, 'd' },
		{ "baud",        required_argument, NULL, 'b' },
		{ "gpio",        required_argument, NULL, 'g' },
		{ "power-delay", required_argument, NULL, 'p' },
		{ "rundir",      required_argument, NULL, 'r' },
		{ "socket",      required_argument, NULL, 's' },
		{ "raw-log",     required_argument, NULL, 'R' },
		{ "probe-baud",  no_argument,       NULL, 'P' },
		{ "init",        no_argument,       NULL, 'i' },
		{ "esw",         required_argument, NULL, 'E' },
		{ "glme",        no_argument,       NULL, 'G' },
		{ "no-power",    no_argument,       NULL, 'n' },
		{ "foreground",  no_argument,       NULL, 'f' },
		{ "verbose",     no_argument,       NULL, 'v' },
		{ "help",        no_argument,       NULL, 'h' },
		{ "version",     no_argument,       NULL, 'V' },
		{ NULL, 0, NULL, 0 }
	};
	static const int probe_rates[] = { 9600, 115200, 4800 };
	struct out_ctx out;
	struct nmea_ctx nmea;
	struct pollfd pfds[MAX_CLIENTS + 3];
	struct sigaction sa;
	int fd = -1, c, ret = 0;
	time_t started, last_report;
	int probe_idx = -1;

	/* Safe to out_close() before out_init() has run. */
	memset(&out, 0, sizeof(out));
	out.pty_master = -1;
	out.listen_fd = -1;
	out.raw_fd = -1;
	nmea.total_bytes = 0;

	while ((c = getopt_long(argc, argv, "d:b:g:p:r:s:R:E:GPinfvhV", opts, NULL)) != -1) {
		switch (c) {
		case 'd': cfg.dev = optarg; break;
		case 'b': cfg.baud = atoi(optarg); break;
		case 'g': cfg.gpio = optarg; break;
		case 'p': cfg.power_delay_ms = atoi(optarg); break;
		case 'r': cfg.rundir = optarg; break;
		case 's':
			cfg.sock_name = (strcmp(optarg, "none") == 0) ? NULL : optarg;
			break;
		case 'R': cfg.raw_log = optarg; break;
		case 'P': cfg.probe_baud = true; break;
		case 'i': cfg.do_init = true; break;
		case 'E': cfg.esw_dir = optarg; cfg.do_init = true; break;
		case 'G': cfg.glme = true; break;
		case 'n': cfg.use_power = false; break;
		case 'f': cfg.foreground = true; break;
		case 'v': cfg.verbose = true; break;
		case 'h': usage(stdout); return 0;
		case 'V': puts("j7elte-gpsd 1.0"); return 0;
		default: usage(stderr); return 2;
		}
	}
	(void)cfg.foreground; /* this daemon never forks */

	if (baud_to_speed(cfg.baud) == (speed_t)-1) {
		logmsg("unsupported baud %d", cfg.baud);
		return 2;
	}

	if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		logmsg("pipe2: %s", strerror(errno));
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	logmsg("starting: uart=%s baud=%d power=%s delay=%dms",
	       cfg.dev, cfg.baud, cfg.use_power ? cfg.gpio : "untouched",
	       cfg.power_delay_ms);

	if (power_set(1) < 0)
		logmsg("continuing without power control");

	if (cfg.power_delay_ms > 0) {
		struct timespec ts = { cfg.power_delay_ms / 1000,
				       (cfg.power_delay_ms % 1000) * 1000000L };
		nanosleep(&ts, NULL);
	}

	fd = serial_open(cfg.dev, cfg.baud);
	if (fd < 0) {
		ret = 1;
		goto out;
	}

	out_init(&out);
	memset(&nmea, 0, sizeof(nmea));

	if (cfg.do_init) {
		/* Consumes the chip's handshake replies; anything that is not a
		 * BRI frame during this phase is counted, not forwarded. */
		if (asic_init(fd) < 0)
			logmsg("init: handshake aborted, falling back to the "
			       "raw receive loop");
		else
			logmsg("init: handshake sequence finished");
	}

	started = last_report = time(NULL);

	while (!g_stop) {
		int nfds = 0, r, serial_idx, listen_idx = -1, sig_idx;

		pfds[nfds].fd = fd;
		pfds[nfds].events = POLLIN;
		serial_idx = nfds++;

		if (out.listen_fd >= 0) {
			pfds[nfds].fd = out.listen_fd;
			pfds[nfds].events = POLLIN;
			listen_idx = nfds++;
		}

		sig_idx = nfds;
		pfds[nfds].fd = g_sigpipe[0];
		pfds[nfds].events = POLLIN;
		nfds++;

		r = poll(pfds, (nfds_t)nfds, 1000);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			logmsg("poll: %s", strerror(errno));
			break;
		}

		if (pfds[sig_idx].revents & POLLIN) {
			char tmp[16];
			ssize_t n = read(g_sigpipe[0], tmp, sizeof(tmp));
			(void)n;
			break;
		}

		if (listen_idx >= 0 && (pfds[listen_idx].revents & POLLIN))
			out_accept(&out);

		if (pfds[serial_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
			char buf[RX_CHUNK];
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n > 0) {
				nmea_feed(&out, &nmea, buf, (size_t)n);
			} else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				logmsg("read(%s): %s; reopening", cfg.dev,
				       n == 0 ? "EOF" : strerror(errno));
				close(fd);
				fd = serial_open(cfg.dev, cfg.baud);
				if (fd < 0) {
					ret = 1;
					break;
				}
			}
		}

		/* Status / diagnostics */
		{
			time_t now = time(NULL);

			if (now - last_report >= 30) {
				last_report = now;
				logmsg("stats: rx=%lu B nmea=%lu bad_cs=%lu dropped=%lu clients=%d",
				       nmea.total_bytes, nmea.nmea_sentences,
				       nmea.nmea_bad, nmea.dropped_bytes,
				       out.nclients);
			}

			if (nmea.total_bytes == 0 && now - started == 10)
				logmsg("no UART data 10s after power-on: the BCM4752 "
				       "does not free-run NMEA; try --init "
				       "(see parity/reports/gps-init-impl.md)");

			if (cfg.probe_baud && nmea.total_bytes == 0 &&
			    now - started >= 15 && probe_idx + 1 < (int)(sizeof(probe_rates) / sizeof(probe_rates[0]))) {
				probe_idx++;
				logmsg("probing alternative baud rate %d", probe_rates[probe_idx]);
				close(fd);
				fd = serial_open(cfg.dev, probe_rates[probe_idx]);
				if (fd < 0) {
					ret = 1;
					break;
				}
				started = now;
				nmea.total_bytes = 0;
			}
		}
	}

	/* If nothing was ever received, say so once more on exit. */
	if (nmea.total_bytes == 0)
		logmsg("exiting: no NMEA bytes were ever received from %s "
		       "(%lu bytes were consumed by the BRI handshake)",
		       cfg.dev, bri_rx_bytes);
	else
		logmsg("exiting: rx=%lu B, %lu NMEA sentences forwarded, "
		       "%lu bad checksums, %lu non-NMEA bytes discarded",
		       nmea.total_bytes, nmea.nmea_sentences,
		       nmea.nmea_bad, nmea.dropped_bytes);

out:
	out_close(&out);
	if (fd >= 0)
		close(fd);
	power_set(0);
	logmsg("GPS powered off");
	return ret;
}
