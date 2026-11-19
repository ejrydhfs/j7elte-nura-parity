/*
 * gpsprobe - raw serial experiment driver for the BCM4752 on j7elte
 *
 * Rationale: the chip's bring-up is a byte-level exchange, so the only way
 * to iterate on device is a tool that can (a) set an arbitrary 8N1 line,
 * (b) emit arbitrary byte sequences with explicit pacing, and (c) record
 * everything the chip says in a hexdump.  This is the "scope" we have.
 *
 * Usage:
 *   gpsprobe [-d DEV] [-b BAUD] OP [OP ...]
 *
 * Ops:
 *   w:HEX        write raw bytes (whitespace/colons in HEX ignored)
 *   s:MS         sleep
 *   r:MS         read for MS milliseconds, hexdump everything
 *   f            tcflush(TCIFLUSH)
 *   P:0|1        write 0|1 to the GPS power-enable sysfs node
 *   B:N          switch baud to N (re-apply termios without closing)
 *   Q            quit
 *
 * Exit code 0 always (this is a probe, not a test); the transcript is the
 * result.  Build: cc -O2 -o gpsprobe gpsprobe.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static const char *dev = "/dev/ttySAC0";
static const char *pwr = "/sys/class/sec/gps/GPS_PWR_EN/value";
static speed_t baud = B921600;
static long baud_num = 921600;
static int hwfc;

static speed_t num2speed(long n)
{
	switch (n) {
	case 1200: return B1200;
	case 2400: return B2400;
	case 4800: return B4800;
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 500000: return B500000;
	case 576000: return B576000;
	case 921600: return B921600;
	case 1000000: return B1000000;
	case 1500000: return B1500000;
	case 2000000: return B2000000;
	case 3000000: return B3000000;
	case 4000000: return B4000000;
	default: return (speed_t)-1;
	}
}

static int set_line(int fd, speed_t sp)
{
	struct termios t;

	if (tcgetattr(fd, &t) < 0)
		return -1;
	cfmakeraw(&t);
	t.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | PARODD | CSIZE);
	if (!hwfc)
		t.c_cflag &= ~(tcflag_t)CRTSCTS;
	t.c_cflag |= CS8 | CLOCAL | CREAD;
	t.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	cfsetispeed(&t, sp);
	cfsetospeed(&t, sp);
	return tcsetattr(fd, TCSANOW, &t);
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static size_t parse_hex(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
	int hi = -1;

	for (; *s && n < max; s++) {
		int v;
		if (*s == ' ' || *s == ':' || *s == '-' || *s == '_')
			continue;
		v = hexval((unsigned char)*s);
		if (v < 0) {
			fprintf(stderr, "bad hex char '%c'\n", *s);
			exit(2);
		}
		if (hi < 0) {
			hi = v;
		} else {
			out[n++] = (uint8_t)((hi << 4) | v);
			hi = -1;
		}
	}
	if (hi >= 0) {
		fprintf(stderr, "odd hex digit count\n");
		exit(2);
	}
	return n;
}

static int read_for(int fd, long ms)
{
	uint8_t buf[512];
	long end = now_ms() + ms;
	size_t total = 0;
	long t0 = now_ms();

	while (now_ms() < end) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		long left = end - now_ms();
		int rv = poll(&p, 1, left > 0 ? (int)left : 0);
		ssize_t n;

		if (rv < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return -1;
		}
		if (rv == 0)
			break;
		n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			perror("read");
			return -1;
		}
		if (n == 0)
			continue;
		printf("  rx @%ldms %zd B:", now_ms() - t0, n);
		for (ssize_t i = 0; i < n; i++)
			printf(" %02x", buf[i]);
		printf("   |");
		for (ssize_t i = 0; i < n; i++) {
			unsigned c = buf[i];
			putchar(c >= 0x20 && c < 0x7f ? (int)c : '.');
		}
		printf("|\n");
		total += (size_t)n;
	}
	if (total == 0)
		printf("  rx @%ldms: (no bytes in %ld ms)\n", now_ms() - t0, ms);
	return (int)total;
}

int main(int argc, char **argv)
{
	int fd, i = 1;
	int power_fd = -1;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s [-d DEV] [-b BAUD] [-p POWER_PATH] [-k] "
			"w:HEX|s:MS|r:MS|f|P:0|1|B:N|C:0|1|R:a|d|D:a|d ...\n",
			argv[0]);
		return 2;
	}
	/* "keep power off": do not touch the rail at all (default is also
	 * untouched unless a P: op appears) */
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			dev = argv[++i];
		} else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
			baud_num = strtol(argv[++i], NULL, 0);
			baud = num2speed(baud_num);
			if (baud == (speed_t)-1) {
				fprintf(stderr, "unsupported baud\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
			pwr = argv[++i];
		} else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}

	fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		perror(dev);
		return 1;
	}
	if (set_line(fd, baud) < 0) {
		perror("tcsetattr");
		return 1;
	}
	printf("gpsprobe: %s 8N1 raw, baud=%ld, ops=%d\n", dev, baud_num,
	       argc - i);

	for (; i < argc; i++) {
		const char *op = argv[i];

		if (!strncmp(op, "w:", 2)) {
			uint8_t b[1024];
			size_t n = parse_hex(op + 2, b, sizeof(b));
			ssize_t w = write(fd, b, n);
			printf("tx %zu B -> %zd", n, w);
			if (w < 0)
				printf(" (%s)", strerror(errno));
			printf("\n");
			if (w < 0)
				return 1;
			tcdrain(fd);
		} else if (!strncmp(op, "s:", 2)) {
			long ms = strtol(op + 2, NULL, 0);
			printf("sleep %ld ms\n", ms);
			usleep((useconds_t)ms * 1000);
		} else if (!strncmp(op, "r:", 2)) {
			long ms = strtol(op + 2, NULL, 0);
			if (read_for(fd, ms) < 0)
				return 1;
		} else if (!strcmp(op, "f")) {
			tcflush(fd, TCIFLUSH);
			printf("flush\n");
		} else if (!strncmp(op, "P:", 2)) {
			if (power_fd < 0)
				power_fd = open(pwr, O_WRONLY);
			if (power_fd < 0) {
				perror(pwr);
				return 1;
			}
			if (write(power_fd, op + 2, 1) != 1) {
				perror("power write");
				return 1;
			}
			printf("power <- %c\n", op[2]);
		} else if (!strncmp(op, "C:", 2)) {
			hwfc = op[2] == '1';
			if (!hwfc) {
				/* clear AFC explicitly, like a fresh termios */
				struct termios t;
				tcgetattr(fd, &t);
				t.c_cflag &= ~(tcflag_t)CRTSCTS;
				tcsetattr(fd, TCSANOW, &t);
			}
			if (set_line(fd, baud) < 0) {
				perror("set_line");
				return 1;
			}
			printf("crtscts=%d\n", hwfc);
		} else if (!strncmp(op, "R:", 2)) {
			int bits = op[2] == 'a' ? TIOCMBIS : TIOCMBIC;
			int m = TIOCM_RTS;
			if (ioctl(fd, bits, &m) < 0)
				perror("TIOCM RTS");
			else
				printf("RTS %s\n", op[2] == 'a' ? "assert" : "deassert");
		} else if (!strncmp(op, "D:", 2)) {
			int bits = op[2] == 'a' ? TIOCMBIS : TIOCMBIC;
			int m = TIOCM_DTR;
			if (ioctl(fd, bits, &m) < 0)
				perror("TIOCM DTR");
			else
				printf("DTR %s\n", op[2] == 'a' ? "assert" : "deassert");
		} else if (!strncmp(op, "B:", 2)) {
			speed_t sp = num2speed(strtol(op + 2, NULL, 0));
			if (sp == (speed_t)-1 || set_line(fd, sp) < 0) {
				perror("baud switch");
				return 1;
			}
			printf("baud -> %s\n", op + 2);
		} else {
			fprintf(stderr, "bad op %s\n", op);
			return 2;
		}
	}
	close(fd);
	return 0;
}
