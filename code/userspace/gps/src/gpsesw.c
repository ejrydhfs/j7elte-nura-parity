/*
 * gpsesw - drive the BCM4752 "4752_B2" RAM-patch (ESW) upload on /dev/ttySAC0
 *
 * The extracted patch (see code/gps/esw/esw_dump.c and
 * docs/reports/gps-esw.md) is four blocks:
 *
 *   idx bank  load addr  size  source
 *     0    0     0x7000   4060  .rodata, verbatim
 *     1    0     0x7fec      4  .rodata, verbatim
 *     2    1     0x8000   6777  .rodata, verbatim
 *     3    0     0x7fd2      4  synthesised (four zero bytes)
 *
 * The public wire format for a data frame is
 *
 *   fe <nwords> fd 61 17 de c0 <addr lo> <addr hi> <data...> fc
 *
 * but the exact meaning of <nwords> and the relation between the chunk size,
 * the 160-byte UART write pieces and the address field are only partially
 * documented, so this tool implements the plausible encodings behind
 * -V and lets the hardware decide:
 *
 *   -V 1  public spec (mbicz/bcm4752-host): chunk = min(rem, -c); the chunk
 *          is split into aligned = chunk & ~3 and tail = chunk & 3; the
 *          aligned part gets one frame with nwords = aligned/4, the tail
 *          gets a second frame with nwords = ceil(tail/4) carrying the last
 *          four bytes of the chunk at addr+chunk-4.  UART writes are paced
 *          in <= -p byte pieces.
 *   -V 2  one frame per -p byte piece: nwords = ceil(piece_len/4), address
 *          advances by piece_len (the stock daemon's inner loop as read from
 *          the ARM32 binary).
 *   -V 3  one frame per chunk, all chunk bytes in the frame,
 *          nwords = ceil(chunk/4) (no separate odd-tail frame).
 *
 * Usage:
 *   gpsesw [-d DEV] [-b BAUD] [-g GPIO] [-D DIR] [-V N] [-c N] [-p N]
 *          [-G MS] [-t MS] [-l N] [-i IDX] [-e] [-q] [-x] [-n] [-S HEX]
 *
 *   -D DIR   directory holding b2-4752_B2-<idx>.bin (from esw_dump --dump)
 *   -V N     data-frame encoding variant (1..3, default 1)
 *   -c N     chunk size in bytes (default 1020 = 0x3fc)
 *   -p N     UART write piece inside a frame (default 160 = 0xa0)
 *   -G MS    delay between UART write pieces (default 1 ms)
 *   -t MS    reply timeout (default 300 ms)
 *   -l N     send at most N payload bytes per block (0 = whole block)
 *   -i IDX   send only block IDX (0..3), with its bank switch
 *   -e       skip the item 0x16 / bank-select writes
 *   -q       query only: prologue + item scan, no upload
 *   -x       do not send the GLME switch frame at the end
 *   -H N     hand-off variant tried against the chip (default 0 = old path):
 *            1  stock parity: blocks 0-2 only, switch immediately after the
 *               last CRC ack, bank 1 still selected, no readbacks
 *            2  blocks 0-2, the old post-patch readbacks, then switch
 *            3  all four blocks, re-select bank 1, switch immediately
 *            4  all four blocks, switch immediately (bank 0 still selected)
 *            5  blocks 0-2, wait HANDOFF_MS, then switch
 *            6  blocks 0-2, readbacks, re-select bank 1, then switch
 *   --golden deterministic vendor hand-off (see docs/reports/gps-glme.md):
 *            blocks 0-2 only (the 0x7fd2 fixup record is never sent for
 *            4752_B2), bank 1 left selected, the 0x55aa switch sent
 *            immediately after the last 2f15 ack, no readbacks, and a 3 s
 *            banner window.  Equivalent to -H 1 -L 3000.
 *   -B 1     vendor prologue: 16 x 0x80 immediately followed by the sync
 *            frame fe00fd210001330e10fc (no 0x001b writes/read)
 *   -n       do not touch GPS_PWR_EN (assume the chip is already up)
 *   -S HEX   first byte of the data frames (default fe)
 *
 * Every byte read back is printed as hex, so the transcript is the result.
 * Exit status is only about I/O; the transcript carries the verdict.
 *
 * License: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_BLOCKS 8

struct blk {
	int bank;
	unsigned addr;
	const char *name;
	unsigned char *data;
	size_t len;
	size_t sent;
};

static struct {
	const char *dev;
	int baud;
	const char *gpio;
	const char *dir;
	int variant;
	int chunk;
	int piece;
	int piece_max;      /* record transfer piece: CRC boundary (4096) */
	int gap_ms;
	int timeout_ms;
	int glme_ms;
	long limit;
	int only_idx;
	int no_banksel;
	int query_only;
	int no_glme;
	int no_power;
	int power_delay_ms;
	int power_off_ms;
	int nwrite;                      /* -w extra frames after prologue */
	const char *writes[16];
	int npost;                       /* -z extra frames after the upload */
	const char *post[16];
	int scan_after;                  /* -Q item scan after the upload */
	int handoff;                     /* -H post-patch hand-off variant */
	int handoff_ms;                  /* --handoff-delay for -H 5 */
	int prologue;                    /* -B 1: vendor autobaud+sync order */
	int golden;                      /* --golden: vendor post-patch hand-off */
	int glme_stream;                 /* --glme-stream: decode GLME after the switch */
	int glme_replay;                 /* --glme-replay: 4x txn1 replay + decode */
	const char *glme_check;          /* --glme-check DIR: offline image check */
	int glme_ms_set;                 /* -L given on the command line */
	unsigned char soff;
} o = {
	.dev = "/dev/ttySAC0",
	.baud = 921600,
	.gpio = "/sys/class/sec/gps/GPS_PWR_EN/value",
	.dir = "/tmp/gps_esw",
	.variant = 1,
	.chunk = 1020,
	.piece = 160,
	.piece_max = 4096,
	.gap_ms = 1,
	.timeout_ms = 300,
	.limit = 0,
	.only_idx = -1,
	.no_banksel = 0,
	.query_only = 0,
	.no_glme = 0,
	.no_power = 0,
	.power_delay_ms = 300,
	.power_off_ms = 1000,
	.handoff = 0,
	.handoff_ms = 100,
	.prologue = 0,
	.golden = 0,
	.glme_ms_set = 0,
	.soff = 0xfe,
};

static struct blk blocks[MAX_BLOCKS];
static int nblocks = 4;

/* last complete fe 00 fd ... fc frame seen by rx_dump() */
static unsigned char g_frame[16];
static size_t g_flen;
static unsigned g_frames;
static int crc_ack;      /* 1 when the last CRC request produced a 2f15 */
static int piece_acks;   /* 2f15 replies for the current record */
static int piece_fails;
static int g_banner;     /* 1 when an e0 00 ... e0 01 GLME frame was seen */
/* GLME framing state (post-switch binary protocol, not BRI).  Wire
 * form and escapes are documented on the codec section below. */
