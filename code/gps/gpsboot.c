/*
 * gpsboot.c - scriptable serial bring-up helper for the BCM4752 GPS UART.
 *
 * Opens a tty (default /dev/ttySAC0) in raw 8N1 mode at a chosen baud rate and
 * executes a scripted list of operations given on the command line or read
 * from stdin, printing every byte it receives as hex.
 *
 * Why: bring-up experiments ("does the chip answer after power-on at baud X
 * with wake bytes Y?") must be repeatable and self-documenting, and the shell
 * can only do this with stty+dd+od incantations that hide timing. This tool
 * keeps delays, writes and read windows in one script with timestamps.
 *
 * Build (on the device, Alpine/musl):
 *     gcc -static -O2 -Wall -Wextra -o gpsboot gpsboot.c
 * Build (on the host, glibc cross toolchain):
 *     aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -o gpsboot gpsboot.c
 *
 * It is deliberately portable C (no arch specifics) so it builds for aarch64,
 * armv7 or x86-64 without changes.
 *
 * Op syntax (command line, one argv per op):
 *     w<delay_ms>:<hex>     delay, then write the bytes   e.g. w0:24000000
 *     r<timeout_ms>[:<max>] read for timeout_ms, at most max bytes (def 4096)
 *     d<ms>                 sleep only
 *     f                     discard pending input
 *   A missing delay/timeout part falls back to the defaults (-t / 0).
 *
 * Op syntax (stdin, one op per line, '#' starts a comment):
 *     w <delay_ms> <hex> [hex ...]
 *     r <timeout_ms> [max]
 *     d <ms>
 *     f
 *
 * Exit status: 0 on a fully executed script, 1 on open/setup/write failure,
 * 2 on a malformed script or bad option.
 */

/* cfmakeraw() and getline() need the default/GNU feature set. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEV "/dev/ttySAC0"
#define DEFAULT_BAUD 921600
#define DEFAULT_RCV_TIMEOUT 1000
#define MAX_READ 65536

enum op_kind { OP_DELAY, OP_WRITE, OP_READ, OP_FLUSH };

struct op {
	enum op_kind kind;
	unsigned delay_ms;	/* OP_WRITE preamble, OP_DELAY duration */
	unsigned timeout_ms;	/* OP_READ window */
	size_t max;		/* OP_READ byte cap */
	unsigned char *buf;	/* OP_WRITE payload */
	size_t len;
};

struct script {
	struct op *ops;
	size_t n, cap;
};

static long long t0;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void stamp(void)
{
	long long ms = now_ms() - t0;

	printf("[%5lld.%03lld] ", ms / 1000, ms % 1000);
}

static void die(const char *fmt, const char *arg)
{
	stamp();
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
	exit(1);
}

/* ---------- baud rate table ---------- */

struct baudent {
	unsigned rate;
	speed_t code;
};

static const struct baudent bauds[] = {
#ifdef B1200
	{1200, B1200},
#endif
#ifdef B2400
	{2400, B2400},
#endif
#ifdef B4800
	{4800, B4800},
#endif
#ifdef B9600
	{9600, B9600},
#endif
#ifdef B19200
	{19200, B19200},
#endif
#ifdef B38400
	{38400, B38400},
#endif
#ifdef B57600
	{57600, B57600},
#endif
#ifdef B115200
	{115200, B115200},
#endif
#ifdef B230400
	{230400, B230400},
#endif
#ifdef B460800
	{460800, B460800},
#endif
#ifdef B500000
	{500000, B500000},
#endif
#ifdef B576000
	{576000, B576000},
#endif
#ifdef B921600
	{921600, B921600},
#endif
#ifdef B1000000
	{1000000, B1000000},
#endif
#ifdef B1152000
	{1152000, B1152000},
#endif
#ifdef B1500000
	{1500000, B1500000},
#endif
#ifdef B2000000
	{2000000, B2000000},
#endif
#ifdef B2500000
	{2500000, B2500000},
#endif
#ifdef B3000000
	{3000000, B3000000},
#endif
#ifdef B3500000
	{3500000, B3500000},
#endif
#ifdef B4000000
	{4000000, B4000000},
#endif
};

static int baud_code(unsigned rate, speed_t *out)
{
	size_t i;

	for (i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++) {
		if (bauds[i].rate == rate) {
			*out = bauds[i].code;
			return 0;
		}
	}
	return -1;
}

