/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * j7elte-sensors - expose the Samsung Galaxy J7 (j7elte) evdev sensors
 *                  through net.hadess.SensorProxy
 *
 * The downstream kernel drivers do not use IIO:
 *   - K2HH accelerometer: input device "accelerometer_sensor",
 *     EV_REL REL_X/REL_Y/REL_Z (raw, 8192 LSB/g at the +-4g range the driver
 *     selects when enabled), plus REL_DIAL/REL_MISC timestamps.
 *     Control: <input-device>/enable and <input-device>/poll_delay.
 *   - CM36672P / STK3013 proximity: input device "proximity_sensor",
 *     EV_ABS ABS_DISTANCE (0 = close, 1 = far).
 *     Control: <input-device>/enable.
 *   - No ambient-light interface is registered by either proximity driver in
 *     this kernel (see parity/reports/sensors.md), so HasAmbientLight=false.
 *
 * The daemon speaks the D-Bus binary protocol directly (dbus-wire.c) and
 * implements the iio-sensor-proxy interface so existing desktop consumers
 * work unchanged.
 */
#define _GNU_SOURCE

#include "dbus-wire.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SENSOR_PROXY_NAME "net.hadess.SensorProxy"
#define SENSOR_PROXY_PATH "/net/hadess/SensorProxy"
#define ACCEL_INPUT_NAME "accelerometer_sensor"
#define PROX_INPUT_NAME "proximity_sensor"

/*
 * k2hh.c enables K2HH_RANGE_4G before streaming (SENSITIVITY_4G = 122 ug/LSB,
 * i.e. MAX_ACCEL_1G = 8192 LSB per g).  iio-sensor-proxy's orientation code
 * works in units of 1 g = 256, hence raw / 32.
 */
#define K2HH_LSB_PER_G 8192.0
#define ORIENT_UNITS_PER_G 256.0
#define ACCEL_POLL_DELAY_NS 100000000LL /* 10 Hz */

#define MAX_EVENT_DEV 64
#define MAX_CLAIMS 32
#define CLAIM_NAME_MAX 128

struct sensor {
	const char *kind;
	int present;
	int fd;
	int enabled;
	int has_enable;
	int has_delay;
	char evpath[64];
	char sysfs[256];
	char enable_path[320];
	char delay_path[320];
};

static struct dw_conn *bus;
static struct sensor accel_sensor = { .kind = "accelerometer", .fd = -1 };
static struct sensor prox_sensor = { .kind = "proximity", .fd = -1 };

static volatile sig_atomic_t stop_requested;
static int verbose;