static unsigned char g_glme[8192];
static size_t g_glme_len;
static unsigned g_glme_frames;
static unsigned g_glme_ver;      /* 1 after the banner frame was seen */
static unsigned g_glme_seq;      /* host config sequence number (00, 02, ...) */
static void logmsg(const char *fmt, ...)
{
	va_list ap;
	struct timespec ts;
	struct tm tm;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	localtime_r(&ts.tv_sec, &tm);
	fprintf(stderr, "%02d:%02d:%02d.%03ld gpsesw: ", tm.tm_hour, tm.tm_min,
		tm.tm_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void hexdump(const char *tag, const unsigned char *b, size_t n)
{
	char line[3 * 64 + 1];
	size_t i, k = 0;

	fprintf(stderr, "%s (%zu):", tag, n);
	for (i = 0; i < n; i++) {
		snprintf(line + k, 4, "%02x ", b[i]);
		k += 3;
		if (k >= 3 * 32 || i + 1 == n) {
			line[k] = '\0';
			fprintf(stderr, " %s", line);
			k = 0;
		}
	}
	fputc('\n', stderr);
	fflush(stderr);
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static int power_set(int on)
{
	char v = on ? '1' : '0';
	int fd, r;

	if (o.no_power)
		return 0;
	fd = open(o.gpio, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		logmsg("open(%s): %s", o.gpio, strerror(errno));
		return -1;
	}
	r = write(fd, &v, 1);
	close(fd);
	if (r != 1) {
		logmsg("write(%s='%c'): %s", o.gpio, v, strerror(errno));
		return -1;
	}
	logmsg("GPS_PWR_EN <- %c", v);
	return 0;
}

static speed_t num2speed(long n)
{
	switch (n) {
	case 4800: return B4800;
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	default: return (speed_t)-1;
	}
}

static int serial_open(void)
{
	struct termios tio;
	speed_t sp = num2speed(o.baud);
	int fd;

	if (sp == (speed_t)-1) {
		logmsg("unsupported baud %d", o.baud);
		return -1;
	}
	fd = open(o.dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		logmsg("open(%s): %s", o.dev, strerror(errno));
		return -1;
	}
	if (tcgetattr(fd, &tio) < 0) {
		logmsg("tcgetattr: %s", strerror(errno));
		close(fd);
		return -1;
	}
	cfmakeraw(&tio);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~CRTSCTS;
	tio.c_cflag &= ~(PARENB | PARODD | CSTOPB);
	tio.c_cflag = (tio.c_cflag & ~CSIZE) | CS8;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	if (cfsetispeed(&tio, sp) < 0 || cfsetospeed(&tio, sp) < 0) {
		logmsg("cfset*speed(%d): %s", o.baud, strerror(errno));
		close(fd);
		return -1;
	}
	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		logmsg("tcsetattr: %s", strerror(errno));
		close(fd);
		return -1;
	}
	tcflush(fd, TCIOFLUSH);
	logmsg("opened %s at %d 8N1, no flow control", o.dev, o.baud);
	return fd;
}

static int xwrite(int fd, const unsigned char *b, size_t n)
{
	size_t off = 0;

	while (off < n) {
		size_t p = n - off;
		ssize_t w;

		if (p > (size_t)o.piece)
			p = (size_t)o.piece;
		w = write(fd, b + off, p);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd pd = { fd, POLLOUT, 0 };
				if (poll(&pd, 1, 1000) <= 0) {
					logmsg("write stalled");
					return -1;
				}
				continue;
			}
			logmsg("write(%s): %s", o.dev, strerror(errno));
			return -1;
		}
		off += (size_t)w;
		if (o.gap_ms && off < n)
			sleep_ms(o.gap_ms);
	}
	return 0;
}

/*
 * Read for timeout_ms and dump every byte.  Frames are also printed
 * individually (chip frames are fe 00 fd ... fc; the GLME banner is
 * e0 ... e0 01 and is not framed).  Returns the number of bytes read.
 */
static size_t rx_dump(int fd, int timeout_ms, const char *what)
{
	unsigned char buf[4096];
	size_t total = 0;
	struct timespec t0, now;
	long elapsed;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		int r;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (now.tv_sec - t0.tv_sec) * 1000L +
			  (now.tv_nsec - t0.tv_nsec) / 1000000L;
		if (elapsed >= timeout_ms)
			break;
		r = poll(&p, 1, (int)(timeout_ms - elapsed));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		{
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n <= 0) {
				if (n < 0 && (errno == EAGAIN || errno == EINTR))
					continue;
				break;
			}
			total += (size_t)n;
			hexdump(what, buf, (size_t)n);
			{
				size_t i, j;
				for (i = 0; i + 1 < (size_t)n; i++) {
					if (buf[i] != 0xe0 || buf[i + 1] != 0x00)
						continue;
					for (j = i + 2; j + 1 < (size_t)n; j++) {
						if (buf[j] == 0xe0 && buf[j + 1] == 0x01) {
							g_banner = 1;
							break;
						}
					}
				}
			}
			/* extract complete fe 00 fd .. fc frames from this burst */
			{
				size_t i = 0;
				while (i + 4 < (size_t)n) {
					size_t j;
					if (buf[i] != 0xfe || buf[i + 1] != 0x00 ||
					    buf[i + 2] != 0xfd) {
						i++;
						continue;
					}
					for (j = i + 3; j < (size_t)n && j < i + 120; j++)
						if (buf[j] == 0xfc)
							break;
					if (j >= (size_t)n || j >= i + 120)
						break;
					fprintf(stderr, "        frame cmd=0x%02x sub=0x%02x len=%zu: ",
						buf[i + 3], buf[i + 4], j - i + 1);
					{
						size_t k;
						for (k = i; k <= j; k++)
							fprintf(stderr, "%02x", buf[k]);
					}
					fputc('\n', stderr);
					fflush(stderr);
					g_frames++;
					g_flen = j - i + 1;
					if (g_flen > sizeof(g_frame))
						g_flen = sizeof(g_frame);
					memcpy(g_frame, buf + i, g_flen);
					i = j + 1;
				}
			}
		}
	}
	if (!total && what)
		fprintf(stderr, "        (%s: no bytes in %d ms)\n", what, timeout_ms);
	return total;
}
/* ------------------------------------------------- GLME codec -------- */
/*
 * Post-switch binary protocol, recovered from the stock daemon
 * (/system/bin/gpsd) against a PTY emulator plus one live GLME banner
 * (docs/reports/gps-glme-decode.md).
 *
 * Wire framing: e0 00 <payload> e0 01.  The payload escapes e0 02 for a
 * literal 0xe0, e0 03 for 0x00 and e0 04 for 0x01 (bcm4752-init SPEC
 * section 9); the terminator e0 01 never appears escaped, so framing
 * splits on raw e0 01 and unescapes afterwards.  BRI framing (fe/fc)
 * no longer applies once the 0x55aa switch is sent.
 *
 * Message table (payload byte 0 is always 0x00; byte 1 is the id):
 *
 *   id 0x00  version/status report (chip -> host, the banner): 13-byte
 *            payload 00 00 07 10 02 01 72 06 30 02 00 05 5f, where
 *            30 02 00 05 little-endian is the ASIC id 0x30020005.
 *            [OBS on live chip 2026-11-19]
 *   id 0x01  receiver-config download (host -> chip): first post-banner
 *            host transaction, 271 raw bytes (225 + 46).  Carries a
 *            fixed 56-byte header and TLV sections; see the report.
 *            The emulator run shows the host retrying it with the
 *            sequence byte (payload offset 47) at 00/02/04/06 until
 *            the chip answers.  [OBS vs stock daemon]
 *   id 0x02  bulk config part (host -> chip, 2644 B).  [OBS, truncated]
 *   id 0x0a  short control (host -> chip).  [OBS, truncated]
 *
 * No measurement/fix message has been observed on this unit yet: the
 * passively listening chip stays silent after the banner, and the
 * emulator (which never answers GLME) only elicits retries of the
 * id-0x01 config.  Measurement decode + NMEA synthesis therefore wait
 * on the chip's reply to the config; --glme-stream sends it and prints
 * whatever comes back.
 */