/* ---------- parsing helpers ---------- */

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Parse a run of hex digits ("24000000", "0x24 00", "24:00:00") into bytes.
 * Returns 0 on success. */
static int parse_hex(const char *s, unsigned char **out, size_t *len)
{
	unsigned char *buf;
	size_t cap = strlen(s) / 2 + 1, n = 0;
	int hi = -1;

	buf = malloc(cap ? cap : 1);
	if (!buf)
		return -1;
	while (*s) {
		int v;

		if (*s == ' ' || *s == '\t' || *s == ':' || *s == ',' || *s == '-' ||
		    (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
			if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
				s++;
			s++;
			continue;
		}
		v = hexval((unsigned char)*s);
		if (v < 0) {
			free(buf);
			return -1;
		}
		if (hi < 0) {
			hi = v;
		} else {
			buf[n++] = (unsigned char)((hi << 4) | v);
			hi = -1;
		}
		s++;
	}
	if (hi >= 0 || n == 0) {	/* odd digit count or empty */
		free(buf);
		return -1;
	}
	*out = buf;
	*len = n;
	return 0;
}

static struct op *script_add(struct script *sc, enum op_kind kind)
{
	struct op *op;

	if (sc->n == sc->cap) {
		size_t cap = sc->cap ? sc->cap * 2 : 16;
		struct op *grown = realloc(sc->ops, cap * sizeof(*grown));

		if (!grown) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
		sc->ops = grown;
		sc->cap = cap;
	}
	op = &sc->ops[sc->n++];
	memset(op, 0, sizeof(*op));
	op->kind = kind;
	return op;	/* timeout_ms/max stay 0 = "use the -t/-m default" */
}

/* Fill in -t/-m defaults for read ops that did not specify their own. */
static void resolve_defaults(struct script *sc, unsigned def_timeout,
			     size_t def_max)
{
	size_t i;

	for (i = 0; i < sc->n; i++) {
		if (sc->ops[i].kind != OP_READ)
			continue;
		if (sc->ops[i].timeout_ms == 0)
			sc->ops[i].timeout_ms = def_timeout;
		if (sc->ops[i].max == 0)
			sc->ops[i].max = def_max;
	}
}

static int parse_uint(const char *s, unsigned *out)
{
	char *end;

	if (!s || !*s || !isdigit((unsigned char)*s))
		return -1;
	errno = 0;
	*out = (unsigned)strtoul(s, &end, 10);
	if (errno || *end)
		return -1;
	return 0;
}

/* "w<delay>:<hex>" or "w:<hex>" or "w<hex>" */
static int parse_argv_write(const char *s, struct script *sc)
{
	const char *colon = strchr(s, ':');
	char delaybuf[16];
	unsigned delay = 0;

	if (colon) {
		size_t n = (size_t)(colon - s);

		if (n >= sizeof(delaybuf))
			return -1;
		memcpy(delaybuf, s, n);
		delaybuf[n] = '\0';
		if (n && parse_uint(delaybuf, &delay) < 0)
			return -1;
		s = colon + 1;
	}
	{
		struct op *op = script_add(sc, OP_WRITE);

		if (parse_hex(s, &op->buf, &op->len) < 0) {
			sc->n--;
			return -1;
		}
		op->delay_ms = delay;
	}
	return 0;
}

/* "r<timeout>[:<max>]" / "r" -- validate first, then append. */
static int parse_argv_read(const char *s, struct script *sc)
{
	const char *colon = strchr(s, ':');
	struct op *op;
	char tmp[16];
	unsigned timeout = 0, max = 0;

	if (colon) {
		size_t n = (size_t)(colon - s);

		if (n) {
			if (n >= sizeof(tmp))
				return -1;
			memcpy(tmp, s, n);
			tmp[n] = '\0';
			if (parse_uint(tmp, &timeout) < 0)
				return -1;
		}
		if (parse_uint(colon + 1, &max) < 0 || max == 0 ||
		    max > MAX_READ)
			return -1;
	} else if (*s) {
		if (parse_uint(s, &timeout) < 0)
			return -1;
	}
	op = script_add(sc, OP_READ);
	op->timeout_ms = timeout;	/* 0 = defer to -t */
	op->max = max;			/* 0 = defer to -m */
	return 0;
}

/* Step on argv: w…/r…/d…/f */
static int parse_argv_step(const char *s, struct script *sc)
{
	switch (*s) {
	case 'w':
		return parse_argv_write(s + 1, sc);
	case 'r':
		return parse_argv_read(s + 1, sc);
	case 'd': {
		struct op *op = script_add(sc, OP_DELAY);

		if (parse_uint(s + 1, &op->delay_ms) < 0) {
			sc->n--;
			return -1;
		}
		return 0;
	}
	case 'f':
		if (s[1])
			return -1;
		script_add(sc, OP_FLUSH);
		return 0;
	default:
		return -1;
	}
}

/* One stdin line: "w <delay> <hex…>" / "r <ms> [max]" / "d <ms>" / "f" */
static int parse_line(char *line, struct script *sc)
{
	char *hash = strchr(line, '#');
	char *tok, *save = NULL;
	char *words[512];
	int nw = 0;

	if (hash)
		*hash = '\0';
	tok = strtok_r(line, " \t\r\n", &save);
	while (tok) {
		if (nw == (int)(sizeof(words) / sizeof(words[0])))
			return -1;
		words[nw++] = tok;
		tok = strtok_r(NULL, " \t\r\n", &save);
	}
	if (nw == 0)
		return 0;	/* blank or comment */

	if (strcmp(words[0], "w") == 0) {
		struct op *op;
		size_t i;

		if (nw < 3)
			return -1;
		op = script_add(sc, OP_WRITE);
		if (parse_uint(words[1], &op->delay_ms) < 0) {
			sc->n--;
			return -1;
		}
		for (i = 2; i < (size_t)nw; i++) {
			unsigned char *b;
			size_t len;

			if (parse_hex(words[i], &b, &len) < 0) {
				sc->n--;
				return -1;
			}
			{
				unsigned char *grown =
					realloc(op->buf, op->len + len);

				if (!grown) {
					free(b);
					sc->n--;
					return -1;
				}
				op->buf = grown;
			}
			memcpy(op->buf + op->len, b, len);
			op->len += len;
			free(b);
		}
		return 0;
	}
	if (strcmp(words[0], "r") == 0) {
		struct op *op = script_add(sc, OP_READ);

		if (nw >= 2 && parse_uint(words[1], &op->timeout_ms) < 0)
			return -1;
		if (nw >= 3) {
			unsigned v;

			if (parse_uint(words[2], &v) < 0 || v == 0 ||
			    v > MAX_READ)
				return -1;
			op->max = v;
		}
		if (nw > 3)
			return -1;
		return 0;
	}
	if (strcmp(words[0], "d") == 0) {
		struct op *op = script_add(sc, OP_DELAY);

		if (nw != 2 || parse_uint(words[1], &op->delay_ms) < 0) {
			sc->n--;
			return -1;
		}
		return 0;
	}
	if (strcmp(words[0], "f") == 0)
		return nw == 1 ? (script_add(sc, OP_FLUSH), 0) : -1;

	return -1;
}

/* ---------- tty setup ---------- */

static int open_tty(const char *dev, unsigned baud)
{
	struct termios tio;
	speed_t code;
	int fd;

	if (baud_code(baud, &code) < 0) {
		fprintf(stderr, "gpsboot: unsupported baud %u\n", baud);
		return -1;
	}
	fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (tcgetattr(fd, &tio) < 0) {
		fprintf(stderr, "gpsboot: tcgetattr(%s): %s\n", dev,
			strerror(errno));
		close(fd);
		return -1;
	}
	cfmakeraw(&tio);
	tio.c_cflag |= (CLOCAL | CREAD);
	tio.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);	/* 8N1, no flow ctl */
	tio.c_cflag &= ~CSIZE;
	tio.c_cflag |= CS8;
	tio.c_iflag &= ~(IXON | IXOFF | IXANY);
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	if (cfsetispeed(&tio, code) < 0 || cfsetospeed(&tio, code) < 0) {
		fprintf(stderr, "gpsboot: cannot set baud %u: %s\n", baud,
			strerror(errno));
		close(fd);
		return -1;
	}
	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		fprintf(stderr, "gpsboot: tcsetattr(%s): %s\n", dev,
			strerror(errno));
		close(fd);
		return -1;
	}
	tcflush(fd, TCIOFLUSH);
	return fd;
}

