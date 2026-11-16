/* nura-shell - minimal phone shell for j7elte (720x1280 fbdev, no GL, no WM).
 *
 * Draws a status bar and a tile grid with Cairo on an override-redirect X
 * window, reads the touchscreen straight from evdev, launches apps as child
 * processes and uses the gpio Home key to close the child and raise itself.
 *
 * Build: gcc -O2 -o nura-shell nura-shell.c $(pkg-config --cflags --libs cairo x11)
 */
#define _GNU_SOURCE
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define W 720
#define H 1280
#define STATUS_H 72
#define COLS 2
#define ROWS 3

struct app {
	const char *label;
	const char *cmd;
	const char *accent;
	pid_t pid;
};

static struct app apps[] = {
	{ "Terminal", "qterminal", "#3b82f6", -1 },
	{ "Files",    "pcmanfm-qt", "#22c55e", -1 },
	{ "Keyboard", "matchbox-keyboard", "#a855f7", -1 },
	{ "Settings", "lxqt-config", "#f59e0b", -1 },
	{ "Camera",   "megapixels", "#ef4444", -1 },
	{ "Reboot",   "reboot", "#64748b", -1 },
};

static Display *dpy;
static Window win;
static cairo_surface_t *sfc;
static cairo_t *cr;
static GC gc;
static Visual *vis;
static int depth;
static int touch_fd = -1;
static int key_fd = -1;
static int touch_x = -1, touch_y = -1;	/* current finger, device coords */
static int touch_down;
static int pressed = -1;
static int child_running;
static pid_t child = -1;

static void rgba(cairo_t *c, const char *hex)
{
	int r = 0, g = 0, b = 0;
	sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
	cairo_set_source_rgb(c, r / 255.0, g / 255.0, b / 255.0);
}

static void rounded(cairo_t *c, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(c);
	cairo_arc(c, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(c, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(c, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(c, x + r, y + r, r, M_PI, 1.5 * M_PI);
	cairo_close_path(c);
}

static int battery(void)
{
	static const char *p = "/sys/class/power_supply/battery/capacity";
	FILE *f = fopen(p, "r");
	int v = -1;
	if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
	return v;
}

static void draw(void)
{
	char buf[64];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);

	rgba(cr, "#0f1115");
	cairo_paint(cr);

	/* status bar */
	rgba(cr, "#161a20");
	cairo_rectangle(cr, 0, 0, W, STATUS_H);
	cairo_fill(cr);
	strftime(buf, sizeof buf, "%H:%M", &tm);
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 34);
	rgba(cr, "#e5e7eb");
	cairo_move_to(cr, 28, 48);
	cairo_show_text(cr, buf);

	int b = battery();
	snprintf(buf, sizeof buf, "%d%%", b);
	cairo_set_font_size(cr, 28);
	cairo_text_extents_t te;
	cairo_text_extents(cr, buf, &te);
	cairo_move_to(cr, W - 28 - te.x_advance, 46);
	cairo_show_text(cr, buf);

	/* tile grid */
	double pad = 24, gap = 20;
	double tw = (W - 2 * pad - gap * (COLS - 1)) / COLS;
	double th = 240;
	double top = STATUS_H + 32;
	for (int i = 0; i < (int)(sizeof apps / sizeof apps[0]); i++) {
		int c = i % COLS, r = i / COLS;
		double x = pad + c * (tw + gap);
		double y = top + r * (th + gap);
		rgba(cr, i == pressed ? "#2a313c" : "#1b2028");
		rounded(cr, x, y, tw, th, 28);
		cairo_fill(cr);
		rgba(cr, apps[i].accent);
		cairo_arc(cr, x + 52, y + 56, 18, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
				       CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 30);
		rgba(cr, "#e8eaed");
		cairo_move_to(cr, x + 28, y + 150);
		cairo_show_text(cr, apps[i].label);
		cairo_set_font_size(cr, 22);
		rgba(cr, "#8b93a1");
		cairo_move_to(cr, x + 28, y + 190);
		cairo_show_text(cr, apps[i].cmd);
	}

	rgba(cr, "#7c8492");
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 22);
	cairo_move_to(cr, 28, H - 28);
	cairo_show_text(cr, "Home key = back to launcher");

	cairo_surface_flush(sfc);
	{
		int stride = cairo_image_surface_get_stride(sfc);
		XImage *xi = XCreateImage(dpy, vis, depth, ZPixmap, 0,
					  (char *)cairo_image_surface_get_data(sfc),
					  W, H, 32, stride);
		if (xi) {
			/* CAIRO_FORMAT_RGB24 is BGRX in memory; the visual is BGR
			 * (red=0xff), so declare the image masks to match. */
			xi->red_mask = 0xff0000;
			xi->green_mask = 0xff00;
			xi->blue_mask = 0xff;
			XPutImage(dpy, win, gc, xi, 0, 0, 0, 0, W, H);
			xi->data = NULL;	/* data belongs to cairo */
			XDestroyImage(xi);
		}
	}
	XFlush(dpy);
}