/* First post-banner host transaction (id 0x01), captured complete from
 * the stock daemon against the PTY emulator (gpsemu3, 2026-11-19):
 * 271 raw bytes = 225-byte frame + 46-byte frame.  Sent paced in
 * <=160-byte UART pieces like the vendor transport. */
static const unsigned char glme_cfg1[225] = {
	0xe0, 0x00, 0x00, 0x01, 0x00, 0xc3, 0x00, 0x01, 0x41, 0xcf, 0xec, 0xe4,
	0xd5, 0x74, 0xfb, 0x12, 0x2b, 0x1f, 0x4b, 0x4b, 0x01, 0xd0, 0x19, 0xd3,
	0x00, 0x05, 0x0b, 0x00, 0x0c, 0x3c, 0x42, 0x2a, 0x02, 0x49, 0x00, 0x4b,
	0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x94, 0x30, 0x21, 0x85, 0x03, 0x1a,
	0x64, 0x80, 0x64, 0x80, 0x64, 0x80, 0x64, 0x80, 0x64, 0x80, 0x18, 0x30,
	0x0e, 0x00, 0x00, 0x00, 0x00, 0x30, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x30,
	0x10, 0x00, 0x00, 0x00, 0x00, 0x30, 0xe0, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x30, 0x12, 0x00, 0x00, 0x00, 0x00, 0x30, 0xe0, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x30, 0x00, 0x00, 0x00, 0x15, 0x09, 0x30, 0x00, 0x00, 0x00, 0x15,
	0x0f, 0x38, 0x01, 0x00, 0x00, 0x00, 0x42, 0x38, 0x02, 0x00, 0x00, 0x00,
	0x0e, 0x38, 0x04, 0x00, 0x00, 0x00, 0x26, 0x38, 0x06, 0x00, 0x00, 0x00,
	0x3d, 0x38, 0x07, 0x00, 0x00, 0x00, 0x81, 0x38, 0x0b, 0x00, 0x00, 0x00,
	0x00, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00, 0x38, 0xe0, 0x03, 0x00, 0x00,
	0x00, 0x30, 0x38, 0x12, 0x00, 0x00, 0x00, 0xe6, 0x38, 0xe0, 0x04, 0x00,
	0x00, 0x00, 0x8c, 0x38, 0x14, 0x00, 0x00, 0x00, 0x1e, 0x38, 0x1a, 0x00,
	0x00, 0x00, 0x06, 0x38, 0x20, 0x00, 0x00, 0x00, 0x4c, 0x38, 0x2b, 0x00,
	0x00, 0x00, 0x6b, 0x38, 0x0b, 0x00, 0x00, 0x00, 0x04, 0x38, 0x02, 0x00,
	0x00, 0x00, 0x0b, 0x0b, 0x00, 0x02, 0x01, 0x38, 0x20, 0x00, 0x00, 0x00,
	0x40, 0x00, 0x00, 0x00, 0xe0, 0x02, 0xac, 0xe0, 0x01 };

/* Second frame of the first post-banner host transaction (id 0x01),
 * captured complete from the stock daemon against the PTY emulator
 * (gpsemu3, 2026-11-19): 46 raw bytes, sent immediately after the
 * 225-byte frame above as one 271-byte transaction. Raw offset 2 is
 * the retry sequence (01/03/05/07, always odd = even-seq + 1 of the
 * preceding 225-byte frame); raw offset 43 is an opaque per-retry
 * tail byte (f4/2e/85/5f, no sum/xor/crc8 relation to the body).
 * Canonical bytes below are retry 00/01; --glme-replay patches both
 * sequence bytes and both tails from the retry table. */
static const unsigned char glme_cfg1b[46] = {
	0xe0, 0x00, 0x01, 0x01, 0x01, 0x03, 0x03, 0x08, 0x01, 0x00, 0x05, 0x02,
	0x00, 0x0a, 0x00, 0x06, 0x04, 0x03, 0x00, 0x02, 0x00, 0x01, 0x00, 0x03,
	0x00, 0x0c, 0x01, 0x02, 0x01, 0x02, 0x00, 0x0a, 0x07, 0x01, 0x06, 0x00,
	0x09, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0xf4, 0xe0, 0x01 };

/* Per-retry opaque tail bytes for the 225-byte frame (raw offset 222)
 * and the 46-byte companion (raw offset 43), by retry index 0..3
 * (even seq 00/02/04/06, odd seq 01/03/05/07). No checksum relation
 * to the body was found (sum/xor/crc8 over several slicings tried);
 * replay the exact captured byte. */
static const unsigned char glme_retry_tail[4] = { 0xac, 0x63, 0xf7, 0x38 };
static const unsigned char glme_retry_tail_b[4] = { 0xf4, 0x2e, 0x85, 0x5f };

/* Unescape one GLME payload in place.  Returns the decoded length. */
static size_t glme_unescape(unsigned char *p, size_t n)
{
	size_t i = 0, w = 0;

	while (i < n) {
		if (p[i] == 0xe0 && i + 1 < n &&
		    (p[i + 1] == 0x02 || p[i + 1] == 0x03 ||
		     p[i + 1] == 0x04)) {
			p[w++] = p[i + 1] == 0x02 ? 0xe0 :
				 p[i + 1] == 0x03 ? 0x00 : 0x01;
			i += 2;
		} else {
			p[w++] = p[i++];
		}
	}
	return w;
}

/* Decode and print one complete GLME frame (including e0 00 / e0 01).
 * Prints the id, decoded length, TLV records when they parse, and a
 * minimal NMEA synthesis (GGA/RMC with void status) when the frame
 * carries position data.  Returns 1 for a fix-bearing frame, else 0. */