/* ---------- execution ---------- */

static void dump_hex(const unsigned char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		printf("%s%02x", i ? " " : "", b[i]);
	fflush(stdout);
}

static int run_read(int fd, unsigned timeout_ms, size_t max)
{
	unsigned char *buf = malloc(max);
	size_t total = 0;
	long long deadline = now_ms() + timeout_ms;
	int got_any = 0;

	if (!buf) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		long long left = deadline - now_ms();
		ssize_t n;
		int rc;

		if (left <= 0 || total >= max)
			break;
		rc = poll(&pfd, 1, (int)left);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return -1;
		}
		if (rc == 0)
			break;	/* window elapsed */
		n = read(fd, buf + total, max - total);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			free(buf);
			return -1;
		}
		if (n == 0)
			continue;
		total += (size_t)n;
		got_any = 1;
		stamp();
		printf("R %zd: ", n);
		dump_hex(buf + total - n, (size_t)n);
		putchar('\n');
	}
	if (!got_any) {
		stamp();
		printf("R 0: (timeout %u ms)\n", timeout_ms);
	}
	free(buf);
	return 0;
}

static int run_script(int fd, const struct script *sc)
{
	size_t i;

	for (i = 0; i < sc->n; i++) {
		const struct op *op = &sc->ops[i];

		switch (op->kind) {
		case OP_DELAY:
			stamp();
			printf("D %u ms\n", op->delay_ms);
			usleep(op->delay_ms * 1000u);
			break;
		case OP_WRITE:
			if (op->delay_ms) {
				stamp();
				printf("D %u ms\n", op->delay_ms);
				usleep(op->delay_ms * 1000u);
			}
			stamp();
			printf("W %zu: ", op->len);
			dump_hex(op->buf, op->len);
			putchar('\n');
			{
				size_t off = 0;

				while (off < op->len) {
					ssize_t n = write(fd, op->buf + off,
							  op->len - off);

					if (n < 0) {
						if (errno == EINTR ||
						    errno == EAGAIN) {
							struct pollfd pfd = {
								.fd = fd,
								.events = POLLOUT
							};

							poll(&pfd, 1, 1000);
							continue;
						}
						stamp();
						fprintf(stderr,
							"write failed: %s\n",
							strerror(errno));
						return -1;
					}
					off += (size_t)n;
				}
			}
			if (tcdrain(fd) < 0) {
				stamp();
				fprintf(stderr, "tcdrain: %s\n",
					strerror(errno));
				return -1;
			}
			break;
		case OP_READ:
			if (run_read(fd, op->timeout_ms, op->max) < 0) {
				stamp();
				fprintf(stderr, "read failed: %s\n",
					strerror(errno));
				return -1;
			}
			break;
		case OP_FLUSH:
			stamp();
			printf("F\n");
			tcflush(fd, TCIFLUSH);
			break;
		}
	}
	return 0;
}

