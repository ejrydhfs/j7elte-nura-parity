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
	.glme_ms = 1000,
	.limit = 0,
	.only_idx = -1,
	.no_banksel = 0,
	.query_only = 0,
	.no_glme = 0,
	.no_power = 0,
	.power_delay_ms = 300,
	.power_off_ms = 1000,
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

/* ---------------------------------------------------------------- utils */

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
	size_t got, answered = 0;

	memset(aa, 0x80, sizeof(aa));
	logmsg("autobaud burst 16 x 0x80");
	if (xwrite(fd, aa, sizeof(aa)) < 0)
		return -1;
	sleep_ms(50);
	rx_dump(fd, 150, "        autobaud-rx");

	got = req(fd, f_nv_write, sizeof(f_nv_write), "0x001b <- 07 (NVRAM word 7)");
	if (got)
		answered++;
	got = req(fd, f_nv_read, sizeof(f_nv_read), "read 0x001b");
	answered += got > 0;
	got = req(fd, f_status, sizeof(f_status), "read 0x401a");
	answered += got > 0;
	got = req(fd, f_reg0, sizeof(f_reg0), "read register 0 (HW version)");
	answered += got > 0;
	got = req(fd, f_asic_ready, sizeof(f_asic_ready), "read item 0x95 (asic ready)");
	answered += got > 0;

	logmsg("prologue: %zu of 5 exchanges answered", answered);
	return answered >= 3 ? 0 : -1;
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
		{ "help",       no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	int fd, c, i, rc = 0;
	int cur_bank = -1;
	int ack;

	while ((c = getopt_long(argc, argv, "d:b:g:D:V:c:p:C:G:t:L:l:i:eqxnQO:P:S:w:z:h",
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
		case 'L': o.glme_ms = atoi(optarg); break;
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
		case 'h':
		default:
			fprintf(stderr,
				"usage: %s [-d DEV] [-b BAUD] [-g GPIO] [-D DIR]\n"
				"       [-V 1|2|3] [-c CHUNK] [-p PIECE] [-C PIECE_MAX] [-G MS] [-t MS]\n"
				"       [-L GLME_MS] [-O OFF_MS]\n"
				"       [-l BYTES] [-i IDX] [-e] [-q] [-x] [-n] [-P MS]\n"
				"       [-S HEX] [-w HEX] [-z HEX] [-Q]\n", argv[0]);
			return c == 'h' ? 0 : 2;
		}
	}
	if (o.chunk < 4 || o.chunk > 1020) {
		logmsg("chunk must be 4..1020");
		return 2;
	}
	if (o.piece < 1 || o.piece > 4096)
		return 2;

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

	logmsg("all blocks sent; post-patch state");
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
	read_item(fd, 0x94, "bank readback after upload");
	read_item(fd, 0x95, "asic ready after upload");
	req(fd, f_reg0, sizeof(f_reg0), "read register 0 after upload");
	req(fd, f_nv_read, sizeof(f_nv_read), "read 0x001b after upload");

	if (!o.no_glme) {
		logmsg("GLME switch");
		if (xwrite(fd, f_glme, sizeof(f_glme)) < 0)
			rc = 5;
		else
			rx_dump(fd, o.glme_ms, "        glme-rx");
	}

	/* final drain so nothing is lost from the transcript */
	rx_dump(fd, 100, "        tail");
out:
	close(fd);
	return rc;
}