static int glme_decode_frame(const unsigned char *f, size_t n)
{
	unsigned char body[8192];
	size_t blen, i;
	unsigned id;
	static const char hexd[] = "0123456789abcdef";
	char hs[64];
	size_t hn;

	if (n < 5 || f[0] != 0xe0 || f[1] != 0x00 ||
	    f[n - 2] != 0xe0 || f[n - 1] != 0x01) {
		logmsg("glme: malformed frame (%zu bytes), skipped", n);
		return 0;
	}
	if (n - 4 > sizeof(body)) {
		logmsg("glme: frame too large (%zu bytes), skipped", n);
		return 0;
	}
	memcpy(body, f + 2, n - 4);
	blen = glme_unescape(body, n - 4);
	if (blen < 2 || body[0] != 0x00) {
		logmsg("glme: bad payload header (%zu decoded bytes)", blen);
		return 0;
	}
	id = body[1];
	/* short hex preview for the log */
	hn = blen < 32 ? blen : 32;
	for (i = 0; i < hn; i++) {
		hs[2 * i] = hexd[body[i] >> 4];
		hs[2 * i + 1] = hexd[body[i] & 15];
	}
	hs[2 * hn] = '\0';
	logmsg("glme: id=0x%02x raw=%zu decoded=%zu %s%s",
	       id, n, blen, hs, blen > 32 ? "..." : "");
	if (id == 0x00 && blen >= 9) {
		unsigned long asic = (unsigned long)body[6] |
				     ((unsigned long)body[7] << 8) |
				     ((unsigned long)body[8] << 16) |
				     ((unsigned long)body[9] << 24);
		logmsg("glme: version/status asic=0x%08lx fw=%02x%02x",
		       asic, body[3], body[4]);
		g_glme_ver = 1;
		return 0;
	}
	/* TLV scan: [tag u16 LE][value u32 LE] records.  Only the 0x38xx
	 * family parses cleanly in the captured config; other tag
	 * classes are dumped, not interpreted. */
	if (id == 0x01 && blen > 60) {
		size_t off = 104;
		unsigned recs = 0;

		if (off < blen && body[off] == 0x0f && off + 1 < blen &&
		    body[off + 1] == 0x38) {
			while (off + 6 <= blen && recs < 64) {
				unsigned tag = (unsigned)body[off] |
					       ((unsigned)body[off + 1] << 8);
				unsigned long val =
					(unsigned long)body[off + 2] |
					((unsigned long)body[off + 3] << 8) |
					((unsigned long)body[off + 4] << 16) |
					((unsigned long)body[off + 5] << 24);
				if ((tag & 0xff00) != 0x3800)
					break;
				logmsg("glme: cfg tag=0x%04x val=%lu",
				       tag, val);
				off += 6;
				recs++;
			}
			logmsg("glme: cfg section: %u TLV records", recs);
		}
	}
	return 0;
}

/* Feed raw bytes into the GLME framer; complete e0 00 ... e0 01 frames
 * are decoded as they arrive.  Returns the number of frames completed. */
static unsigned glme_feed(const unsigned char *d, size_t n)
{
	size_t i;
	unsigned done = 0;

	for (i = 0; i < n; i++) {
		if (g_glme_len >= sizeof(g_glme)) {
			logmsg("glme: overrun, resyncing");
			g_glme_len = 0;
		}
		g_glme[g_glme_len++] = d[i];
		/* resync: a frame always starts with e0 00 */
		if (g_glme_len == 1 && g_glme[0] != 0xe0)
			g_glme_len = 0;
		else if (g_glme_len == 2 && g_glme[1] != 0x00)
			g_glme_len = g_glme[1] == 0xe0 ? 1 : 0;
		if (g_glme_len >= 4 && g_glme[g_glme_len - 2] == 0xe0 &&
		    g_glme[g_glme_len - 1] == 0x01) {
			hexdump("        glme-frame", g_glme, g_glme_len);
			glme_decode_frame(g_glme, g_glme_len);
			g_glme_frames++;
			done++;
			g_glme_len = 0;
		}
	}
	return done;
}

/* NMEA checksum over the characters between $ and *. */
static unsigned nmea_cksum(const char *s)
{
	unsigned c = 0;

	while (*s)
		c ^= (unsigned char)*s++;
	return c;
}

/* Emit one NMEA sentence to stdout (the fix consumer reads stdout). */
static void nmea_out(const char *body)
{
	printf("$%s*%02X\r\n", body, nmea_cksum(body));
	fflush(stdout);
}

/* Minimal NMEA synthesiser.  No fix-bearing GLME message has been
 * observed on this unit yet (the chip never answered the id-0x01
 * config), so this emits the valid-but-void sentences the daemon
 * produces while waiting for a fix, proving the consumer path. */
static void nmea_void(void)
{
	nmea_out("GPGGA,000000.00,0000.0000,N,00000.0000,E,0,00,99.9,,M,,M,,");
	nmea_out("GPRMC,000000.00,V,0000.0000,N,00000.0000,E,0.0,0.0,010100,,,N");
}

/*
 * --glme-stream: after the banner, send the recovered id-0x01 config
 * transaction and decode the stream for -L ms.  Prints decoded frames
 * and void NMEA sentences.  Returns 0 when at least the banner plus
 * one more frame arrived, 7 (same cold-boot code as the banner wait)
 * when the chip stays silent.
 */
static int glme_send_txn1(int fd, unsigned retry)
{
	unsigned char a[sizeof(glme_cfg1)], b[sizeof(glme_cfg1b)];
	unsigned seq = (retry & 3) * 2;

	memcpy(a, glme_cfg1, sizeof(a));
	memcpy(b, glme_cfg1b, sizeof(b));
	a[2] = (unsigned char)seq;
	a[sizeof(a) - 3] = glme_retry_tail[retry & 3];
	b[2] = (unsigned char)(seq + 1);
	b[sizeof(b) - 3] = glme_retry_tail_b[retry & 3];
	logmsg("glme-replay: sending txn1 retry %u (seq %02x/%02x, %zu+%zu bytes)",
	       retry & 3, seq, seq + 1, sizeof(a), sizeof(b));
	hexdump("        glme-tx", a, sizeof(a));
	if (xwrite(fd, a, sizeof(a)) < 0)
		return -1;
	hexdump("        glme-tx", b, sizeof(b));
	if (xwrite(fd, b, sizeof(b)) < 0)
		return -1;
	g_glme_seq = seq + 2;
	return 0;
}

/* Offline check: verify the compiled-in retry-0 transaction against
 * code/userspace/gps/glme/txn1_seq00_*.bin. Returns 0 on match. */
static int glme_replay_selftest(const char *dir)
{
	static const char *names[2] = { "txn1_seq00_225.bin",
					"txn1_seq00_46.bin" };
	static const unsigned char *exp[2] = { glme_cfg1, glme_cfg1b };
	static const size_t explen[2] = { sizeof(glme_cfg1),
					  sizeof(glme_cfg1b) };
	int i, rc = 0;

	for (i = 0; i < 2; i++) {
		char path[1024];
		FILE *f;
		long sz;
		unsigned char *buf;

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		f = fopen(path, "rb");
		if (!f) {
			logmsg("glme-replay --check: missing %s: %s", path,
			       strerror(errno));
			return 1;
		}
		if (fseek(f, 0, SEEK_END) < 0 || (sz = ftell(f)) < 0 ||
		    fseek(f, 0, SEEK_SET) < 0) {
			logmsg("%s: seek failed", path);
			fclose(f);
			return 1;
		}
		if ((size_t)sz != explen[i]) {
			logmsg("glme-replay --check: %s: size %ld, want %zu",
			       path, sz, explen[i]);
			fclose(f);
			rc = 1;
			continue;
		}
		buf = malloc((size_t)sz ? (size_t)sz : 1);
		if (!buf) {
			fclose(f);
			return 1;
		}
		if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
			logmsg("%s: read failed", path);
			free(buf);
			fclose(f);
			return 1;
		}
		fclose(f);
		if (memcmp(buf, exp[i], explen[i]) != 0) {
			size_t k;

			for (k = 0; k < explen[i]; k++)
				if (buf[k] != exp[i][k])
					break;
			logmsg("glme-replay --check: %s: byte %zu differs "
			       "(file 0x%02x, image 0x%02x)", path, k,
			       buf[k], exp[i][k]);
			rc = 1;
		} else {
			logmsg("glme-replay --check: %s: %ld bytes match",
			       path, sz);
		}
		free(buf);
	}
	return rc;
}

