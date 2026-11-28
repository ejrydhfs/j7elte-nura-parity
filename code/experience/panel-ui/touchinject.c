#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <sys/ioctl.h>

static int fd;

static void emit(int type, int code, int val)
{
	struct input_event e;
	memset(&e, 0, sizeof e);
	e.type = type;
	e.code = code;
	e.value = val;
	write(fd, &e, sizeof e);
}

static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

int main(void)
{
	struct uinput_user_dev uidev;
	int i;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) { perror("open uinput"); return 1; }

	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
	ioctl(fd, UI_SET_ABSBIT, ABS_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_Y);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

	memset(&uidev, 0, sizeof uidev);
	snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "virt-touch");
	uidev.id.bustype = BUS_VIRTUAL;
	uidev.absmin[ABS_X] = 0; uidev.absmax[ABS_X] = 719;
	uidev.absmin[ABS_Y] = 0; uidev.absmax[ABS_Y] = 1279;
	uidev.absmin[ABS_MT_POSITION_X] = 0; uidev.absmax[ABS_MT_POSITION_X] = 719;
	uidev.absmin[ABS_MT_POSITION_Y] = 0; uidev.absmax[ABS_MT_POSITION_Y] = 1279;
	write(fd, &uidev, sizeof uidev);
	if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return 1; }
	usleep(800000);

	/* Swipe down from the top edge: reveals Plasma Mobile panels. */
	emit(EV_ABS, ABS_MT_SLOT, 0);
	emit(EV_ABS, ABS_MT_TRACKING_ID, 1);
	emit(EV_KEY, BTN_TOUCH, 1);
	for (i = 0; i <= 40; i++) {
		int y = 5 + (395 * i) / 40;
		emit(EV_ABS, ABS_MT_POSITION_X, 360);
		emit(EV_ABS, ABS_MT_POSITION_Y, y);
		emit(EV_ABS, ABS_X, 360);
		emit(EV_ABS, ABS_Y, y);
		syn();
		usleep(12000);
	}
	emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
	emit(EV_KEY, BTN_TOUCH, 0);
	syn();
	usleep(300000);

	/* Tap in the middle. */
	emit(EV_ABS, ABS_MT_SLOT, 0);
	emit(EV_ABS, ABS_MT_TRACKING_ID, 2);
	emit(EV_KEY, BTN_TOUCH, 1);
	emit(EV_ABS, ABS_MT_POSITION_X, 360);
	emit(EV_ABS, ABS_MT_POSITION_Y, 640);
	emit(EV_ABS, ABS_X, 360);
	emit(EV_ABS, ABS_Y, 640);
	syn();
	usleep(80000);
	emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
	emit(EV_KEY, BTN_TOUCH, 0);
	syn();

	usleep(200000);
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	printf("swipe done\n");
	return 0;
}
