/* Minimal touch launcher for j7elte: Xlib + cairo, no GTK/Qt. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define W 720
#define H 1280
#define COLS 2
#define ROWS 3

static const char *labels[ROWS][COLS] = {
	{ "Terminal", "Keyboard" },
	{ "Wi-Fi scan", "Reboot" },
	{ "Poweroff", "About" },
};
static const char *cmds[ROWS][COLS] = {
	{ "xterm -geometry 80x40+0+0 -bg white -fg black",
	  "matchbox-keyboard" },
	{ "xterm -e 'nmcli device wifi list; sleep 30'",
	  "sh -c \"echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger\"" },
	{ "sh -c \"echo 1 > /proc/sys/kernel/sysrq; echo o > /proc/sysrq-trigger\"", "xterm -e 'uname -a; ip -4 a; sleep 30'" },
};

static Display *dpy;
static Window win;
static cairo_surface_t *surf;

static void draw(void)
{
	cairo_t *cr = cairo_create(surf);
	int x, y, w = W / COLS, h = (H - 120) / ROWS;

	cairo_set_source_rgb(cr, 0.05, 0.05, 0.07);
	cairo_paint(cr);

	cairo_select_font_face(cr, "Droid Sans", CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 34);
	cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
	cairo_move_to(cr, 24, 48);
	cairo_show_text(cr, "Nura - j7elte");

	for (y = 0; y < ROWS; y++) {
		for (x = 0; x < COLS; x++) {
			double bx = x * w + 12, by = 80 + y * h + 12;
			double bw = w - 24, bh = h - 24;

			cairo_set_source_rgb(cr, 0.13, 0.16, 0.22);
			cairo_rectangle(cr, bx, by, bw, bh);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 0.25, 0.55, 0.90);
			cairo_set_line_width(cr, 3);
			cairo_rectangle(cr, bx, by, bw, bh);
			cairo_stroke(cr);

			cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
			cairo_set_font_size(cr, 30);
			cairo_move_to(cr, bx + 20, by + bh / 2 + 10);
			cairo_show_text(cr, labels[y][x]);
		}
	}
	cairo_destroy(cr);
	XFlush(dpy);
}

static void run(const char *cmd)
{
	pid_t p = fork();
	if (p == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", cmd, NULL);
		_exit(127);
	}
}

int main(void)
{
	dpy = XOpenDisplay(NULL);
	if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
	int scr = DefaultScreen(dpy);
	win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, W, H, 0,
				  BlackPixel(dpy, scr), BlackPixel(dpy, scr));
	XSelectInput(dpy, win, ExposureMask | ButtonPressMask | StructureNotifyMask);
	XMapWindow(dpy, win);
	surf = cairo_xlib_surface_create(dpy, win, DefaultVisual(dpy, scr), W, H);

	for (;;) {
		XEvent ev;
		XNextEvent(dpy, &ev);
		if (ev.type == Expose) {
			draw();
		} else if (ev.type == ButtonPress) {
			int x = ev.xbutton.x / (W / COLS);
			int y = (ev.xbutton.y - 80) / ((H - 120) / ROWS);
			if (y >= 0 && y < ROWS && x >= 0 && x < COLS) {
				fprintf(stderr, "tap %d,%d -> %s\n", x, y,
					cmds[y][x]);
				run(cmds[y][x]);
			}
		}
	}
	return 0;
}