static int glme_stream_run(int fd)
{
	struct pollfd p = { fd, POLLIN, 0 };
	unsigned char buf[4096];
	struct timespec t0, now;
	long elapsed;
	int r;
	unsigned char cfg[sizeof(glme_cfg1)];
	unsigned char cfg2[sizeof(glme_cfg1b)];
	size_t off = 0;

	logmsg("glme-stream: sending id-0x01 config (%zu+%zu bytes)",
	       sizeof(glme_cfg1), sizeof(glme_cfg1b));
	memcpy(cfg, glme_cfg1, sizeof(cfg));
	memcpy(cfg2, glme_cfg1b, sizeof(cfg2));
	/* sequence bytes: stock retries with 00/02/04/06 on the 225-byte
	 * frame and 01/03/05/07 on the 46-byte companion (raw offset 2
	 * of each frame); tails at raw offsets 222/43 are opaque
	 * per-retry bytes, patched from the retry table. */
	cfg[2] = (unsigned char)(g_glme_seq & 0xff);
	cfg[sizeof(cfg) - 3] = glme_retry_tail[(g_glme_seq / 2) & 3];
	cfg2[2] = (unsigned char)((g_glme_seq + 1) & 0xff);
	cfg2[sizeof(cfg2) - 3] = glme_retry_tail_b[(g_glme_seq / 2) & 3];
	hexdump("        glme-tx", cfg, sizeof(cfg));
	if (xwrite(fd, cfg, sizeof(cfg)) < 0)
		return 5;
	hexdump("        glme-tx", cfg2, sizeof(cfg2));
	if (xwrite(fd, cfg2, sizeof(cfg2)) < 0)
		return 5;
	g_glme_seq += 2;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (now.tv_sec - t0.tv_sec) * 1000L +
			  (now.tv_nsec - t0.tv_nsec) / 1000000L;
		if (elapsed >= o.glme_ms)
			break;
		r = poll(&p, 1, (int)(o.glme_ms - elapsed));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		{
			ssize_t n = read(fd, buf, sizeof(buf));

			if (n <= 0) {
				if (n < 0 && (errno == EAGAIN || errno == EINTR))
					continue;
				break;
			}
			hexdump("        glme-rx", buf, (size_t)n);
			off += glme_feed(buf, (size_t)n);
			(void)off;
		}
	}
	logmsg("glme-stream: %u frames in %d ms", g_glme_frames, o.glme_ms);
	if (!g_glme_frames) {
		logmsg("glme-stream: chip silent after config; needs the "
		       "chip reply (measurement stream) which no capture has "
		       "produced yet");
		nmea_void();
		return 7;
	}
	nmea_void();
	return 0;
}

/* --glme-replay: after the banner, send the recovered id-0x01
 * transaction 4x (225+46 bytes per retry, exact captured bytes
 * including per-retry tails) and decode the stream for -L ms.
 * Same exit codes as --glme-stream (0 = >=1 frame, 7 = silent). */
static int glme_replay_run(int fd)
{
	struct pollfd pl = { fd, POLLIN, 0 };
	unsigned char buf[4096];
	struct timespec rt0, rnow;
	long relapsed;
	int rr;
	unsigned n;
	size_t roff = 0;

	for (n = 0; n < 4; n++) {
		if (glme_send_txn1(fd, n) < 0)
			return 5;
	}
	clock_gettime(CLOCK_MONOTONIC, &rt0);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &rnow);
		relapsed = (rnow.tv_sec - rt0.tv_sec) * 1000L +
			  (rnow.tv_nsec - rt0.tv_nsec) / 1000000L;
		if (relapsed >= o.glme_ms)
			break;
		rr = poll(&pl, 1, (int)(o.glme_ms - relapsed));
		if (rr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (rr == 0)
			break;
		{
			ssize_t nread = read(fd, buf, sizeof(buf));

			if (nread <= 0) {
				if (nread < 0 && (errno == EAGAIN || errno == EINTR))
					continue;
				break;
			}
			hexdump("        glme-rx", buf, (size_t)nread);
			roff += glme_feed(buf, (size_t)nread);
			(void)roff;
		}
	}
	logmsg("glme-replay: %u frames in %d ms", g_glme_frames, o.glme_ms);
	if (!g_glme_frames) {
		logmsg("glme-replay: peer silent after 4x txn1; the chip "
		       "reply (measurement stream) is still the missing "
		       "piece - a cold-boot silicon run is needed");
		nmea_void();
		return 7;
	}
	nmea_void();
	return 0;
}


/* Send one request and dump the reply.  Returns bytes received. */
static size_t req(int fd, const unsigned char *b, size_t n, const char *what)
{
	logmsg("tx %s", what);
	hexdump("        tx", b, n);
	if (xwrite(fd, b, n) < 0)
		return 0;
	return rx_dump(fd, o.timeout_ms, "        rx");
}

/* --------------------------------------------------------------- frames */

static int read_item(int fd, unsigned item, const char *why)
{
	unsigned char f[6];
	char label[96];

	f[0] = 0xff;
	f[1] = 0x00;
	f[2] = 0xfd;
	f[3] = 0x1f;
	f[4] = (unsigned char)item;
	f[5] = 0xfc;
	snprintf(label, sizeof(label), "read item 0x%02x (%s)", item, why);
	g_frames = 0;
	g_flen = 0;
	req(fd, f, sizeof(f), label);
	if (g_flen >= 10 && g_frame[3] == 0x1f && g_frame[4] == item) {
		unsigned long v = (unsigned long)g_frame[5] |
				  (unsigned long)g_frame[6] << 8 |
				  (unsigned long)g_frame[7] << 16 |
				  (unsigned long)g_frame[8] << 24;
		logmsg("item 0x%02x = 0x%08lx", item, v);
	}
	return 0;
}

static int bank_select(int fd, int bank)
{
	unsigned char f[10];
	char label[64];
	size_t got;

	f[0] = 0xfe;
	f[1] = 0x00;
	f[2] = 0xfd;
	f[3] = 0x61;
	f[4] = 0x16;
	f[5] = (unsigned char)bank;
	f[6] = f[7] = f[8] = 0x00;
	f[9] = 0xfc;
	snprintf(label, sizeof(label), "item 0x16 <- bank %d", bank);
	got = req(fd, f, sizeof(f), label);
	if (!got)
		logmsg("bank select %d: no reply (expected)", bank);
	return 0;
}

/*
 * One data frame: fe <nwords> fd 61 17 de c0 <alo> <ahi> <data> fc.
 * nwords = ceil(nbytes/4); <= 255 by construction (chunk <= 1020).
 */
