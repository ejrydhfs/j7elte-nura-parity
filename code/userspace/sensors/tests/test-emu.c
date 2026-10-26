/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test-emu.c - TEST ONLY uinput emulator for j7elte-sensors.
 *
 * Creates two virtual input devices with the same names and capabilities as
 * the j7elte kernel drivers, then accepts commands from a FIFO:
 *
 *   accel <x> <y> <z>   emit REL_X/Y/Z plus SYN_REPORT (raw K2HH LSB, 8192/g)
 *   prox <0|1>          emit ABS_DISTANCE (0 = close, 1 = far)
 *   quit
 *
 * This exists only to exercise the j7elte-sensors evdev/D-Bus paths on a
 * development host.  It is not installed and must never be used to feed the
 * daemon on real hardware.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

static void emit(int fd, int type, int code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		perror("write event");
}

static int create_uinput(const char *name, int is_abs)
{
	struct uinput_setup setup;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		perror("open /dev/uinput");
		return -1;
	}

	if (is_abs) {
		ioctl(fd, UI_SET_EVBIT, EV_ABS);
		ioctl(fd, UI_SET_ABSBIT, ABS_DISTANCE);
	} else {
		ioctl(fd, UI_SET_EVBIT, EV_REL);
		ioctl(fd, UI_SET_RELBIT, REL_X);
		ioctl(fd, UI_SET_RELBIT, REL_Y);
		ioctl(fd, UI_SET_RELBIT, REL_Z);
		ioctl(fd, UI_SET_RELBIT, REL_DIAL);
		ioctl(fd, UI_SET_RELBIT, REL_MISC);
	}

	memset(&setup, 0, sizeof(setup));
	snprintf(setup.name, sizeof(setup.name), "%s", name);
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1234;
	setup.id.product = 0x5678;
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
		perror("UI_DEV_SETUP");
		close(fd);
		return -1;
	}
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("UI_DEV_CREATE");
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	const char *fifo = argc > 1 ? argv[1] : "/tmp/j7elte-sensors-test.fifo";
	int accel_fd, prox_fd, fifo_fd;
	struct sigaction sa = { .sa_handler = on_sig };

	if (argc > 2 && !strcmp(argv[2], "--help")) {
		fprintf(stderr, "see source header for usage\n");
		return 0;
	}

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	accel_fd = create_uinput("accelerometer_sensor", 0);
	prox_fd = create_uinput("proximity_sensor", 1);
	if (accel_fd < 0 || prox_fd < 0)
		return 1;

	/* O_RDWR keeps the FIFO from reporting EOF when no test writer is open. */
	fifo_fd = open(fifo, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fifo_fd < 0) {
		perror("open fifo (mkfifo first)");
		return 1;
	}

	fprintf(stderr, "test-emu: devices created, waiting on %s\n", fifo);
	{
		char buf[1024];
		size_t used = 0;

		while (!stop) {
			struct pollfd pfd = { .fd = fifo_fd, .events = POLLIN };
			ssize_t n;
			char *nl;

			if (poll(&pfd, 1, 500) <= 0)
				continue;
			n = read(fifo_fd, buf + used, sizeof(buf) - 1 - used);
			if (n <= 0)
				continue;
			used += (size_t)n;
			buf[used] = '\0';

			while ((nl = strchr(buf, '\n')) != NULL) {
				char *cmd, *save = NULL;
				int x, y, z;

				*nl = '\0';
				cmd = strtok_r(buf, " ", &save);
				if (cmd && !strcmp(cmd, "accel") &&
				    sscanf(save ? save : "", "%d %d %d",
					   &x, &y, &z) == 3) {
					emit(accel_fd, EV_REL, REL_X, x);
					emit(accel_fd, EV_REL, REL_Y, y);
					emit(accel_fd, EV_REL, REL_Z, z);
					emit(accel_fd, EV_REL, REL_DIAL, 0);
					emit(accel_fd, EV_REL, REL_MISC, 0);
					emit(accel_fd, EV_SYN, SYN_REPORT, 0);
					fprintf(stderr,
						"test-emu: accel %d %d %d\n",
						x, y, z);
				} else if (cmd && !strcmp(cmd, "prox") &&
					   sscanf(save ? save : "", "%d",
						  &x) == 1) {
					emit(prox_fd, EV_ABS, ABS_DISTANCE, x);
					emit(prox_fd, EV_SYN, SYN_REPORT, 0);
					fprintf(stderr,
						"test-emu: prox %d\n", x);
				} else if (cmd && !strcmp(cmd, "quit")) {
					stop = 1;
				}
				memmove(buf, nl + 1, used - (size_t)(nl + 1 - buf));
				used -= (size_t)(nl + 1 - buf);
				buf[used] = '\0';
			}
			if (used == sizeof(buf) - 1)
				used = 0; /* drop overlong garbage */
		}
	}

	ioctl(accel_fd, UI_DEV_DESTROY);
	ioctl(prox_fd, UI_DEV_DESTROY);
	close(accel_fd);
	close(prox_fd);
	close(fifo_fd);
	return 0;
}