/* ---------- selftest ---------- */

static int selftest(void)
{
	static const struct {
		const char *arg;
		enum op_kind kind;
		unsigned delay;
		size_t len;
	} argv_cases[] = {
		{ "w0:24000000", OP_WRITE, 0, 4 },
		{ "w100:24 00", OP_WRITE, 100, 2 },
		{ "r200", OP_READ, 0, 0 },
		{ "r200:64", OP_READ, 0, 0 },
		{ "d50", OP_DELAY, 50, 0 },
		{ "f", OP_FLUSH, 0, 0 },
	};
	static const char script[] =
		"# bring-up probe\n"
		"w 100 24 00 00\n"
		"r 500 1024\n"
		"d 10\n"
		"f\n"
		"\n"
		"   # indented comment\n"
		"r 250\n";
	struct script sc = { 0 };
	size_t i, nargv = sizeof(argv_cases) / sizeof(argv_cases[0]);
	int failures = 0;

	for (i = 0; i < nargv; i++) {
		if (parse_argv_step(argv_cases[i].arg, &sc) < 0) {
			fprintf(stderr, "selftest: failed to parse '%s'\n",
				argv_cases[i].arg);
			failures++;
			continue;
		}
		{
			const struct op *op = &sc.ops[sc.n - 1];

			if (op->kind != argv_cases[i].kind ||
			    op->delay_ms != argv_cases[i].delay ||
			    op->len != argv_cases[i].len) {
				fprintf(stderr,
					"selftest: '%s' parsed as kind=%d delay=%u len=%zu\n",
					argv_cases[i].arg, (int)op->kind,
					op->delay_ms, op->len);
				failures++;
			}
		}
	}
	sc.n = 0;

	{
		char *copy = strdup(script), *save = NULL, *line;
		unsigned lineno = 0;

		if (!copy) {
			fprintf(stderr, "selftest: out of memory\n");
			return 1;
		}
		for (line = strtok_r(copy, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			lineno++;
			if (parse_line(line, &sc) < 0) {
				fprintf(stderr,
					"selftest: stdin line %u failed: '%s'\n",
					lineno, line);
				failures++;
			}
		}
		free(copy);
	}
	if (sc.n != 5) {
		fprintf(stderr, "selftest: stdin script gave %zu ops, want 5\n",
			sc.n);
		failures++;
	}
	if (sc.n >= 1 && (sc.ops[0].kind != OP_WRITE || sc.ops[0].len != 3 ||
			  sc.ops[0].delay_ms != 100)) {
		fprintf(stderr, "selftest: op0 wrong\n");
		failures++;
	}
	if (sc.n >= 2 && (sc.ops[1].kind != OP_READ ||
			  sc.ops[1].timeout_ms != 500 ||
			  sc.ops[1].max != 1024)) {
		fprintf(stderr, "selftest: op1 wrong\n");
		failures++;
	}
	if (sc.n >= 3 && sc.ops[2].kind != OP_DELAY) {
		fprintf(stderr, "selftest: op2 wrong\n");
		failures++;
	}
	if (sc.n >= 4 && sc.ops[3].kind != OP_FLUSH) {
		fprintf(stderr, "selftest: op3 wrong\n");
		failures++;
	}
	if (sc.n >= 5 && (sc.ops[4].kind != OP_READ ||
			  sc.ops[4].timeout_ms != 250 || sc.ops[4].max != 0)) {
		fprintf(stderr, "selftest: op4 wrong\n");
		failures++;
	}
	/* Defaults and rejects. */
	if (parse_argv_step("r", &sc) < 0 ||
	    sc.ops[sc.n - 1].timeout_ms != 0 ||
	    sc.ops[sc.n - 1].max != 0) {
		fprintf(stderr, "selftest: bare 'r' should defer to -t/-m\n");
		failures++;
	}
	resolve_defaults(&sc, DEFAULT_RCV_TIMEOUT, 4096);
	if (sc.ops[sc.n - 1].kind != OP_READ ||
	    sc.ops[sc.n - 1].timeout_ms != DEFAULT_RCV_TIMEOUT ||
	    sc.ops[sc.n - 1].max != 4096) {
		fprintf(stderr, "selftest: resolve_defaults wrong\n");
		failures++;
	}
	if (sc.ops[1].timeout_ms != 500 || sc.ops[1].max != 1024) {
		fprintf(stderr, "selftest: resolve_defaults clobbered "
			"explicit values\n");
		failures++;
	}
	if (parse_argv_step("w:ff00", &sc) < 0 ||
	    sc.ops[sc.n - 1].len != 2 || sc.ops[sc.n - 1].delay_ms != 0) {
		fprintf(stderr, "selftest: 'w:ff00' wrong\n");
		failures++;
	}
	{
		size_t before = sc.n;

		if (parse_argv_step("w0:abc", &sc) >= 0 ||
		    parse_argv_step("q1", &sc) >= 0 ||
		    parse_argv_step("r:0", &sc) >= 0) {
			fprintf(stderr,
				"selftest: rejected input was accepted\n");
			failures++;
		}
		if (sc.n != before) {
			fprintf(stderr,
				"selftest: rejected input leaked ops\n");
			failures++;
		}
	}

	for (i = 0; i < sc.n; i++)
		free(sc.ops[i].buf);
	free(sc.ops);

	if (failures) {
		printf("selftest: FAIL (%d)\n", failures);
		return 1;
	}
	printf("selftest: ok (%zu argv cases + stdin script)\n", nargv);
	printf("selftest: tty not opened, no bring-up performed\n");
	return 0;
}

/* ---------- main ---------- */

static void usage(FILE *f)
{
	fprintf(f,
		"usage: gpsboot [-d DEV] [-s BAUD] [-t MS] [-m MAX] [--stdin] OP...\n"
		"       gpsboot --selftest\n"
		"\n"
		"Scripted serial helper: opt %s @ %d baud, raw 8N1.\n"
		"  OP on argv:  w<delay_ms>:<hex>   write bytes after a delay\n"
		"               r<timeout_ms>[:<max>]  read for a window\n"
		"               d<ms>              sleep\n"
		"               f                  flush pending input\n"
		"  OP on stdin (--stdin):  'w <delay_ms> <hex...>', 'r <ms> [max]',\n"
		"               'd <ms>', 'f'; '#' starts a comment.\n"
		"  -d DEV  tty device (default %s)\n"
		"  -s BAUD baud rate (default %d)\n"
		"  -t MS   default read timeout (default %d)\n"
		"  -m MAX  default read cap in bytes (default 4096)\n"
		"  --selftest  parse-sample the op parser, never open the tty\n",
		DEFAULT_DEV, DEFAULT_BAUD, DEFAULT_DEV, DEFAULT_BAUD,
		DEFAULT_RCV_TIMEOUT);
}

int main(int argc, char **argv)
{
	const char *dev = DEFAULT_DEV;
	unsigned baud = DEFAULT_BAUD;
	unsigned def_timeout = DEFAULT_RCV_TIMEOUT;
	size_t def_max = 4096;
	int use_stdin = 0;
	struct script sc = { 0 };
	int fd, i, rc;
	char *line = NULL;
	size_t linecap = 0;

	t0 = now_ms();
	/* Timestamps are only useful if they reach a log file promptly. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "--selftest") == 0)
			return selftest();
		if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
			usage(stdout);
			return 0;
		}
		if (strcmp(a, "--stdin") == 0) {
			use_stdin = 1;
			continue;
		}
		if (strcmp(a, "-d") == 0 && i + 1 < argc) {
			dev = argv[++i];
			continue;
		}
		if (strcmp(a, "-s") == 0 && i + 1 < argc) {
			if (parse_uint(argv[++i], &baud) < 0) {
				fprintf(stderr, "gpsboot: bad baud '%s'\n",
					argv[i]);
				return 2;
			}
			continue;
		}
		if (strcmp(a, "-t") == 0 && i + 1 < argc) {
			if (parse_uint(argv[++i], &def_timeout) < 0) {
				fprintf(stderr, "gpsboot: bad timeout '%s'\n",
					argv[i]);
				return 2;
			}
			continue;
		}
		if (strcmp(a, "-m") == 0 && i + 1 < argc) {
			unsigned v;

			if (parse_uint(argv[++i], &v) < 0 || v == 0 ||
			    v > MAX_READ) {
				fprintf(stderr, "gpsboot: bad max '%s'\n",
					argv[i]);
				return 2;
			}
			def_max = v;
			continue;
		}
		if (a[0] == '-') {
			fprintf(stderr, "gpsboot: unknown option '%s'\n", a);
			usage(stderr);
			return 2;
		}
		if (parse_argv_step(a, &sc) < 0) {
			fprintf(stderr, "gpsboot: bad step '%s'\n", a);
			return 2;
		}
	}

	if (use_stdin) {
		unsigned lineno = 0;

		while (getline(&line, &linecap, stdin) > 0) {
			lineno++;
			if (parse_line(line, &sc) < 0) {
				fprintf(stderr,
					"gpsboot: stdin line %u: bad step: %s",
					lineno, line);
				return 2;
			}
		}
		free(line);
	}

	if (sc.n == 0) {
		fprintf(stderr, "gpsboot: no steps given (try --help)\n");
		return 2;
	}
	resolve_defaults(&sc, def_timeout, def_max);

	fd = open_tty(dev, baud);
	if (fd < 0)
		die("gpsboot: cannot open %s (raw %u baud): check the device node and permissions", dev);
	stamp();
	printf("OPEN %s @ %u 8N1 raw, %zu steps\n", dev, baud, sc.n);

	rc = run_script(fd, &sc);
	close(fd);
	for (i = 0; i < (int)sc.n; i++)
		free(sc.ops[i].buf);
	free(sc.ops);
	stamp();
	printf("%s\n", rc == 0 ? "DONE" : "FAILED");
	return rc == 0 ? 0 : 1;
}