static int data_frame(int fd, unsigned addr, const unsigned char *data,
		      size_t nbytes)
{
	unsigned char hdr[9];
	unsigned char ft = 0xfc;
	unsigned nwords = (unsigned)((nbytes + 3) / 4);
	size_t sent_before = 0;

	(void)sent_before;
	if (nwords > 255) {
		logmsg("frame too large: %zu bytes -> %u words", nbytes, nwords);
		return -1;
	}
	hdr[0] = o.soff;
	hdr[1] = (unsigned char)nwords;
	hdr[2] = 0xfd;
	hdr[3] = 0x61;
	hdr[4] = 0x17;
	hdr[5] = 0xde;
	hdr[6] = 0xc0;
	hdr[7] = (unsigned char)(addr & 0xff);
	hdr[8] = (unsigned char)((addr >> 8) & 0xff);

	if (xwrite(fd, hdr, sizeof(hdr)) < 0)
		return -1;
	if (nbytes && xwrite(fd, data, nbytes) < 0)
		return -1;
	if (xwrite(fd, &ft, 1) < 0)
		return -1;
	return 0;
}

static int crc_request(int fd, unsigned addr, unsigned len)
{
	unsigned char f[10];

	f[0] = 0xfe;
	f[1] = 0x00;
	f[2] = 0xfd;
	f[3] = 0x21;
	f[4] = 0x18;
	f[5] = (unsigned char)(addr & 0xff);
	f[6] = (unsigned char)((addr >> 8) & 0xff);
	f[7] = (unsigned char)(len & 0xff);
	f[8] = (unsigned char)((len >> 8) & 0xff);
	f[9] = 0xfc;
	g_frames = 0;
	g_flen = 0;
	req(fd, f, sizeof(f), "CRC request 21 18");
	if (g_flen >= 10 && g_frame[3] == 0x2f && g_frame[4] == 0x15) {
		unsigned long v = (unsigned long)g_frame[5] |
				  (unsigned long)g_frame[6] << 8 |
				  (unsigned long)g_frame[7] << 16 |
				  (unsigned long)g_frame[8] << 24;
		logmsg("CRC ACK 2f15 value=0x%08lx", v);
		crc_ack = 1;
		piece_acks++;
		return 1;
	}
	logmsg("CRC ACK missing (frames=%u)", g_frames);
	crc_ack = 0;
	piece_fails++;
	return 0;
}

/* Host CRC-32 (zlib polynomial) for comparison with the chip's 2f15 value. */
static uint32_t crc32_host(const unsigned char *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	size_t i;
	int k;

	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
	}
	return ~c;
}

/* ------------------------------------------------------------- sequence */

/* Frames recovered on this hardware (docs/reports/gps-esw.md, prologue). */
static const unsigned char f_nv_write[10] = {
	0xfe, 0x00, 0xfd, 0x00, 0x1b, 0x07, 0x00, 0x00, 0x00, 0xfc };
static const unsigned char f_nv_read[6] = {
	0xff, 0x00, 0xfd, 0x00, 0x1b, 0xfc };
static const unsigned char f_status[6] = {
	0xff, 0x00, 0xfd, 0x40, 0x1a, 0xfc };
static const unsigned char f_reg0[6] = {
	0xff, 0x00, 0xfd, 0x00, 0x00, 0xfc };
static const unsigned char f_asic_ready[6] = {
	0xff, 0x00, 0xfd, 0x1f, 0x95, 0xfc };
static const unsigned char f_glme[10] = {
	0xfe, 0x00, 0xfd, 0x00, 0x00, 0x55, 0xaa, 0x00, 0xff, 0xfc };
/* autobaud/line-rate sync frame: the vendor sends it immediately after the
 * 16-byte 0x80 burst and the chip echoes it verbatim. */
static const unsigned char f_sync[10] = {
	0xfe, 0x00, 0xfd, 0x21, 0x00, 0x01, 0x33, 0x0e, 0x10, 0xfc };

/* Parse a hex string ("fe00fd..." — spaces/colons allowed) and send it. */
static int send_hex(int fd, const char *hex, const char *what)
{
	unsigned char buf[4096];
	size_t n = 0;
	const char *p = hex;

	while (*p && n < sizeof(buf)) {
		int hi, lo;

		if (*p == ' ' || *p == ':' || *p == ',') {
			p++;
			continue;
		}
		hi = *p >= '0' && *p <= '9' ? *p - '0' :
		     *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
		     *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
		if (hi < 0 || !p[1])
			return -1;
		p++;
		lo = *p >= '0' && *p <= '9' ? *p - '0' :
		     *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
		     *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
		if (lo < 0)
			return -1;
		p++;
		buf[n++] = (unsigned char)(hi << 4 | lo);
	}
	req(fd, buf, n, what);
	return 0;
}

static int prologue(int fd)
{
	unsigned char aa[16];
	size_t got, answered = 0, total = 5;

	memset(aa, 0x80, sizeof(aa));
	logmsg("autobaud burst 16 x 0x80");
	if (xwrite(fd, aa, sizeof(aa)) < 0)
		return -1;
	if (o.prologue == 1) {
		/* Vendor order: sync frame right behind the burst, echoed back. */
		logmsg("autobaud sync frame (vendor order)");
		got = req(fd, f_sync, sizeof(f_sync), "autobaud sync");
		answered += got > 0;
		total = 3;
	} else {
		sleep_ms(50);
		rx_dump(fd, 150, "        autobaud-rx");
		got = req(fd, f_nv_write, sizeof(f_nv_write), "0x001b <- 07 (NVRAM word 7)");
		if (got)
			answered++;
		got = req(fd, f_nv_read, sizeof(f_nv_read), "read 0x001b");
		answered += got > 0;
		got = req(fd, f_status, sizeof(f_status), "read 0x401a");
		answered += got > 0;
	}
	got = req(fd, f_reg0, sizeof(f_reg0), "read register 0 (HW version)");
	answered += got > 0;
	got = req(fd, f_asic_ready, sizeof(f_asic_ready), "read item 0x95 (asic ready)");
	answered += got > 0;

	logmsg("prologue: %zu of %zu exchanges answered", answered, total);
	/* old prologue: 3 of 5 answered is enough (0x001b write and 0x401a
	 * read are optional); vendor prologue: the sync echo or both reads. */
	return answered >= 3 ? 0 : (total == 3 && answered >= 2 ? 0 : -1);
}

static void item_scan(int fd)
{
	unsigned i;

	for (i = 0x90; i <= 0x9f; i++)
		read_item(fd, i, "scan");
}

static int send_piece(int fd, struct blk *b, size_t base, size_t len)
{
	unsigned char *p = b->data + base;
	size_t sent = 0;

	while (sent < len) {
		size_t chunk = len - sent;

		if (chunk > (size_t)o.chunk)
			chunk = (size_t)o.chunk;

		if (o.variant == 1) {
			size_t aligned = chunk & ~(size_t)3;
			size_t tail = chunk - aligned;

			logmsg("chunk at 0x%04x: %zu bytes (aligned %zu tail %zu)",
			       b->addr + (unsigned)base + (unsigned)sent, chunk,
			       aligned, tail);
			if (aligned &&
			    data_frame(fd, b->addr + (unsigned)base + (unsigned)sent,
				       p + sent, aligned) < 0)
				return -1;
			if (tail) {
				unsigned a = (b->addr + (unsigned)base +
					      (unsigned)sent + (unsigned)chunk - 4) &
					     0xffff;
				logmsg("odd tail: 4 bytes at 0x%04x", a);
				if (data_frame(fd, a,
					       p + sent + chunk - 4, 4) < 0)
					return -1;
			}
			/* no reply expected; keep the line drained */
			rx_dump(fd, 20, "        frame-rx");
		} else if (o.variant == 2) {
			size_t off = 0;

			while (off < chunk) {
				size_t pl = chunk - off;

				if (pl > (size_t)o.piece)
					pl = (size_t)o.piece;
				logmsg("piece at 0x%04x: %zu bytes",
				       b->addr + (unsigned)base + (unsigned)sent +
				       (unsigned)off, pl);
				if (data_frame(fd, b->addr + (unsigned)base +
					       (unsigned)sent + (unsigned)off,
					       p + sent + off, pl) < 0)
					return -1;
				off += pl;
				rx_dump(fd, 20, "        piece-rx");
			}
		} else {
			logmsg("chunk at 0x%04x: %zu bytes (single frame)",
			       b->addr + (unsigned)base + (unsigned)sent, chunk);
			if (data_frame(fd, b->addr + (unsigned)base + (unsigned)sent,
				       p + sent, chunk) < 0)
				return -1;
			rx_dump(fd, 20, "        frame-rx");
		}
		sent += chunk;
	}
	return 0;
}