/* ------------------------------------------------------------------ */
/* logging                                                            */
/* ------------------------------------------------------------------ */

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	fputs("j7elte-sensors: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void logdbg(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	fputs("j7elte-sensors: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* orientation / tilt (algorithm compatible with iio-sensor-proxy 3.9, */
/* which took it from sensorfw; GPL-3.0-or-later).                    */
/* ------------------------------------------------------------------ */

enum orientation {
	ORIENT_UNDEFINED,
	ORIENT_NORMAL,
	ORIENT_BOTTOM_UP,
	ORIENT_LEFT_UP,
	ORIENT_RIGHT_UP,
};

enum tilt {
	TILT_UNDEFINED,
	TILT_VERTICAL,
	TILT_TILTED_UP,
	TILT_TILTED_DOWN,
	TILT_FACE_UP,
	TILT_FACE_DOWN,
};

static const char *const orientation_names[] = {
	"undefined", "normal", "bottom-up", "left-up", "right-up",
};

static const char *const tilt_names[] = {
	"undefined", "vertical", "tilted-up", "tilted-down", "face-up",
	"face-down",
};

#define RADIANS_TO_DEGREES (180.0 / M_PI)
#define SAME_AXIS_LIMIT 5
#define THRESHOLD_LANDSCAPE 35
#define THRESHOLD_PORTRAIT 35
#define THRESHOLD_FACE_DOWN 70
#define THRESHOLD_TILT 20
#define TILT_MAX 90

static enum orientation orientation_calc(enum orientation prev, int x, int y,
					 int z)
{
	enum orientation ret = prev;
	int portrait_rotation, landscape_rotation;

	portrait_rotation = (int)lround(atan2(x, sqrt((double)y * y +
						     (double)z * z)) *
					RADIANS_TO_DEGREES);
	landscape_rotation = (int)lround(atan2(y, sqrt((double)x * x +
						       (double)z * z)) *
					 RADIANS_TO_DEGREES);

	/* On the common border of two thresholds, keep the previous value. */
	if (abs(portrait_rotation) > THRESHOLD_PORTRAIT &&
	    abs(landscape_rotation) > THRESHOLD_LANDSCAPE)
		return prev;

	if (abs(portrait_rotation) > THRESHOLD_PORTRAIT) {
		ret = portrait_rotation > 0 ? ORIENT_LEFT_UP : ORIENT_RIGHT_UP;
		if ((prev == ORIENT_LEFT_UP || prev == ORIENT_RIGHT_UP) &&
		    abs(portrait_rotation) < SAME_AXIS_LIMIT)
			ret = prev;
	} else if (abs(landscape_rotation) > THRESHOLD_LANDSCAPE) {
		ret = landscape_rotation > 0 ? ORIENT_BOTTOM_UP : ORIENT_NORMAL;
		if ((prev == ORIENT_BOTTOM_UP || prev == ORIENT_NORMAL) &&
		    abs(landscape_rotation) < SAME_AXIS_LIMIT)
			ret = prev;
	}

	return ret;
}

static enum tilt tilt_calc(enum tilt prev, int x, int y, int z)
{
	enum tilt ret = prev;
	int tilt;

	tilt = (int)lround(atan2(z, sqrt((double)x * x + (double)y * y)) *
			   RADIANS_TO_DEGREES);

	if (abs(tilt) <= THRESHOLD_TILT)
		ret = TILT_VERTICAL;
	else if (abs(tilt) > THRESHOLD_TILT && abs(tilt) < THRESHOLD_FACE_DOWN)
		ret = tilt > 0 ? TILT_TILTED_UP : TILT_TILTED_DOWN;
	else if (abs(tilt) >= THRESHOLD_FACE_DOWN && abs(tilt) <= TILT_MAX)
		ret = tilt > 0 ? TILT_FACE_UP : TILT_FACE_DOWN;
	else
		ret = TILT_UNDEFINED;

	return ret;
}

/* ------------------------------------------------------------------ */
/* D-Bus property state                                               */
/* ------------------------------------------------------------------ */

static enum orientation current_orientation = ORIENT_UNDEFINED;
static enum tilt current_tilt = TILT_UNDEFINED;
static int proximity_near;
/* The kernel exposes no ambient-light channel for this device. */
static const int has_ambient_light = 0;

struct claim_list {
	int count;
	char names[MAX_CLAIMS][CLAIM_NAME_MAX];
};

static struct claim_list accel_claims;
static struct claim_list prox_claims;
static struct claim_list light_claims;

/* ------------------------------------------------------------------ */
/* properties                                                         */
/* ------------------------------------------------------------------ */

static int write_sysfs(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, value, strlen(value));
	close(fd);
	return n == (ssize_t)strlen(value) ? 0 : -1;
}

struct prop_ctx {
	const char *name;
	enum { PT_BOOL, PT_STRING, PT_DOUBLE } type;
	int b;
	const char *s;
	double d;
};

static int write_prop_variant(struct dw_writer *w, const char *name)
{
	if (!strcmp(name, "HasAccelerometer"))
		dw_w_variant_bool(w, accel_sensor.present);
	else if (!strcmp(name, "AccelerometerOrientation"))
		dw_w_variant_string(w, orientation_names[current_orientation]);
	else if (!strcmp(name, "AccelerometerTilt"))
		dw_w_variant_string(w, tilt_names[current_tilt]);
	else if (!strcmp(name, "HasAmbientLight"))
		dw_w_variant_bool(w, has_ambient_light);
	else if (!strcmp(name, "LightLevelUnit"))
		dw_w_variant_string(w, "lux");
	else if (!strcmp(name, "LightLevel"))
		dw_w_variant_double(w, 0.0);
	else if (!strcmp(name, "HasProximity"))
		dw_w_variant_bool(w, prox_sensor.present);
	else if (!strcmp(name, "ProximityNear"))
		dw_w_variant_bool(w, proximity_near);
	else
		return -1;
	return 0;
}

static void fill_props_changed(struct dw_writer *w, void *ud)
{
	struct prop_ctx *ctx = ud;
	struct dw_array_marker dict, invalidated;

	dw_w_string(w, SENSOR_PROXY_NAME);

	dict = dw_w_array_begin(w, 8);
	dw_w_string(w, ctx->name);
	switch (ctx->type) {
	case PT_BOOL:
		dw_w_variant_bool(w, ctx->b);
		break;
	case PT_STRING:
		dw_w_variant_string(w, ctx->s);
		break;
	case PT_DOUBLE:
		dw_w_variant_double(w, ctx->d);
		break;
	}
	dw_w_array_end(w, dict);

	invalidated = dw_w_array_begin(w, 4); /* empty "as" */
	dw_w_array_end(w, invalidated);
}

static void emit_bool_prop(const char *name, int value)
{
	struct prop_ctx ctx = { .name = name, .type = PT_BOOL, .b = value };

	if (dw_send_signal(bus, SENSOR_PROXY_PATH,
			   "org.freedesktop.DBus.Properties",
			   "PropertiesChanged", "sa{sv}as", fill_props_changed,
			   &ctx) < 0)
		logmsg("failed to emit %s change", name);
}

static void emit_string_prop(const char *name, const char *value)
{
	struct prop_ctx ctx = { .name = name, .type = PT_STRING, .s = value };

	if (dw_send_signal(bus, SENSOR_PROXY_PATH,
			   "org.freedesktop.DBus.Properties",
			   "PropertiesChanged", "sa{sv}as", fill_props_changed,
			   &ctx) < 0)
		logmsg("failed to emit %s change", name);
}

/* ------------------------------------------------------------------ */
/* evdev discovery                                                    */
/* ------------------------------------------------------------------ */

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NBITS(x) (((x) - 1) / BITS_PER_LONG + 1)
#define TEST_BIT(bit, arr) \
	(((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static int evdev_test_bits(int fd, unsigned long request, int bit,
			   unsigned long *bits, size_t nbits)
{
	memset(bits, 0, nbits * sizeof(*bits));
	if (ioctl(fd, request, bits) < 0)
		return 0;
	return TEST_BIT(bit, bits) ? 1 : 0;
}

static int evdev_is_accel(int fd)
{
	unsigned long bits[NBITS(REL_MAX)];

	return evdev_test_bits(fd, EVIOCGBIT(EV_REL, sizeof(bits)), REL_X,
			       bits, NBITS(REL_MAX)) &&
	       evdev_test_bits(fd, EVIOCGBIT(EV_REL, sizeof(bits)), REL_Y,
			       bits, NBITS(REL_MAX)) &&
	       evdev_test_bits(fd, EVIOCGBIT(EV_REL, sizeof(bits)), REL_Z,
			       bits, NBITS(REL_MAX));
}

static int evdev_is_prox(int fd)
{
	unsigned long bits[NBITS(ABS_MAX)];

	return evdev_test_bits(fd, EVIOCGBIT(EV_ABS, sizeof(bits)),
			       ABS_DISTANCE, bits, NBITS(ABS_MAX));
}

static void sensor_close(struct sensor *s)
{
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
	s->present = 0;
	s->enabled = 0;
}

static void drain_events(int fd)
{
	struct input_event ev;

	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
		;
}

static int sensor_from_event(int idx, const char *want_name,
			     int (*capable)(int), struct sensor *s)
{
	char path[64], name[256];
	int fd;

	snprintf(path, sizeof(path), "/dev/input/event%d", idx);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return 0;
	memset(name, 0, sizeof(name));
	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 ||
	    strcmp(name, want_name) != 0 || !capable(fd)) {
		close(fd);
		return 0;
	}

	if (s->fd >= 0)
		close(s->fd);
	s->fd = fd;
	s->present = 1;
	s->enabled = 0;
	snprintf(s->evpath, sizeof(s->evpath), "%s", path);
	snprintf(s->sysfs, sizeof(s->sysfs), "/sys/class/input/event%d/device",
		 idx);
	snprintf(s->enable_path, sizeof(s->enable_path), "%s/enable", s->sysfs);
	snprintf(s->delay_path, sizeof(s->delay_path), "%s/poll_delay",
		 s->sysfs);
	s->has_enable = access(s->enable_path, W_OK) == 0;
	s->has_delay = access(s->delay_path, W_OK) == 0;

	logmsg("found %s at %s (control: enable=%s poll_delay=%s)", s->kind,
	       path, s->has_enable ? "yes" : "no",
	       s->has_delay ? "yes" : "no");
	if (!s->has_enable)
		logmsg("warning: %s has no writable sysfs 'enable'; "
		       "reading will start unconditionally", s->kind);
	return 1;
}

static int discover_sensor(struct sensor *s, const char *name,
			   int (*capable)(int))
{
	int i;

	for (i = 0; i < MAX_EVENT_DEV; i++)
		if (sensor_from_event(i, name, capable, s))
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* sensor enable/disable                                              */
/* ------------------------------------------------------------------ */

static void sensor_set_enabled(struct sensor *s, int on)
{
	char delay[32];

	if (!s->present || s->enabled == on)
		return;
	if (on) {
		if (s->has_delay) {
			snprintf(delay, sizeof(delay), "%lld",
				 ACCEL_POLL_DELAY_NS);
			if (write_sysfs(s->delay_path, delay) < 0)
				logmsg("failed to write %s", s->delay_path);
		}
		if (s->has_enable &&
		    write_sysfs(s->enable_path, "1") < 0) {
			logmsg("failed to enable %s via %s: %s", s->kind,
			       s->enable_path, strerror(errno));
			return;
		}
		drain_events(s->fd);
	} else {
		if (s->has_enable &&
		    write_sysfs(s->enable_path, "0") < 0)
			logmsg("failed to disable %s via %s: %s", s->kind,
			       s->enable_path, strerror(errno));
		drain_events(s->fd);
	}
	s->enabled = on;
	logdbg("%s %s", s->kind, on ? "enabled" : "disabled");
}

/* ------------------------------------------------------------------ */
/* claim tracking                                                     */
/* ------------------------------------------------------------------ */

static int claim_list_add(struct claim_list *l, const char *name)
{
	int i;

	for (i = 0; i < l->count; i++)
		if (!strcmp(l->names[i], name))
			return 0; /* already claimed */
	if (l->count >= MAX_CLAIMS)
		return -1;
	snprintf(l->names[l->count], CLAIM_NAME_MAX, "%s", name);
	l->count++;
	return 1;
}

static int claim_list_remove(struct claim_list *l, const char *name)
{
	int i;

	for (i = 0; i < l->count; i++) {
		if (strcmp(l->names[i], name))
			continue;
		memmove(&l->names[i], &l->names[i + 1],
			(size_t)(l->count - i - 1) * CLAIM_NAME_MAX);
		l->count--;
		return 1;
	}
	return 0;
}

static void claims_release_owner(const char *name)
{
	int a, p, l;

	a = claim_list_remove(&accel_claims, name);
	p = claim_list_remove(&prox_claims, name);
	l = claim_list_remove(&light_claims, name);
	if (!a && !p && !l)
		return;
	logdbg("released claims of vanished client %s", name);
	if (a && accel_claims.count == 0)
		sensor_set_enabled(&accel_sensor, 0);
	if (p && prox_claims.count == 0)
		sensor_set_enabled(&prox_sensor, 0);
}

/* ------------------------------------------------------------------ */
/* input event processing                                             */
/* ------------------------------------------------------------------ */

static void accel_reading(int raw_x, int raw_y, int raw_z)
{
	enum orientation new_orientation;
	enum tilt new_tilt;
	int x, y, z;

	/*
	 * Convert to iio-sensor-proxy's "1 g == 256" units.
	 *
	 * The K2HH driver emits Android-convention proper acceleration
	 * (the axis pointing at the sky reads +g; the Android HAL only
	 * multiplies by 9.80665/8192, see AccelerometerSensor.cpp in
	 * android_hardware_samsung_sensors).  iio-sensor-proxy's
	 * orientation_calc(), inherited from sensorfw, expects the gravity
	 * vector instead, so negate it: this makes an upright phone report
	 * "normal" (the upstream test vector for normal is (0, -256, 0)).
	 * tilt_calc() expects proper acceleration (z=+256 is "face-up"), so
	 * it uses the unnegated vector.
	 */
	x = (int)(raw_x * ORIENT_UNITS_PER_G / K2HH_LSB_PER_G);
	y = (int)(raw_y * ORIENT_UNITS_PER_G / K2HH_LSB_PER_G);
	z = (int)(raw_z * ORIENT_UNITS_PER_G / K2HH_LSB_PER_G);

	new_orientation = orientation_calc(current_orientation, -x, -y, -z);
	if (new_orientation != current_orientation) {
		current_orientation = new_orientation;
		logdbg("orientation: %s", orientation_names[current_orientation]);
		emit_string_prop("AccelerometerOrientation",
				 orientation_names[current_orientation]);
	}

	new_tilt = tilt_calc(current_tilt, x, y, z);
	if (new_tilt != current_tilt) {
		current_tilt = new_tilt;
		logdbg("tilt: %s", tilt_names[current_tilt]);
		emit_string_prop("AccelerometerTilt",
				 tilt_names[current_tilt]);
	}
}

static void proximity_reading(int distance)
{
	int near = distance == 0; /* 0 = close, 1 = far (driver comments) */

	if (near == proximity_near)
		return;
	proximity_near = near;
	logdbg("proximity: %s", near ? "near" : "far");
	emit_bool_prop("ProximityNear", near);
}

static int handle_accel_events(void)
{
	struct input_event ev;
	int x = 0, y = 0, z = 0, saw_rel = 0;

	/*
	 * The evdev layer does not deliver zero-valued EV_REL events, but
	 * k2hh_work_func() reads all three axes for every report and emits
	 * them in a single SYN_REPORT.  A missing axis therefore means that
	 * its raw value was exactly 0, not that the previous value should be
	 * reused (EV_REL carries absolute samples in this driver).  Keep the
	 * accumulator zero-initialised and consider every report complete.
	 */
	while (read(accel_sensor.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (getenv("J7ELTE_EVDEV_DEBUG"))
			fprintf(stderr, "evdev: type=%u code=%u value=%d\n",
				ev.type, ev.code, ev.value);
		if (ev.type == EV_REL) {
			saw_rel = 1;
			switch (ev.code) {
			case REL_X:
				x = ev.value;
				break;
			case REL_Y:
				y = ev.value;
				break;
			case REL_Z:
				z = ev.value;
				break;
			default:
				break;
			}
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			if (saw_rel)
				accel_reading(x, y, z);
			x = y = z = 0;
			saw_rel = 0;
		}
	}
	return 0;
}

static int handle_prox_events(void)
{
	struct input_event ev;

	while (read(prox_sensor.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_ABS && ev.code == ABS_DISTANCE)
			proximity_reading(ev.value);
	}
	return 0;
}

static void prox_read_initial(void)
{
	struct input_absinfo info;

	if (prox_sensor.fd < 0)
		return;
	if (ioctl(prox_sensor.fd, EVIOCGABS(ABS_DISTANCE), &info) == 0) {
		logdbg("initial ABS_DISTANCE=%d", info.value);
		proximity_reading(info.value);
	}
}

/* ------------------------------------------------------------------ */
/* D-Bus interface                                                    */
/* ------------------------------------------------------------------ */

static const char introspection_xml[] =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
"<node>\n"
"  <interface name=\"net.hadess.SensorProxy\">\n"
"    <property name=\"HasAccelerometer\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"AccelerometerOrientation\" type=\"s\" access=\"read\"/>\n"
"    <property name=\"AccelerometerTilt\" type=\"s\" access=\"read\"/>\n"
"    <property name=\"HasAmbientLight\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"LightLevelUnit\" type=\"s\" access=\"read\"/>\n"
"    <property name=\"LightLevel\" type=\"d\" access=\"read\"/>\n"
"    <property name=\"HasProximity\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"ProximityNear\" type=\"b\" access=\"read\"/>\n"
"    <method name=\"ClaimAccelerometer\"/>\n"
"    <method name=\"ReleaseAccelerometer\"/>\n"
"    <method name=\"ClaimLight\"/>\n"
"    <method name=\"ReleaseLight\"/>\n"
"    <method name=\"ClaimProximity\"/>\n"
"    <method name=\"ReleaseProximity\"/>\n"
"  </interface>\n"
"  <interface name=\"org.freedesktop.DBus.Properties\">\n"
"    <method name=\"Get\">\n"
"      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"GetAll\">\n"
"      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"Set\">\n"
"      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
"    </method>\n"
"    <signal name=\"PropertiesChanged\">\n"
"      <arg name=\"interface_name\" type=\"s\"/>\n"
"      <arg name=\"changed_properties\" type=\"a{sv}\"/>\n"
"      <arg name=\"invalidated_properties\" type=\"as\"/>\n"
"    </signal>\n"
"  </interface>\n"
"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
"    <method name=\"Introspect\">\n"
"      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
"    </method>\n"
"  </interface>\n"
"  <interface name=\"org.freedesktop.DBus.Peer\">\n"
"    <method name=\"Ping\"/>\n"
"    <method name=\"GetMachineId\">\n"
"      <arg name=\"machine_uuid\" type=\"s\" direction=\"out\"/>\n"
"    </method>\n"
"  </interface>\n"
"</node>\n";

static const char *const property_names[] = {
	"HasAccelerometer",
	"AccelerometerOrientation",
	"AccelerometerTilt",
	"HasAmbientLight",
	"LightLevelUnit",
	"LightLevel",
	"HasProximity",
	"ProximityNear",
	NULL,
};

struct string_ctx {
	const char *s;
};

static void fill_string(struct dw_writer *w, void *ud)
{
	struct string_ctx *ctx = ud;

	dw_w_string(w, ctx->s);
}

struct variant_ctx {
	const char *name;
};

static void fill_variant(struct dw_writer *w, void *ud)
{
	struct variant_ctx *ctx = ud;

	write_prop_variant(w, ctx->name);
}

struct get_all_ctx {
	int dummy;
};

static void fill_all_props(struct dw_writer *w, void *ud)
{
	struct dw_array_marker dict;
	int i;

	(void)ud;
	dict = dw_w_array_begin(w, 8);
	for (i = 0; property_names[i]; i++) {
		dw_w_align(w, 8);
		dw_w_string(w, property_names[i]);
		write_prop_variant(w, property_names[i]);
	}
	dw_w_array_end(w, dict);
}

static int read_two_strings(struct dw_msg *m, struct dw_reader *r,
			    char *a, size_t alen, char *b, size_t blen)
{
	const char *s;
	size_t n;

	r->p = m->raw;
	r->len = m->raw_len;
	r->off = m->body_off;
	if (dw_r_string(r, &s, &n) < 0 || n >= alen)
		return -1;
	memcpy(a, s, n);
	a[n] = '\0';
	if (dw_r_string(r, &s, &n) < 0 || n >= blen)
		return -1;
	memcpy(b, s, n);
	b[n] = '\0';
	return 0;
}

static int prop_known(const char *name)
{
	int i;

	for (i = 0; property_names[i]; i++)
		if (!strcmp(name, property_names[i]))
			return 1;
	return 0;
}

static void handle_properties_call(struct dw_msg *m)
{
	struct dw_reader r;
	char iface[128], prop[128];

	if (!strcmp(m->member, "Get")) {
		struct variant_ctx ctx;

		if (read_two_strings(m, &r, iface, sizeof(iface), prop,
				     sizeof(prop)) < 0 ||
		    strcmp(iface, SENSOR_PROXY_NAME) != 0 ||
		    !prop_known(prop)) {
			dw_send_error(bus, m, "org.freedesktop.DBus.Error.InvalidArgs",
				      "Invalid interface or property name");
			return;
		}
		ctx.name = prop;
		dw_send_return(bus, m, "v", fill_variant, &ctx);
	} else if (!strcmp(m->member, "GetAll")) {
		struct dw_reader rr;
		const char *s;
		size_t n;
		struct get_all_ctx ctx = { 0 };

		rr.p = m->raw;
		rr.len = m->raw_len;
		rr.off = m->body_off;
		if (dw_r_string(&rr, &s, &n) < 0 || n >= sizeof(iface) ||
		    strcmp(s, SENSOR_PROXY_NAME) != 0) {
			dw_send_error(bus, m, "org.freedesktop.DBus.Error.InvalidArgs",
				      "Invalid interface name");
			return;
		}
		dw_send_return(bus, m, "a{sv}", fill_all_props, &ctx);
	} else if (!strcmp(m->member, "Set")) {
		dw_send_error(bus, m,
			      "org.freedesktop.DBus.Error.PropertyReadOnly",
			      "All net.hadess.SensorProxy properties are read-only");
	} else {
		dw_send_error(bus, m, "org.freedesktop.DBus.Error.UnknownMethod",
			      "Unknown method on org.freedesktop.DBus.Properties");
	}
}

static void handle_claim(struct dw_msg *m, int claim)
{
	struct claim_list *list;
	struct sensor *sensor;

	if (!strcmp(m->member, "ClaimAccelerometer") ||
	    !strcmp(m->member, "ReleaseAccelerometer")) {
		list = &accel_claims;
		sensor = &accel_sensor;
	} else if (!strcmp(m->member, "ClaimProximity") ||
		   !strcmp(m->member, "ReleaseProximity")) {
		list = &prox_claims;
		sensor = &prox_sensor;
	} else {
		list = &light_claims;
		sensor = NULL;
	}

	if (claim) {
		int added = claim_list_add(list, m->sender);

		if (added < 0) {
			dw_send_error(bus, m,
				      "org.freedesktop.DBus.Error.LimitsExceeded",
				      "Too many clients");
			return;
		}
		if (added && list->count == 1 && sensor) {
			sensor_set_enabled(sensor, 1);
			if (sensor == &prox_sensor)
				prox_read_initial();
		}
	} else {
		if (claim_list_remove(list, m->sender) &&
		    list->count == 0 && sensor)
			sensor_set_enabled(sensor, 0);
	}
	dw_send_return(bus, m, NULL, NULL, NULL);
}

static void handle_sensor_call(struct dw_msg *m)
{
	if (!strcmp(m->member, "ClaimAccelerometer"))
		handle_claim(m, 1);
	else if (!strcmp(m->member, "ReleaseAccelerometer"))
		handle_claim(m, 0);
	else if (!strcmp(m->member, "ClaimLight"))
		handle_claim(m, 1);
	else if (!strcmp(m->member, "ReleaseLight"))
		handle_claim(m, 0);
	else if (!strcmp(m->member, "ClaimProximity"))
		handle_claim(m, 1);
	else if (!strcmp(m->member, "ReleaseProximity"))
		handle_claim(m, 0);
	else
		dw_send_error(bus, m, "org.freedesktop.DBus.Error.UnknownMethod",
			      "Unknown method on net.hadess.SensorProxy");
}

static void handle_name_owner_changed(struct dw_msg *m)
{
	struct dw_reader r;
	const char *name, *old_owner, *new_owner;
	size_t n1, n2, n3;

	r.p = m->raw;
	r.len = m->raw_len;
	r.off = m->body_off;
	if (dw_r_string(&r, &name, &n1) < 0 ||
	    dw_r_string(&r, &old_owner, &n2) < 0 ||
	    dw_r_string(&r, &new_owner, &n3) < 0)
		return;
	if (n3 == 0 && n2 > 0 && n1 > 0 && n1 < 128) {
		char owner[128];

		/* For NameOwnerChanged the vanished client is in "name". */
		memcpy(owner, name, n1);
		owner[n1] = '\0';
		claims_release_owner(owner);
	}
}

static int read_machine_id(char *out, size_t outlen)
{
	static const char *const paths[] = {
		"/etc/machine-id", "/var/lib/dbus/machine-id", NULL
	};
	int i;

	for (i = 0; paths[i]; i++) {
		char raw[64];
		ssize_t n;
		int fd = open(paths[i], O_RDONLY | O_CLOEXEC);

		if (fd < 0)
			continue;
		n = read(fd, raw, sizeof(raw) - 1);
		close(fd);
		if (n <= 0)
			continue;
		while (n > 0 && (raw[n - 1] == '\n' || raw[n - 1] == '\r' ||
				 raw[n - 1] == ' '))
			n--;
		raw[n] = '\0';
		if (n != 32)
			continue;
		if (snprintf(out, outlen,
			     "%.8s-%.4s-%.4s-%.4s-%.12s",
			     raw, raw + 8, raw + 12, raw + 16, raw + 20) >=
		    (int)outlen)
			return -1;
		return 0;
	}
	return -1;
}

static void handle_message(struct dw_msg *m)
{
	if (m->type == DW_TYPE_METHOD_CALL) {
		if (!strcmp(m->iface, "org.freedesktop.DBus.Properties")) {
			handle_properties_call(m);
		} else if (!strcmp(m->iface,
				   "org.freedesktop.DBus.Introspectable") &&
			   !strcmp(m->member, "Introspect")) {
			struct string_ctx ctx = { introspection_xml };

			dw_send_return(bus, m, "s", fill_string, &ctx);
		} else if (!strcmp(m->iface, "org.freedesktop.DBus.Peer") &&
			   !strcmp(m->member, "Ping")) {
			dw_send_return(bus, m, NULL, NULL, NULL);
		} else if (!strcmp(m->iface, "org.freedesktop.DBus.Peer") &&
			   !strcmp(m->member, "GetMachineId")) {
			char uuid[40];
			struct string_ctx ctx = { uuid };

			if (read_machine_id(uuid, sizeof(uuid)) < 0) {
				dw_send_error(bus, m,
					      "org.freedesktop.DBus.Error.Failed",
					      "No machine ID available");
				return;
			}
			dw_send_return(bus, m, "s", fill_string, &ctx);
		} else if (!strcmp(m->iface, SENSOR_PROXY_NAME)) {
			handle_sensor_call(m);
		} else {
			dw_send_error(bus, m,
				      "org.freedesktop.DBus.Error.UnknownInterface",
				      "Unknown interface");
		}
	} else if (m->type == DW_TYPE_SIGNAL) {
		if (!strcmp(m->iface, "org.freedesktop.DBus") &&
		    !strcmp(m->member, "NameOwnerChanged"))
			handle_name_owner_changed(m);
	}
}

/* ------------------------------------------------------------------ */
/* main loop                                                          */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void discover_missing(void)
{
	if (!accel_sensor.present) {
		if (discover_sensor(&accel_sensor, ACCEL_INPUT_NAME,
				    evdev_is_accel)) {
			emit_bool_prop("HasAccelerometer", 1);
			if (accel_claims.count > 0)
				sensor_set_enabled(&accel_sensor, 1);
		}
	}
	if (!prox_sensor.present) {
		if (discover_sensor(&prox_sensor, PROX_INPUT_NAME,
				    evdev_is_prox)) {
			emit_bool_prop("HasProximity", 1);
			if (prox_claims.count > 0) {
				sensor_set_enabled(&prox_sensor, 1);
				prox_read_initial();
			}
		}
	}
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--address D-BUS-ADDRESS] [--verbose]\n"
		"  Default address: $DBUS_SYSTEM_BUS_ADDRESS or "
		"unix:path=/run/dbus/system_bus_socket\n", argv0);
}

int main(int argc, char **argv)
{
	const char *address = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	char err[256];
	struct sigaction sa = { .sa_handler = on_signal };
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--address") && i + 1 < argc) {
			address = argv[++i];
		} else if (!strcmp(argv[i], "--verbose") ||
			   !strcmp(argv[i], "-v")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "--help") ||
			   !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	bus = dw_conn_open(address, err, sizeof(err));
	if (!bus) {
		logmsg("cannot connect to D-Bus: %s", err);
		return 1;
	}
	if (dw_request_name(bus, SENSOR_PROXY_NAME, err, sizeof(err)) < 0) {
		logmsg("%s", err);
		dw_conn_free(bus);
		return 1;
	}
	if (dw_add_match(bus, "type='signal',sender='org.freedesktop.DBus',"
			 "interface='org.freedesktop.DBus',"
			 "member='NameOwnerChanged'") < 0)
		logmsg("warning: could not subscribe to NameOwnerChanged");

	discover_sensor(&accel_sensor, ACCEL_INPUT_NAME, evdev_is_accel);
	discover_sensor(&prox_sensor, PROX_INPUT_NAME, evdev_is_prox);
	if (!accel_sensor.present)
		logmsg("warning: '%s' input device not found yet",
		       ACCEL_INPUT_NAME);
	if (!prox_sensor.present)
		logmsg("warning: '%s' input device not found yet",
		       PROX_INPUT_NAME);
	logmsg("owning %s as %s", SENSOR_PROXY_NAME,
	       accel_sensor.present || prox_sensor.present ?
	       "sensor proxy" : "empty sensor proxy");

	while (!stop_requested) {
		struct pollfd fds[3];
		int nfds = 0, dbus_idx, accel_idx = -1, prox_idx = -1;
		int timeout = (accel_sensor.present && prox_sensor.present) ?
			      -1 : 2000;
		int r;

		dbus_idx = nfds;
		fds[nfds].fd = dw_conn_fd(bus);
		fds[nfds].events = POLLIN;
		fds[nfds].revents = 0;
		nfds++;
		if (accel_sensor.fd >= 0) {
			accel_idx = nfds;
			fds[nfds].fd = accel_sensor.fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}
		if (prox_sensor.fd >= 0) {
			prox_idx = nfds;
			fds[nfds].fd = prox_sensor.fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}

		r = poll(fds, (nfds_t)nfds, timeout);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			logmsg("poll: %s", strerror(errno));
			break;
		}
		if (r == 0) {
			discover_missing();
			continue;
		}

		if (fds[dbus_idx].revents & POLLIN) {
			while (dw_conn_fill(bus, 0) == 1)
				;
			for (;;) {
				struct dw_msg m;
				int mr = dw_next_message(bus, &m);

				if (mr == 0)
					break;
				if (mr < 0) {
					logmsg("malformed D-Bus message");
					stop_requested = 1;
					break;
				}
				handle_message(&m);
			}
		}
		if (fds[dbus_idx].revents & (POLLHUP | POLLERR)) {
			logmsg("D-Bus connection closed");
			break;
		}
		if (accel_idx >= 0 && fds[accel_idx].revents & POLLIN)
			handle_accel_events();
		if (prox_idx >= 0 && fds[prox_idx].revents & POLLIN)
			handle_prox_events();
		if (accel_idx >= 0 &&
		    fds[accel_idx].revents & (POLLHUP | POLLERR | POLLNVAL)) {
			logmsg("accelerometer input device disappeared");
			sensor_close(&accel_sensor);
			emit_bool_prop("HasAccelerometer", 0);
		}
		if (prox_idx >= 0 &&
		    fds[prox_idx].revents & (POLLHUP | POLLERR | POLLNVAL)) {
			logmsg("proximity input device disappeared");
			sensor_close(&prox_sensor);
			emit_bool_prop("HasProximity", 0);
		}
	}

	sensor_set_enabled(&accel_sensor, 0);
	sensor_set_enabled(&prox_sensor, 0);
	sensor_close(&accel_sensor);
	sensor_close(&prox_sensor);
	dw_conn_free(bus);
	logmsg("stopped");
	return 0;
}