static void launch(int i)
{
	if (child > 0)
		return;
	child = fork();
	if (child == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", apps[i].cmd, (char *)NULL);
		_exit(127);
	}
	if (child > 0) {
		apps[i].pid = child;
		child_running = 1;
	}
}

static void close_child(void)
{
	if (child > 0) {
		kill(-child, SIGTERM);
		kill(child, SIGTERM);
	}
	child = -1;
	child_running = 0;
	XRaiseWindow(dpy, win);
	draw();
}

static int hit(double x, double y)
{
	double pad = 24, gap = 20;
	double tw = (W - 2 * pad - gap * (COLS - 1)) / COLS;
	double th = 240, top = STATUS_H + 32;
	for (int i = 0; i < (int)(sizeof apps / sizeof apps[0]); i++) {
		int c = i % COLS, r = i / COLS;
		double tx = pad + c * (tw + gap), ty = top + r * (th + gap);
		if (x >= tx && x <= tx + tw && y >= ty && y <= ty + th)
			return i;
	}
	return -1;
}

static void handle_input(int fd)
{
	if (fd == touch_fd) {
		struct input_event ev;
		while (read(fd, &ev, sizeof ev) == sizeof ev) {
			if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X)
				touch_x = ev.value;
			if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y)
				touch_y = ev.value;
			if (ev.type == EV_ABS && ev.code == ABS_X)
				touch_x = ev.value;
			if (ev.type == EV_ABS && ev.code == ABS_Y)
				touch_y = ev.value;
			if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
				if (ev.value) {
					touch_down = 1;
					pressed = hit(touch_x, touch_y);
					draw();
				} else if (touch_down) {
					touch_down = 0;
					int a = hit(touch_x, touch_y);
					if (a >= 0 && a == pressed)
						launch(a);
					pressed = -1;
					draw();
				}
			}
		}
	} else if (fd == key_fd) {
		struct input_event ev;
		while (read(fd, &ev, sizeof ev) == sizeof ev) {
			if (ev.type == EV_KEY && ev.value == 1 &&
			    (ev.code == KEY_HOMEPAGE || ev.code == KEY_BACK ||
			     ev.code == KEY_MENU))
				close_child();
		}
	}
}

int main(void)
{
	dpy = XOpenDisplay(NULL);
	if (!dpy) { fprintf(stderr, "no display\n"); return 1; }

	XSetWindowAttributes at;
	at.override_redirect = True;
	at.background_pixel = 0;
	win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, W, H, 0,
			    DefaultDepth(dpy, DefaultScreen(dpy)), InputOutput,
			    DefaultVisual(dpy, DefaultScreen(dpy)),
			    CWOverrideRedirect | CWBackPixel, &at);
	XStoreName(dpy, win, "nura-shell");
	vis = DefaultVisual(dpy, DefaultScreen(dpy));
	depth = DefaultDepth(dpy, DefaultScreen(dpy));
	sfc = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
	cr = cairo_create(sfc);
	gc = XCreateGC(dpy, win, 0, NULL);

	XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
	XMapRaised(dpy, win);

	touch_fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
	key_fd = open("/dev/input/event5", O_RDONLY | O_NONBLOCK);

	signal(SIGCHLD, SIG_IGN);
	draw();

	for (;;) {
		fd_set fds;
		FD_ZERO(&fds);
		int maxfd = ConnectionNumber(dpy);
		FD_SET(maxfd, &fds);
		if (touch_fd >= 0) { FD_SET(touch_fd, &fds); if (touch_fd > maxfd) maxfd = touch_fd; }
		if (key_fd >= 0) { FD_SET(key_fd, &fds); if (key_fd > maxfd) maxfd = key_fd; }
		struct timeval tv = { 1, 0 };
		int r = select(maxfd + 1, &fds, NULL, NULL, &tv);
		if (r > 0) {
			while (XPending(dpy)) {
				XEvent ev;
				XNextEvent(dpy, &ev);
				if (ev.type == Expose) draw();
			}
			if (touch_fd >= 0 && FD_ISSET(touch_fd, &fds))
				handle_input(touch_fd);
			if (key_fd >= 0 && FD_ISSET(key_fd, &fds))
				handle_input(key_fd);
		} else if (r == 0) {
			draw();		/* clock tick */
		}
	}
}