/*
 * Send one record.  The stock loader transfers at most [this+4] = 4096
 * bytes per piece and asks for a CRC after each piece (gps-esw.md §4), so
 * the CRC boundary is the 4096-byte piece, not the whole record.
 */
static int send_one_block(int fd, struct blk *b)
{
	size_t len = b->len;
	size_t done = 0;
	size_t pmax = (size_t)o.piece_max;

	if (o.limit > 0 && (size_t)o.limit < len)
		len = (size_t)o.limit;
	if (pmax == 0)
		pmax = 0x1000;

	piece_acks = piece_fails = 0;
	logmsg("== block %s bank %d addr 0x%04x len %zu (host crc32 0x%08x)",
	       b->name, b->bank, b->addr, len, crc32_host(b->data, len));

	while (done < len) {
		size_t pl = len - done;

		if (pl > pmax)
			pl = pmax;
		logmsg("-- piece at 0x%04x, %zu bytes", b->addr + (unsigned)done, pl);
		if (send_piece(fd, b, done, pl) < 0)
			return -1;
		if (crc_request(fd, b->addr + (unsigned)done, (unsigned)pl) == 0)
			logmsg("piece at 0x%04x/%zu: no 2f15 ack", b->addr + (unsigned)done, pl);
		done += pl;
	}
	b->sent = len;
	return 0;
}

static int load_block(const char *dir, struct blk *b)
{
	char path[512];
	FILE *f;
	long sz;

	snprintf(path, sizeof(path), "%s/%s", dir, b->name);
	f = fopen(path, "rb");
	if (!f) {
		logmsg("missing block file %s: %s", path, strerror(errno));
		return -1;
	}
	if (fseek(f, 0, SEEK_END) < 0 || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) < 0) {
		logmsg("%s: seek failed", path);
		fclose(f);
		return -1;
	}
	b->data = malloc((size_t)sz ? (size_t)sz : 1);
	if (!b->data) {
		fclose(f);
		return -1;
	}
	b->len = fread(b->data, 1, (size_t)sz, f);
	fclose(f);
	logmsg("loaded %s: %zu bytes", path, b->len);
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "device",     required_argument, NULL, 'd' },
		{ "baud",       required_argument, NULL, 'b' },
		{ "gpio",       required_argument, NULL, 'g' },
		{ "dir",        required_argument, NULL, 'D' },
		{ "variant",    required_argument, NULL, 'V' },
		{ "chunk",      required_argument, NULL, 'c' },
		{ "piece",      required_argument, NULL, 'p' },
		{ "piece-max",  required_argument, NULL, 'C' },
		{ "gap",        required_argument, NULL, 'G' },
		{ "timeout",    required_argument, NULL, 't' },
		{ "limit",      required_argument, NULL, 'l' },
		{ "index",      required_argument, NULL, 'i' },
		{ "no-banksel", no_argument,       NULL, 'e' },
		{ "query",      no_argument,       NULL, 'q' },
		{ "no-glme",    no_argument,       NULL, 'x' },
		{ "no-power",   no_argument,       NULL, 'n' },
		{ "power-delay", required_argument, NULL, 'P' },
		{ "power-off",  required_argument, NULL, 'O' },
		{ "soff",       required_argument, NULL, 'S' },
		{ "glme-listen", required_argument, NULL, 'L' },
		{ "write",      required_argument, NULL, 'w' },
		{ "post",       required_argument, NULL, 'z' },
		{ "handoff",    required_argument, NULL, 'H' },
		{ "prologue",   required_argument, NULL, 'B' },
		{ "golden",     no_argument,       NULL, 1001 },
		{ "glme-stream", no_argument,      NULL, 1002 },
		{ "glme-replay", no_argument,      NULL, 1003 },
		{ "glme-check", required_argument, NULL, 1004 },
		{ "handoff-delay", required_argument, NULL, 1000 },
		{ "help",       no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	int fd, c, i, rc = 0;
	int cur_bank = -1;
	int ack;

	while ((c = getopt_long(argc, argv, "d:b:g:D:V:c:p:C:G:t:L:l:i:eqxnQO:P:S:w:z:H:B:h",
				opts, NULL)) != -1) {
		switch (c) {
		case 'd': o.dev = optarg; break;
		case 'b': o.baud = atoi(optarg); break;
		case 'g': o.gpio = optarg; break;
		case 'D': o.dir = optarg; break;
		case 'V': o.variant = atoi(optarg); break;
		case 'c': o.chunk = atoi(optarg); break;
		case 'p': o.piece = atoi(optarg); break;
		case 'C': o.piece_max = atoi(optarg); break;
		case 'G': o.gap_ms = atoi(optarg); break;
		case 't': o.timeout_ms = atoi(optarg); break;
		case 'L': o.glme_ms = atoi(optarg); o.glme_ms_set = 1; break;
		case 'l': o.limit = atol(optarg); break;
		case 'i': o.only_idx = atoi(optarg); break;
		case 'e': o.no_banksel = 1; break;
		case 'q': o.query_only = 1; break;
		case 'Q': o.scan_after = 1; break;
		case 'x': o.no_glme = 1; break;
		case 'n': o.no_power = 1; break;
		case 'P': o.power_delay_ms = atoi(optarg); break;
		case 'O': o.power_off_ms = atoi(optarg); break;
		case 'S': o.soff = (unsigned char)strtoul(optarg, NULL, 16); break;
		case 'w':
			if (o.nwrite < (int)(sizeof(o.writes) / sizeof(o.writes[0])))
				o.writes[o.nwrite++] = optarg;
			break;
		case 'z':
			if (o.npost < (int)(sizeof(o.post) / sizeof(o.post[0])))
				o.post[o.npost++] = optarg;
			break;
		case 1001:
			o.golden = 1;
			o.handoff = 1;
			if (!o.glme_ms_set)
				o.glme_ms = 3000;
			break;
		case 'H': o.handoff = atoi(optarg); break;
		case 'B': o.prologue = atoi(optarg); break;
		case 1002:
			o.glme_stream = 1;
			o.golden = 1;
			o.handoff = 1;
			if (!o.glme_ms_set)
				o.glme_ms = 15000;
			break;
		case 1003:
			o.glme_replay = 1;
			o.golden = 1;
			o.handoff = 1;
			if (!o.glme_ms_set)
				o.glme_ms = 15000;
			break;
		case 1004: o.glme_check = optarg; break;
		case 1000: o.handoff_ms = atoi(optarg); break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s [-d DEV] [-b BAUD] [-g GPIO] [-D DIR]\n"
				"       [-V 1|2|3] [-c CHUNK] [-p PIECE] [-C PIECE_MAX] [-G MS] [-t MS]\n"
				"       [-L GLME_MS] [-O OFF_MS]\n"
				"       [-l BYTES] [-i IDX] [-e] [-q] [-x] [-n] [-P MS]\n"
				"       [-S HEX] [-w HEX] [-z HEX] [-Q]\n"
				"       [-H 0..6] [--handoff-delay MS] [-B 1] [--golden]\n"
				"       [--glme-stream]  (implies --golden; sends the\n"
				"        recovered id-0x01 config after the banner and\n"
				"        decodes the GLME stream, default -L 15000)\n"
		       "[--glme-replay]  (implies --golden; sends txn1\n"
		       "        4x with exact per-retry bytes and decodes\n"
		       "        the stream, default -L 15000)\n"
		       "[--glme-check DIR]  (offline: verify the\n"
		       "        compiled-in txn1 against glme/*.bin)\n", argv[0]);
			return c == 'h' ? 0 : 2;
		}
	}
	if (o.chunk < 4 || o.chunk > 1020) {
		logmsg("chunk must be 4..1020");
		return 2;
	}
	if (o.piece < 1 || o.piece > 4096)
		return 2;
	if (o.handoff < 0 || o.handoff > 6) {
		logmsg("handoff variant must be 0..6");
		return 2;
	}

	{
		static const struct { int bank; unsigned addr; const char *name; } t[4] = {
			{ 0, 0x7000, "b2-4752_B2-0.bin" },
			{ 0, 0x7fec, "b2-4752_B2-1.bin" },
			{ 1, 0x8000, "b2-4752_B2-2.bin" },
			{ 0, 0x7fd2, "b2-4752_B2-3.bin" },
		};
		for (i = 0; i < nblocks; i++) {
			blocks[i].bank = t[i].bank;
			blocks[i].addr = t[i].addr;
			blocks[i].name = t[i].name;
		}
	}

	/* The vendor daemon never sends the synthetic 0x7fd2 record for
	 * 4752_B2: it lies inside block 0 and zeroing it breaks the patch. */
	if (o.handoff == 1 || o.handoff == 2 || o.handoff == 5 || o.handoff == 6)
		nblocks = 3;

	if (o.glme_check)
		return glme_replay_selftest(o.glme_check);

	if (!o.query_only) {
		for (i = 0; i < nblocks; i++) {
			if (o.only_idx >= 0 && i != o.only_idx)
				continue;
			if (load_block(o.dir, &blocks[i]) < 0)
				return 3;
		}
	}

	if (power_set(0) < 0)
		return 3;
	sleep_ms(o.power_off_ms);
	if (power_set(1) < 0)
		return 3;
	sleep_ms(o.power_delay_ms);

	fd = serial_open();
	if (fd < 0)
		return 3;

	if (prologue(fd) < 0) {
		logmsg("prologue did not answer; chip state unknown, stopping");
		rc = 4;
		goto out;
	}
	for (i = 0; i < o.nwrite; i++) {
		char label[64];

		snprintf(label, sizeof(label), "-w frame %d", i);
		if (send_hex(fd, o.writes[i], label) < 0) {
			logmsg("bad hex in -w %s", o.writes[i]);
			rc = 2;
			goto out;
		}
	}

	/* cheap state read before touching the patch */
	read_item(fd, 0x94, "bank readback before upload");
	read_item(fd, 0x95, "asic ready before upload");

	if (o.query_only) {
		item_scan(fd);
		goto out;
	}

	for (i = 0; i < nblocks; i++) {
		struct blk *b = &blocks[i];

		if (o.only_idx >= 0 && i != o.only_idx)
			continue;
		if (!b->data)
			continue;

		if (!o.no_banksel && b->bank != cur_bank) {
			bank_select(fd, b->bank);
			cur_bank = b->bank;
			if (b->bank)
				read_item(fd, 0x94, "bank readback after switch");
		}

		/* send_one_block() ends with the CRC request; ask it for the
		 * decoded 0x2f15 verdict. */
		if (send_one_block(fd, b) < 0) {
			rc = 5;
			break;
		}
		ack = piece_acks > 0 && piece_fails == 0;
		logmsg("block %d: %zu bytes, host crc32 0x%08x, acked pieces %d/%d",
		       i, b->sent, crc32_host(b->data, b->sent),
		       piece_acks, piece_acks + piece_fails);
		if (!ack) {
			/* Tell "CRC frame rejected" from "chip mute". */
			unsigned before = g_frames;

			read_item(fd, 0x95, "mute probe after failed block");
			logmsg("chip %s after block %d; stopping",
			       g_frames > before ? "still answering" : "is mute", i);
			rc = 6;
			break;
		}
	}
	if (rc)
		goto out;

	logmsg("all blocks sent (handoff variant %d); post-patch state", o.handoff);
	for (i = 0; i < o.npost; i++) {
		char label[64];

		snprintf(label, sizeof(label), "-z post frame %d", i);
		if (send_hex(fd, o.post[i], label) < 0) {
			logmsg("bad hex in -z %s", o.post[i]);
			rc = 2;
			goto out;
		}
	}
	if (o.scan_after)
		item_scan(fd);
	/* 1 and 5 go straight to the switch, like the vendor daemon: it sends
	 * the 0x55aa frame as soon as the last 2f15 ack is parsed. */
	if (o.handoff != 1 && o.handoff != 5) {
		read_item(fd, 0x94, "bank readback after upload");
		read_item(fd, 0x95, "asic ready after upload");
		req(fd, f_reg0, sizeof(f_reg0), "read register 0 after upload");
		req(fd, f_nv_read, sizeof(f_nv_read), "read 0x001b after upload");
	}
	if (o.handoff == 3 || o.handoff == 6) {
		/* Re-map the patch bank before switching: 61 16 01 + 1f 94. */
		bank_select(fd, 1);
		read_item(fd, 0x94, "bank readback after re-select");
	}
	if (o.handoff == 5)
		sleep_ms(o.handoff_ms);

	if (!o.no_glme) {
		logmsg("GLME switch");
		g_banner = 0;
		if (xwrite(fd, f_glme, sizeof(f_glme)) < 0) {
			rc = 5;
		} else {
			rx_dump(fd, o.glme_ms, "        glme-rx");
			if (g_banner) {
				logmsg("GLME banner received: hand-off works");
				if (o.glme_replay)
					rc = glme_replay_run(fd);
				else if (o.glme_stream)
					rc = glme_stream_run(fd);
			} else {
				logmsg("no GLME banner in %d ms: chip is mute for BRI; "
				       "a cold boot (device reboot) is required before "
				       "another BRI/ESW attempt", o.glme_ms);
				rc = 7;
			}
		}
	}

	/* final drain so nothing is lost from the transcript */
	rx_dump(fd, 100, "        tail");
out:
	close(fd);
	return rc;
}
