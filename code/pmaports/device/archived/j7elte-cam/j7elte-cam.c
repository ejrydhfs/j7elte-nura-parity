// SPDX-License-Identifier: MIT
/*
 * j7elte-cam - minimal still-frame capture client for the Samsung Galaxy J7
 * (SM-J700F/M, "j7elte", Exynos 7580) FIMC-IS camera pipeline.
 *
 * The FIMC-IS2 driver in the Nura 3.10 kernel is not a generic V4L2 capture
 * device.  It exposes Samsung's Android camera-HAL interface instead:
 *
 *   - one "sensor" video node per sensor        (exynos-fimc-is-ss0/ss1)
 *   - a 3AA group: leader "30s"/"31s" (metadata output) and capture
 *     nodes "30c"/"31c"
 *   - an ISP group: leader "i0s"/"i1s" (metadata output) and the scaler
 *     capture node "scp"
 *
 * Each leader node is an OUTPUT (metadata) queue: every buffer's last plane
 * is a struct camera2_shot_ext describing the per-frame node group (crop
 * rectangles and which capture nodes are enabled).  The driver validates
 * shot->magicNumber and programs the firmware from it.  Capture nodes are
 * ordinary MMAP capture queues.
 *
 * This client speaks that interface directly, without libexynoscamera.so:
 * it opens the five nodes above, builds the shot metadata, starts the
 * pipeline and writes the first SCP frame as a PPM.  It does NOT implement
 * a camera HAL: there is no JPEG encoder.  The firmware's setfile supplies
 * the ISP defaults.
 *
 * 3A: the driver exposes no V4L2 exposure/gain/white-balance control that
 * works for these two sensors.  The only 3A control surface is the shot
 * metadata itself (struct camera2_shot.ctl), which the firmware reads
 * straight out of the mapped OUTPUT buffer (see shot_apply_3a()).  With
 * --ae the tool runs a bounded closed-loop AE: it sets ctl.aa.aeMode to
 * AA_AEMODE_OFF and drives ctl.sensor.exposureTime/sensitivity from the
 * measured mean luma of each SCP frame.  --awb only forwards a white
 * balance mode to the firmware; there is no AWB loop because the metadata
 * ABI exposes no per-frame WB gain control that this client can validate.
 *
 * Node discovery uses /sys/class/video4linux/videoN/name because several of
 * the driver's ioctl handlers are stubs that return 0 without touching the
 * caller's struct (QUERYCAP/ENUM_FMT/G_FMT on the sensor and 3AA nodes,
 * while e.g. the SCP node's QUERYCAP does fill in the driver name), so the
 * program never relies on them for identification.  The driver also does not
 * register a media_device, so /dev/media* contains no FIMC-IS entity graph;
 * we enumerate it for diagnostics only.
 *
 * Evidence for the sequence is in parity/reports/camera-userspace.md.
 *
 * Copyright (c) 2026
 * MIT license.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#if defined(__has_include)
# if __has_include(<linux/media.h>)
#  include <linux/media.h>
#  define HAVE_MEDIA_H 1
# endif
#endif

#include "j7elte-cam-ioctl.h"
#include "j7elte-cam-meta.h"

/* The metadata ABI is fixed by CONFIG_ENABLE_HAL3_2_META_INTERFACE=y. */
_Static_assert(sizeof(struct camera2_shot_ext) == 30000,
	       "fimc-is metadata ABI mismatch (HAL3.2 interface?)");
_Static_assert(sizeof(struct camera2_shot) == 29624,
	       "fimc-is shot ABI mismatch");
_Static_assert(sizeof(struct camera2_node_group) == 200,
	       "fimc-is node_group ABI mismatch");
_Static_assert(__builtin_offsetof(struct camera2_shot_ext, node_group) == 4,
	       "fimc-is node_group offset mismatch");
_Static_assert(__builtin_offsetof(struct camera2_shot_ext, shot) == 376,
	       "fimc-is camera2_shot offset mismatch (dvaddr_shot)");
_Static_assert(__builtin_offsetof(struct camera2_shot, magicNumber) == 29616,
	       "fimc-is magicNumber offset mismatch");

#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define MAX_BUFFERS		8
#define MAX_PLANES		VIDEO_MAX_PLANES
#define SPARE_PLANE_SIZE	(32 * 1024)

/* Bounds of the optional AE loop (see ae_update()). */
#define AE_MAX_FRAMES		60
#define AE_DEFAULT_FRAMES	10
#define AE_DEFAULT_TARGET	110	/* sRGB mid-grey is ~118 */
#define AE_DEFAULT_TOL		8
#define AE_DEFAULT_ISO_MIN	100
#define AE_DEFAULT_ISO_MAX	800
#define AE_DEFAULT_EXP_MIN_NS	100000ull	/* 100 us */
#define AE_LUMA_SATURATED	250

/* Sensor mode tables, from sensor/module/fimc-is-device-{3l2,5e3}.c */
static const struct j7_sensor_mode modes_3l2[] = {
	J7_MODE(4144, 3106, 30, 0),
	J7_MODE(4144, 2332, 30, 1),
	J7_MODE(4128, 3096, 30, 2),
	J7_MODE(4128, 2322, 30, 3),
	J7_MODE(2072, 1166, 60, 4),
	J7_MODE(1008, 754, 120, 5),
	J7_MODE(1008, 568, 120, 6),
	J7_MODE(2072, 1552, 15, 7),
	J7_MODE(2072, 1552, 30, 8),
	J7_MODE(2072, 1552, 60, 9),
	J7_MODE(2072, 1162, 60, 10),
};

static const struct j7_sensor_mode modes_5e3[] = {
	J7_MODE(2576, 1932, 30, 0),
	J7_MODE(2576, 1932, 24, 1),
	J7_MODE(1280, 960, 15, 2),
	J7_MODE(1280, 960, 7, 3),
	J7_MODE(640, 480, 15, 4),
	J7_MODE(1280, 960, 30, 5),
	J7_MODE(640, 480, 120, 6),
};

struct j7_options {
	bool list;
	bool verbose;
	bool front;
	bool otf;
	bool full_range;
	bool sensor_size_set;
	int hal_version;
	int awb_mode;		/* -1 = do not touch ctl.aa.awbMode */
	bool ae;
	unsigned ae_frames;
	unsigned ae_target;
	unsigned ae_tol;
	unsigned ae_iso_min;
	unsigned ae_iso_max;
	uint64_t ae_exp_min_ns;
	uint64_t ae_exp_max_ns;	/* 0 = 90% of one frame period */
	unsigned nbufs;
	unsigned timeout_s;
	unsigned sensor_w;
	unsigned sensor_h;
	unsigned sensor_fps;
	unsigned out_w;
	unsigned out_h;
	const char *output;
	const char *raw;
	const char *node_ss;
	const char *node_3as;
	const char *node_3ac;
	const char *node_is;
	const char *node_scp;
};

enum j7_node_id {
	NODE_SS,
	NODE_3AS,
	NODE_3AC,
	NODE_IS,
	NODE_SCP,
	NODE_MAX,
};

struct j7_buffer {
	void *addr[MAX_PLANES];
	size_t len[MAX_PLANES];
	bool mapped[MAX_PLANES];
};

struct j7_node {
	const char *role;
	char name[64];
	char path[32];
	int fd;
	unsigned vid;
	bool output;		/* OUTPUT_MPLANE (shot metadata queue) */
	bool shot;		/* last plane is struct camera2_shot_ext */
	unsigned width;
	unsigned height;
	uint32_t pixfmt;
	unsigned nplanes;
	unsigned nbufs;
	bool streaming;
	unsigned completed;
	struct j7_buffer buf[MAX_BUFFERS];
};

struct j7_capture_node {
	unsigned vid;
	unsigned width;
	unsigned height;
};

/*
 * Closed-loop AE state.  Only exposure time and sensitivity (ISO) are
 * driven: those are the two quantities struct camera2_sensor_ctl exposes to
 * userspace (drivers/media/platform/exynos/fimc-is2/include/
 * fimc-is-metadata.h, "unit : nano" / "ISO arithmetic units").
 */
struct j7_ae {
	bool enabled;
	unsigned max_frames;
	unsigned target;
	unsigned tol;
	uint64_t frame_ns;	/* 1/fps, the AE frame period */
	uint64_t exp_ns;
	uint64_t exp_min_ns;
	uint64_t exp_max_ns;
	unsigned iso;
	unsigned iso_min;
	unsigned iso_max;
	unsigned frames;	/* iterations completed */
	unsigned luma;		/* mean luma of the last frame */
	bool converged;
	bool at_limit;		/* neither exposure nor gain can move further */
};

/*
 * 3A values reported back by the firmware, read from the 3AA leader shot
 * before that buffer is refilled.  The UDM comment in fimc-is-metadata.h
 * documents vendorSpecific2[100..103] as exposure, iso, Bv and Tv, so both
 * that block and the standard dm.sensor/dm.aa fields are sampled.
 */
struct j7_fw_3a {
	bool valid;
	uint32_t frame_count;
	uint64_t exposure_ns;
	uint32_t sensitivity;
	int32_t ae_state;
	uint32_t udm_exposure;
	uint32_t udm_iso;
	uint32_t bv;
	uint32_t tv;
};

static struct j7_options opt = {
	.nbufs = 4,
	.timeout_s = 15,
	.sensor_w = 2072,
	.sensor_h = 1552,
	.sensor_fps = 30,
	.out_w = 1280,
	.out_h = 720,
	.output = "j7elte-cam.ppm",
	.otf = true,
	.full_range = true,
	.hal_version = J7_IS_HAL_VER_3_2,
	.awb_mode = -1,
	.ae_frames = AE_DEFAULT_FRAMES,
	.ae_target = AE_DEFAULT_TARGET,
	.ae_tol = AE_DEFAULT_TOL,
	.ae_iso_min = AE_DEFAULT_ISO_MIN,
	.ae_iso_max = AE_DEFAULT_ISO_MAX,
	.ae_exp_min_ns = AE_DEFAULT_EXP_MIN_NS,
};

static const char *progname = "j7elte-cam";

static void msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "%s: ", progname);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "%s: fatal: ", progname);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static const char *v4l2_req_name(unsigned long req)
{
	switch (req) {
	case VIDIOC_QUERYCAP:		return "VIDIOC_QUERYCAP";
	case VIDIOC_ENUM_FMT:		return "VIDIOC_ENUM_FMT";
	case VIDIOC_G_FMT:		return "VIDIOC_G_FMT";
	case VIDIOC_S_FMT:		return "VIDIOC_S_FMT";
	case VIDIOC_REQBUFS:		return "VIDIOC_REQBUFS";
	case VIDIOC_QUERYBUF:		return "VIDIOC_QUERYBUF";
	case VIDIOC_QBUF:		return "VIDIOC_QBUF";
	case VIDIOC_DQBUF:		return "VIDIOC_DQBUF";
	case VIDIOC_STREAMON:		return "VIDIOC_STREAMON";
	case VIDIOC_STREAMOFF:		return "VIDIOC_STREAMOFF";
	case VIDIOC_S_INPUT:		return "VIDIOC_S_INPUT";
	case VIDIOC_S_CTRL:		return "VIDIOC_S_CTRL";
	case VIDIOC_G_CTRL:		return "VIDIOC_G_CTRL";
	default:			return "ioctl";
	}
}

static int j7_ioctl(int fd, unsigned long req, void *arg,
		    const char *node, const char *what)
{
	int ret, saved;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		saved = errno;
		msg("%s: %s (%s, req=0x%lx) failed: %s", node, what,
		    v4l2_req_name(req), req, strerror(saved));
		/* msg() may clobber errno; callers branch on it. */
		errno = saved;
		return -1;
	}

	if (opt.verbose)
		msg("%s: %s (%s) ok", node, what, v4l2_req_name(req));

	return 0;
}

/* ------------------------------------------------------------------ */
/* Node discovery                                                      */
/* ------------------------------------------------------------------ */

static bool read_sysfs_name(unsigned index, char *name, size_t size)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/class/video4linux/video%u/name", index);
	f = fopen(path, "r");
	if (!f)
		return false;

	if (!fgets(name, size, f)) {
		fclose(f);
		return false;
	}
	fclose(f);

	name[strcspn(name, "\r\n")] = '\0';
	return name[0] != '\0';
}

static int find_video_node(const char *wanted, char *path, size_t path_size,
			   char *found_name, size_t name_size)
{
	unsigned i;
	char name[64];

	for (i = 0; i < 256; i++) {
		if (!read_sysfs_name(i, name, sizeof(name)))
			continue;
		if (strcmp(name, wanted) != 0)
			continue;

		snprintf(path, path_size, "/dev/video%u", i);
		snprintf(found_name, name_size, "%s", name);
		return 0;
	}

	return -1;
}

static void list_nodes(void)
{
	unsigned i;
	char name[64];
	bool any = false;

	printf("Video nodes (/sys/class/video4linux):\n");
	for (i = 0; i < 256; i++) {
		if (!read_sysfs_name(i, name, sizeof(name)))
			continue;
		printf("  /dev/video%-3u %s\n", i, name);
		if (strstr(name, "exynos-fimc-is"))
			any = true;
	}
	if (!any)
		printf("  (no exynos-fimc-is video nodes found)\n");

	printf("\nMedia controller devices (/dev/media*):\n");
#ifdef HAVE_MEDIA_H
	for (i = 0; i < 16; i++) {
		char path[32];
		struct media_device_info info;
		int fd;

		snprintf(path, sizeof(path), "/dev/media%u", i);
		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;
		memset(&info, 0, sizeof(info));
		if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0) {
			printf("  %s driver=%s model=%s\n", path,
			       info.driver, info.model);
			{
				struct media_entity_desc ed;
				unsigned count = 0;

				memset(&ed, 0, sizeof(ed));
				ed.id = MEDIA_ENT_ID_FLAG_NEXT;
				while (count < 256 &&
				       ioctl(fd, MEDIA_IOC_ENUM_ENTITIES,
					     &ed) == 0) {
					printf("    entity %u: %s\n",
					       ed.id & 0xffffff, ed.name);
					count++;
					ed.id = (ed.id & ~MEDIA_ENT_ID_FLAG_NEXT)
						| MEDIA_ENT_ID_FLAG_NEXT;
					if (!(ed.id & ~MEDIA_ENT_ID_FLAG_NEXT))
						break;
				}
				if (!count)
					printf("    (no entities)\n");
			}
		} else {
			printf("  %s (no media device info)\n", path);
		}
		close(fd);
	}
#else
	printf("  (built without linux/media.h media-controller ioctls)\n");
#endif
	printf("\nNote: the fimc-is2 driver registers no media_device and no\n"
	       "v4l2 subdev nodes; /dev/media* carries no FIMC-IS entity graph.\n");
}

/* ------------------------------------------------------------------ */
/* Node setup                                                          */
/* ------------------------------------------------------------------ */

static int node_open(struct j7_node *n, const char *role,
		     const char *node_name, unsigned vid,
		     bool output, bool shot)
{
	memset(n, 0, sizeof(*n));
	n->role = role;
	n->vid = vid;
	n->output = output;
	n->shot = shot;
	n->fd = -1;

	if (find_video_node(node_name, n->path, sizeof(n->path),
			    n->name, sizeof(n->name)) < 0)
		die("video node \"%s\" (%s) not found; is "
		    "CONFIG_VIDEO_EXYNOS_FIMC_IS enabled and the sensor bound?",
		    node_name, role);

	n->fd = open(n->path, O_RDWR | O_NONBLOCK);
	if (n->fd < 0)
		die("cannot open %s (%s): %s", n->path, role, strerror(errno));

	msg("%s: %s -> %s%s", role, node_name, n->path,
	    output ? " (metadata OUTPUT queue)" : " (capture queue)");
	return 0;
}

static enum v4l2_buf_type node_buf_type(const struct j7_node *n)
{
	return n->output ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
			 : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
}

static int node_s_input(struct j7_node *n, uint32_t input)
{
	return j7_ioctl(n->fd, VIDIOC_S_INPUT, &input, n->role, "S_INPUT");
}

static int node_s_ctrl(struct j7_node *n, uint32_t id, int32_t value)
{
	struct v4l2_control ctrl = {
		.id = id,
		.value = value,
	};

	return j7_ioctl(n->fd, VIDIOC_S_CTRL, &ctrl, n->role, "S_CTRL");
}

static int node_set_format(struct j7_node *n, unsigned width, unsigned height,
			   uint32_t pixfmt)
{
	struct v4l2_format fmt;
	enum v4l2_buf_type type = node_buf_type(n);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.pixelformat = pixfmt;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;
	fmt.fmt.pix_mp.num_planes = 1;

	if (j7_ioctl(n->fd, VIDIOC_S_FMT, &fmt, n->role, "S_FMT") < 0)
		return -1;

	/*
	 * The driver never writes the accepted format back: S_FMT only reads
	 * the struct, and this driver's G_FMT handler is a stub that returns
	 * 0 with the struct untouched.  The requested values are therefore
	 * what the driver's queue stores (the format table supplies
	 * num_planes); the plane count and sizes that matter come from
	 * QUERYBUF in node_reqbufs().
	 */
	n->width = width;
	n->height = height;
	n->pixfmt = pixfmt;

	if (opt.verbose)
		msg("%s: %ux%u pixfmt=0x%08x", n->role, n->width, n->height,
		    n->pixfmt);

	return 0;
}

/*
 * The frame rate is a separate control from the format:
 * fimc_is_sensor_s_framerate() stores it in device->image.framerate and
 * re-runs get_sensor_mode(), and fimc_is_sensor_s_format() only then
 * resolves the final mode from the requested width/height *and* that frame
 * rate.  Without S_PARM the driver stays at SENSOR_DEFAULT_FRAMERATE (30)
 * and every other entry of the mode table is rejected by S_FMT with
 * -EINVAL.
 */
static int node_s_parm(struct j7_node *n, unsigned fps)
{
	struct v4l2_streamparm parm;

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;

	if (j7_ioctl(n->fd, VIDIOC_S_PARM, &parm, n->role, "S_PARM") < 0)
		return -1;

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (j7_ioctl(n->fd, VIDIOC_G_PARM, &parm, n->role, "G_PARM") == 0) {
		unsigned got = parm.parm.capture.timeperframe.denominator;

		if (got != fps)
			msg("%s: driver reports %u fps after requesting %u "
			    "(max_framerate limit?)", n->role, got, fps);
		else if (opt.verbose)
			msg("%s: frame rate %u fps", n->role, got);
	}

	return 0;
}

static int node_reqbufs(struct j7_node *n, unsigned count)
{
	struct v4l2_requestbuffers rb;
	unsigned i, p;

	memset(&rb, 0, sizeof(rb));
	rb.count = count;
	rb.type = node_buf_type(n);
	rb.memory = V4L2_MEMORY_MMAP;

	if (j7_ioctl(n->fd, VIDIOC_REQBUFS, &rb, n->role, "REQBUFS") < 0)
		return -1;

	if (rb.count < 1)
		die("%s: REQBUFS returned 0 buffers", n->role);
	if (rb.count > MAX_BUFFERS)
		die("%s: REQBUFS returned %u buffers (max %u)",
		    n->role, rb.count, MAX_BUFFERS);

	n->nbufs = rb.count;

	for (i = 0; i < n->nbufs; i++) {
		struct v4l2_buffer b;
		struct v4l2_plane planes[MAX_PLANES];

		memset(&b, 0, sizeof(b));
		memset(planes, 0, sizeof(planes));
		b.type = node_buf_type(n);
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.m.planes = planes;
		b.length = MAX_PLANES;

		if (j7_ioctl(n->fd, VIDIOC_QUERYBUF, &b, n->role,
			     "QUERYBUF") < 0)
			return -1;

		if (b.length < 1 || b.length > MAX_PLANES)
			die("%s: QUERYBUF returned %u planes", n->role,
			    b.length);

		if (i == 0)
			n->nplanes = b.length;

		if (b.length != n->nplanes)
			die("%s: buffer %u has %u planes, buffer 0 has %u",
			    n->role, i, b.length, n->nplanes);

		for (p = 0; p < b.length; p++) {
			void *addr;

			addr = mmap(NULL, planes[p].length,
				    PROT_READ | PROT_WRITE, MAP_SHARED,
				    n->fd, planes[p].m.mem_offset);
			if (addr == MAP_FAILED)
				die("%s: mmap(plane %u, %u bytes, off 0x%x): %s",
				    n->role, p, planes[p].length,
				    planes[p].m.mem_offset, strerror(errno));

			n->buf[i].addr[p] = addr;
			n->buf[i].len[p] = planes[p].length;
			n->buf[i].mapped[p] = true;
		}

		if (opt.verbose) {
			msg("%s: buf %u:", n->role, i);
			for (p = 0; p < b.length; p++)
				fprintf(stderr, " p%u=%zu", p,
					(size_t)planes[p].length);
			fputc('\n', stderr);
		}
	}

	if (n->nplanes < 2)
		die("%s: expected at least one data plane plus the driver's "
		    "spare plane, got %u", n->role, n->nplanes);

	if (n->buf[0].len[n->nplanes - 1] < SPARE_PLANE_SIZE)
		msg("%s: warning: spare plane is %zu bytes, driver normally "
		    "reserves %u", n->role,
		    n->buf[0].len[n->nplanes - 1], SPARE_PLANE_SIZE);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Shot metadata                                                       */
/* ------------------------------------------------------------------ */

static void set_crop(uint32_t crop[4], uint32_t x, uint32_t y,
		     uint32_t w, uint32_t h)
{
	crop[0] = x;
	crop[1] = y;
	crop[2] = w;
	crop[3] = h;
}

static void shot_fill(struct j7_node *n, unsigned index, unsigned leader_vid,
		      unsigned in_w, unsigned in_h,
		      const struct j7_capture_node *caps, unsigned ncap)
{
	unsigned spare = n->nplanes - 1;
	struct camera2_shot_ext *shot;
	size_t len;
	unsigned i;

	if (index >= n->nbufs)
		die("%s: shot index %u out of range", n->role, index);

	shot = n->buf[index].addr[spare];
	len = n->buf[index].len[spare];
	if (!shot)
		die("%s: spare plane not mapped", n->role);
	if (len < sizeof(*shot))
		die("%s: spare plane too small for camera2_shot_ext "
		    "(%zu < %zu)", n->role, len, sizeof(*shot));

	/*
	 * The kernel owns the driver-side fields; we only own the HAL part.
	 * Zero everything, then set the minimum the firmware/driver needs:
	 * a valid magic number, a node group with request=1 for every node
	 * we expect to run, and post-processing bypasses we do not drive.
	 */
	memset(shot, 0, sizeof(*shot));

	shot->setfile = 0;
	shot->drc_bypass = 1;
	shot->dis_bypass = 1;
	shot->dnr_bypass = 1;
	shot->fd_bypass = 1;

	shot->node_group.leader.vid = leader_vid;
	shot->node_group.leader.request = 1;
	set_crop(shot->node_group.leader.input.cropRegion, 0, 0, in_w, in_h);
	set_crop(shot->node_group.leader.output.cropRegion, 0, 0, in_w, in_h);

	if (ncap > (unsigned)CAPTURE_NODE_MAX)
		die("%s: %u capture nodes, max %d", n->role, ncap,
		    CAPTURE_NODE_MAX);

	for (i = 0; i < ncap; i++) {
		struct camera2_node *node = &shot->node_group.capture[i];

		node->vid = caps[i].vid;
		node->request = 1;
		set_crop(node->input.cropRegion, 0, 0, in_w, in_h);
		set_crop(node->output.cropRegion, 0, 0,
			 caps[i].width, caps[i].height);
	}

	shot->shot.magicNumber = SHOT_MAGIC_NUMBER;

	if (opt.verbose) {
		msg("%s: shot[%u] leader=%u %ux%u", n->role, index,
		    leader_vid, in_w, in_h);
		for (i = 0; i < ncap; i++)
			msg("%s:   capture[%u] vid=%u %ux%u", n->role, i,
			    caps[i].vid, caps[i].width, caps[i].height);
	}
}

/* ------------------------------------------------------------------ */
/* Buffer queueing                                                     */
/* ------------------------------------------------------------------ */

static int node_qbuf(struct j7_node *n, unsigned index, bool output_data)
{
	struct v4l2_buffer b;
	struct v4l2_plane planes[MAX_PLANES];
	unsigned p;

	if (index >= n->nbufs)
		die("%s: QBUF index %u out of range", n->role, index);

	memset(&b, 0, sizeof(b));
	memset(planes, 0, sizeof(planes));

	for (p = 0; p < n->nplanes; p++) {
		planes[p].length = n->buf[index].len[p];
		/*
		 * For MMAP output queues vb2 wants a non-zero bytesused.
		 * The shot lives in the spare (last) plane; the data planes
		 * of 30s/i0s are not consumed by the hardware.
		 */
		planes[p].bytesused = output_data ? planes[p].length : 0;
	}
	if (n->shot)
		planes[n->nplanes - 1].bytesused =
			n->buf[index].len[n->nplanes - 1];

	b.type = node_buf_type(n);
	b.memory = V4L2_MEMORY_MMAP;
	b.index = index;
	b.m.planes = planes;
	b.length = n->nplanes;

	return j7_ioctl(n->fd, VIDIOC_QBUF, &b, n->role, "QBUF");
}

/*
 * Returns 0 on a dequeued buffer, 1 when the queue is empty (EAGAIN), and
 * -1 on a real error.  EAGAIN is the normal result of the non-blocking
 * drain loop and must not print an error line.  *err reports
 * V4L2_BUF_FLAG_ERROR, which the ischain sets (VB2_BUF_STATE_ERROR) when a
 * frame was dropped or failed post-processing: such a buffer must not be
 * written out as if it were a valid picture.
 */
static int node_dqbuf(struct j7_node *n, unsigned *index, bool *err)
{
	struct v4l2_buffer b;
	struct v4l2_plane planes[MAX_PLANES];
	int ret, saved;

	memset(&b, 0, sizeof(b));
	memset(planes, 0, sizeof(planes));

	b.type = node_buf_type(n);
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = planes;
	b.length = MAX_PLANES;

	do {
		ret = ioctl(n->fd, VIDIOC_DQBUF, &b);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		saved = errno;
		if (saved == EAGAIN)
			return 1;
		msg("%s: DQBUF failed: %s", n->role, strerror(saved));
		errno = saved;
		return -1;
	}

	*index = b.index;
	*err = !!(b.flags & V4L2_BUF_FLAG_ERROR);
	n->completed++;
	return 0;
}

static int node_streamon(struct j7_node *n)
{
	enum v4l2_buf_type type = node_buf_type(n);

	if (j7_ioctl(n->fd, VIDIOC_STREAMON, &type, n->role,
		     "STREAMON") < 0)
		return -1;
	n->streaming = true;
	return 0;
}

static int node_streamoff(struct j7_node *n)
{
	enum v4l2_buf_type type = node_buf_type(n);

	if (!n->streaming || n->fd < 0)
		return 0;

	if (j7_ioctl(n->fd, VIDIOC_STREAMOFF, &type, n->role,
		     "STREAMOFF") < 0)
		return -1;
	n->streaming = false;
	return 0;
}

static void node_release(struct j7_node *n)
{
	unsigned i, p;

	if (n->fd < 0)
		return;

	for (i = 0; i < n->nbufs; i++)
		for (p = 0; p < n->nplanes; p++)
			if (n->buf[i].mapped[p])
				munmap(n->buf[i].addr[p], n->buf[i].len[p]);

	close(n->fd);
	n->fd = -1;
}

/* ------------------------------------------------------------------ */
/* Sensor mode validation                                              */
/* ------------------------------------------------------------------ */

/*
 * Mirrors get_sensor_mode() in fimc-is-device-sensor.c: an entry matches on
 * width and height, and an exact frame rate wins; otherwise the entry with
 * the smallest frame rate above the request is used.  So a request is
 * resolvable when some entry with the same window has fps >= the request.
 */
static bool mode_supported(bool front, unsigned w, unsigned h, unsigned fps)
{
	const struct j7_sensor_mode *m;
	size_t count, i;

	if (front) {
		m = modes_5e3;
		count = ARRAY_SIZE(modes_5e3);
	} else {
		m = modes_3l2;
		count = ARRAY_SIZE(modes_3l2);
	}

	if (fps == 0)
		return false;

	for (i = 0; i < count; i++) {
		if (m[i].width == w && m[i].height == h && m[i].fps >= fps)
			return true;
	}
	return false;
}

static void print_supported_modes(bool front)
{
	const struct j7_sensor_mode *m;
	size_t count, i;

	if (front) {
		m = modes_5e3;
		count = ARRAY_SIZE(modes_5e3);
	} else {
		m = modes_3l2;
		count = ARRAY_SIZE(modes_3l2);
	}

	fprintf(stderr, "Supported %s sensor modes: ",
		front ? "front S5K5E3" : "rear S5K3L2");
	for (i = 0; i < count; i++)
		fprintf(stderr, "%ux%u@%u%s", m[i].width, m[i].height,
			m[i].fps, i + 1 == count ? "\n" : " ");
}

/* ------------------------------------------------------------------ */
/* PPM output                                                          */
/* ------------------------------------------------------------------ */

static unsigned char clamp_u8(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return (unsigned char)v;
}

static int write_ppm(const char *path, const struct j7_node *scp,
		     unsigned index)
{
	const unsigned w = scp->width;
	const unsigned h = scp->height;
	const unsigned char *y_plane = scp->buf[index].addr[0];
	const unsigned char *cb_plane = scp->buf[index].addr[1];
	const unsigned char *cr_plane = scp->buf[index].addr[2];
	FILE *f;
	unsigned x, yy;
	int rc = 0;

	if (!y_plane || !cb_plane || !cr_plane) {
		msg("SCP frame planes are not mapped");
		return -1;
	}

	/*
	 * The driver allocates exactly w*h, w*h/4, w*h/4 for YUV420M when
	 * bytesperline is left at 0 (fimc_is_set_plane_size), so the tightly
	 * packed 4:2:0 layout below is the only thing the queue can hold.
	 */
	if (scp->buf[index].len[0] < (size_t)w * h ||
	    scp->buf[index].len[1] < (size_t)(w / 2) * (h / 2) ||
	    scp->buf[index].len[2] < (size_t)(w / 2) * (h / 2)) {
		msg("SCP planes are smaller than %ux%u 4:2:0 (%zu/%zu/%zu)",
		    w, h, scp->buf[index].len[0], scp->buf[index].len[1],
		    scp->buf[index].len[2]);
		return -1;
	}

	f = fopen(path, "wb");
	if (!f) {
		msg("cannot create %s: %s", path, strerror(errno));
		return -1;
	}

	fprintf(f, "P6\n%u %u\n255\n", w, h);

	for (yy = 0; yy < h; yy++) {
		for (x = 0; x < w; x++) {
			unsigned i = yy * w + x;
			unsigned ci = (yy / 2) * (w / 2) + (x / 2);
			int Y = y_plane[i];
			int Cb = cb_plane[ci] - 128;
			int Cr = cr_plane[ci] - 128;
			int r, g, b;

			if (!opt.full_range) {
				/* BT.601 limited range (16..235 / 16..240) */
				Y = (Y - 16) * 255 / 219;
				Cb = (cb_plane[ci] - 128) * 255 / 224;
				Cr = (cr_plane[ci] - 128) * 255 / 224;
			}

			/* BT.601 full-range integer conversion */
			r = Y + ((359 * Cr) >> 8);
			g = Y - ((88 * Cb + 183 * Cr) >> 8);
			b = Y + ((454 * Cb) >> 8);

			fputc(clamp_u8(r), f);
			fputc(clamp_u8(g), f);
			fputc(clamp_u8(b), f);
		}
	}

	if (ferror(f))
		rc = -1;
	if (fclose(f) != 0)
		rc = -1;

	if (rc)
		msg("error writing %s", path);
	else
		msg("wrote %s (%ux%u P6, from SCP buffer %u)", path, w, h,
		    index);

	return rc;
}

static int write_raw(const char *prefix, const struct j7_node *scp,
		     unsigned index)
{
	static const char *suffix[3] = { "y", "cb", "cr" };
	unsigned p;

	for (p = 0; p < 3; p++) {
		char path[PATH_MAX];
		FILE *f;
		size_t len = scp->buf[index].len[p];

		if (!scp->buf[index].addr[p])
			continue;

		snprintf(path, sizeof(path), "%s.%s", prefix, suffix[p]);
		f = fopen(path, "wb");
		if (!f) {
			msg("cannot create %s: %s", path, strerror(errno));
			return -1;
		}
		if (fwrite(scp->buf[index].addr[p], 1, len, f) != len) {
			msg("short write to %s", path);
			fclose(f);
			return -1;
		}
		fclose(f);
		msg("wrote %s (%zu bytes)", path, len);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Pipeline                                                            */
/* ------------------------------------------------------------------ */

/*
 * Mean luma of an SCP frame, sampled every 4th pixel in both directions
 * (1/16 of the pixels).  The SCP queue holds tightly packed YUV420M, so
 * plane 0 is width*height bytes of full-resolution Y.  Returns 0 when the
 * plane is not usable, which the AE controller treats as "completely dark".
 */
static unsigned frame_luma_mean(const struct j7_node *scp, unsigned index)
{
	const unsigned w = scp->width;
	const unsigned h = scp->height;
	const unsigned char *y;
	uint64_t sum = 0;
	unsigned n = 0;
	unsigned x, yy;

	if (index >= scp->nbufs || w == 0 || h == 0)
		return 0;

	y = scp->buf[index].addr[0];
	if (!y || scp->buf[index].len[0] < (size_t)w * h)
		return 0;

	for (yy = 0; yy < h; yy += 4) {
		const unsigned char *row = y + (size_t)yy * w;

		for (x = 0; x < w; x += 4) {
			sum += row[x];
			n++;
		}
	}

	return n ? (unsigned)((sum + n / 2) / n) : 0;
}

struct j7_pipeline {
	struct j7_node node[NODE_MAX];
	struct j7_ae ae;
	struct j7_fw_3a fw;
	unsigned sensor_id;
	unsigned ss_vid;
	unsigned a3s_vid;
	unsigned a3c_vid;
	unsigned is_vid;
	unsigned scp_vid;
};

/* ------------------------------------------------------------------ */
/* 3A: shot metadata control and firmware readback                     */
/* ------------------------------------------------------------------ */

/*
 * Program the 3A controls of a 3AA leader (30s/31s) shot.
 *
 * Everything the firmware's 3A engine can be told sits in struct
 * camera2_shot.ctl, which lives inside the same 30000-byte region the driver
 * hands to the firmware per group: fimc_is_itf_grp_shot() passes
 * frame->dvaddr_shot (= plane kva + 376, the start of camera2_shot) as
 * HIC_SHOT param1 (fimc-is-device-ischain.c:2443, fimc-is-video.c:575).
 * The driver never reads ctl.aa/ctl.sensor: it only validates the magic
 * number and bookkeeps the ctl.vendor_entry parameter indices
 * (fimc-is-device-ischain.c:1399-1407, 7488-7496), so these fields are a
 * pure userspace -> firmware channel:
 *
 *   ctl.aa.aeMode            AA_AEMODE_OFF disables the firmware AE
 *   ctl.sensor.exposureTime  shutter in ns
 *   ctl.sensor.sensitivity   gain in ISO arithmetic units
 *   ctl.sensor.frameDuration 1/fps in ns (keeps the sensor frame period)
 *   ctl.aa.awbMode           firmware AWB mode (--awb pass-through)
 *
 * The ABI has no per-frame white-balance gain control that is documented
 * for this firmware (ctl.color.gains belongs to the ISP colour-correction
 * transform, which the setfile owns), hence no AWB loop.
 */
static void shot_apply_3a(struct j7_node *n, unsigned index,
			  const struct j7_ae *ae)
{
	struct camera2_shot_ext *ext;

	if (index >= n->nbufs)
		die("%s: shot index %u out of range", n->role, index);

	ext = n->buf[index].addr[n->nplanes - 1];
	if (!ext)
		die("%s: spare plane not mapped", n->role);

	if (opt.awb_mode >= 0)
		ext->shot.ctl.aa.awbMode = (enum aa_awbmode)opt.awb_mode;

	if (!ae->enabled)
		return;

	ext->shot.ctl.aa.aeMode = AA_AEMODE_OFF;
	ext->shot.ctl.aa.aeTargetFpsRange[0] = opt.sensor_fps;
	ext->shot.ctl.aa.aeTargetFpsRange[1] = opt.sensor_fps;
	ext->shot.ctl.sensor.exposureTime = ae->exp_ns;
	ext->shot.ctl.sensor.frameDuration = ae->frame_ns;
	ext->shot.ctl.sensor.sensitivity = ae->iso;

	if (opt.verbose)
		msg("%s: shot[%u] ae: exp=%llu ns (%llu us), iso=%u, "
		    "frame=%llu ns", n->role, index,
		    (unsigned long long)ae->exp_ns,
		    (unsigned long long)(ae->exp_ns / 1000),
		    ae->iso, (unsigned long long)ae->frame_ns);
}

/*
 * Read back the 3A values the firmware left in a 3AA leader shot.  This has
 * to happen before the buffer is refilled: shot_fill() memsets the whole
 * struct camera2_shot_ext.
 */
static void shot_read_fw_3a(struct j7_node *n, unsigned index,
			    struct j7_fw_3a *out)
{
	const struct camera2_shot_ext *ext;

	memset(out, 0, sizeof(*out));

	if (index >= n->nbufs)
		return;
	ext = n->buf[index].addr[n->nplanes - 1];
	if (!ext)
		return;

	out->valid = true;
	out->frame_count = ext->shot.dm.request.frameCount;
	out->exposure_ns = ext->shot.dm.sensor.exposureTime;
	out->sensitivity = ext->shot.dm.sensor.sensitivity;
	out->ae_state = (int32_t)ext->shot.dm.aa.aeState;
	out->udm_exposure = ext->shot.udm.internal.vendorSpecific2[100];
	out->udm_iso = ext->shot.udm.internal.vendorSpecific2[101];
	out->bv = ext->shot.udm.internal.vendorSpecific2[102];
	out->tv = ext->shot.udm.internal.vendorSpecific2[103];
}

static void ae_init(struct j7_ae *ae)
{
	unsigned fps = opt.sensor_fps ? opt.sensor_fps : 30;

	memset(ae, 0, sizeof(*ae));
	if (!opt.ae)
		return;

	ae->enabled = true;
	ae->max_frames = opt.ae_frames;
	ae->target = opt.ae_target;
	ae->tol = opt.ae_tol;
	ae->frame_ns = 1000000000ull / fps;

	ae->iso_min = opt.ae_iso_min;
	ae->iso_max = opt.ae_iso_max;

	ae->exp_min_ns = opt.ae_exp_min_ns ? opt.ae_exp_min_ns : 1;
	/* Never ask for more than 90% of the frame period: the sensor needs
	 * the remaining time to read out. */
	ae->exp_max_ns = opt.ae_exp_max_ns ? opt.ae_exp_max_ns
					   : ae->frame_ns * 9 / 10;
	if (ae->exp_max_ns > ae->frame_ns * 9 / 10)
		ae->exp_max_ns = ae->frame_ns * 9 / 10;
	if (ae->exp_max_ns < ae->exp_min_ns)
		ae->exp_max_ns = ae->exp_min_ns;

	/* First guess: half a frame at ISO 200. */
	ae->exp_ns = ae->frame_ns / 2;
	ae->iso = 200;
	if (ae->iso < ae->iso_min)
		ae->iso = ae->iso_min;
	if (ae->iso > ae->iso_max)
		ae->iso = ae->iso_max;

	msg("AE: target luma %u +-%u, max %u frames, exposure %llu..%llu us, "
	    "iso %u..%u, start %llu us/iso %u", ae->target, ae->tol,
	    ae->max_frames, (unsigned long long)(ae->exp_min_ns / 1000),
	    (unsigned long long)(ae->exp_max_ns / 1000), ae->iso_min,
	    ae->iso_max, (unsigned long long)(ae->exp_ns / 1000), ae->iso);
}

static uint64_t clamp_u64(uint64_t v, uint64_t lo, uint64_t hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static unsigned clamp_u32(unsigned v, unsigned lo, unsigned hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

/*
 * One damped proportional AE step.  The controlled quantity is the product
 * exposure * sensitivity: a single value keeps the split between shutter and
 * gain monotonic, and both ends are clamped to the ranges the caller asked
 * for.  The step is damped by a factor of two so that a wrong luma reading
 * (or a scene change) cannot overshoot by more than ~4x per frame; a fully
 * black or clipped frame gets a deliberately larger step so the loop leaves
 * the rails.
 */
static void ae_update(struct j7_ae *ae, unsigned luma)
{
	uint64_t num, den, g, gn, lo, hi;
	uint64_t old_exp = ae->exp_ns;
	unsigned old_iso = ae->iso;

	ae->luma = luma;
	ae->frames++;

	if (luma + ae->tol >= ae->target && luma <= ae->target + ae->tol) {
		ae->converged = true;
		return;
	}

	num = (uint64_t)ae->target + luma;
	den = 2ull * (luma ? luma : 1);
	if (luma == 0) {
		num = den * 8;			/* completely dark: 8x */
	} else if (luma >= AE_LUMA_SATURATED) {
		den *= 4;			/* clipped: /4 */
	}

	g = ae->exp_ns * ae->iso;
	gn = g / den * num + (g % den) * num / den;

	lo = ae->exp_min_ns * ae->iso_min;
	hi = ae->exp_max_ns * ae->iso_max;
	gn = clamp_u64(gn, lo, hi);

	/* Prefer shutter speed over gain, then let the gain absorb the rest. */
	ae->exp_ns = clamp_u64(gn / ae->iso, ae->exp_min_ns, ae->exp_max_ns);
	ae->iso = clamp_u32((unsigned)(gn / ae->exp_ns), ae->iso_min,
			    ae->iso_max);
	ae->exp_ns = clamp_u64(gn / ae->iso, ae->exp_min_ns, ae->exp_max_ns);

	/*
	 * Both ends of the range are exhausted: further frames cannot change
	 * anything, so stop instead of spinning until --ae-frames.
	 */
	if (ae->exp_ns == old_exp && ae->iso == old_iso)
		ae->at_limit = true;
}

static const char *ae_state_name(int32_t state)
{
	switch (state) {
	case 0:				return "invalid";
	case AE_STATE_INACTIVE:		return "inactive";
	case AE_STATE_SEARCHING:	return "searching";
	case AE_STATE_CONVERGED:	return "converged";
	case AE_STATE_LOCKED:		return "locked";
	case AE_STATE_FLASH_REQUIRED:	return "flash-required";
	case AE_STATE_PRECAPTURE:	return "precapture";
	default:			return "?";
	}
}

static void ae_log(const struct j7_pipeline *p, unsigned luma)
{
	const struct j7_ae *ae = &p->ae;
	const struct j7_fw_3a *fw = &p->fw;

	msg("AE frame %u/%u: luma %u (target %u) -> exposure %llu us, "
	    "iso %u%s%s", ae->frames, ae->max_frames, luma, ae->target,
	    (unsigned long long)(ae->exp_ns / 1000), ae->iso,
	    ae->converged ? " [converged]" : "",
	    ae->at_limit ? " [at limit]" : "");

	if (fw->valid)
		msg("  firmware fcount %u: aeState %s (%d), dm exposure %llu us, "
		    "dm iso %u, udm exposure %u, udm iso %u, Bv %u, Tv %u",
		    fw->frame_count, ae_state_name(fw->ae_state), fw->ae_state,
		    (unsigned long long)(fw->exposure_ns / 1000),
		    fw->sensitivity, fw->udm_exposure, fw->udm_iso,
		    fw->bv, fw->tv);
}

/*
 * Summary after the loop.  A non-converged result is reported as such: the
 * tool cannot tell from the outside whether the scene was out of range or
 * whether the firmware ignored ctl.sensor, and it must not pretend either
 * way.  The per-frame firmware readback above is what distinguishes the two.
 */
static void ae_summary(const struct j7_pipeline *p)
{
	const struct j7_ae *ae = &p->ae;

	if (ae->converged)
		msg("AE converged after %u frame(s): luma %u (target %u)",
		    ae->frames, ae->luma, ae->target);
	else if (ae->at_limit)
		msg("AE stopped after %u frame(s) at the exposure/ISO limit: "
		    "luma %u, target %u; exposure %llu us at iso %u "
		    "(limits %llu..%llu us, iso %u..%u)", ae->frames, ae->luma,
		    ae->target, (unsigned long long)(ae->exp_ns / 1000),
		    ae->iso, (unsigned long long)(ae->exp_min_ns / 1000),
		    (unsigned long long)(ae->exp_max_ns / 1000),
		    ae->iso_min, ae->iso_max);
	else
		msg("AE did not converge in %u frame(s): luma %u, target %u; "
		    "exposure %llu us at iso %u (limits %llu..%llu us, "
		    "iso %u..%u)", ae->frames, ae->luma, ae->target,
		    (unsigned long long)(ae->exp_ns / 1000), ae->iso,
		    (unsigned long long)(ae->exp_min_ns / 1000),
		    (unsigned long long)(ae->exp_max_ns / 1000),
		    ae->iso_min, ae->iso_max);
}

static uint32_t encode_group_input(unsigned sensor_id, unsigned vindex,
				   bool otf, bool leader)
{
	return ((sensor_id & 0xff) << J7_INPUT_MODULE_SHIFT) |
	       ((vindex & 0xff) << J7_INPUT_VINDEX_SHIFT) |
	       ((otf ? J7_INPUT_MEMORY_OTF : J7_INPUT_MEMORY_DMA)
		<< J7_INPUT_MEMORY_SHIFT) |
	       ((leader ? 1u : 0u) << J7_INPUT_LEADER_SHIFT);
}

static int pipeline_open(struct j7_pipeline *p)
{
	memset(p, 0, sizeof(*p));

	if (opt.front) {
		p->sensor_id = J7_SENSOR_NAME_S5K5E3;
		p->ss_vid = J7_VID_SS1;
		p->a3s_vid = J7_VID_31S;
		p->a3c_vid = J7_VID_31C;
		p->is_vid = J7_VID_I0S;
		p->scp_vid = J7_VID_SCP;

		node_open(&p->node[NODE_SS], "ss1",
			  opt.node_ss ?: J7NODE_SS1, p->ss_vid, false, true);
		node_open(&p->node[NODE_3AS], "31s",
			  opt.node_3as ?: J7NODE_31S, p->a3s_vid, true, true);
		node_open(&p->node[NODE_3AC], "31c",
			  opt.node_3ac ?: J7NODE_31C, p->a3c_vid, false, false);
		node_open(&p->node[NODE_IS], "i0s",
			  opt.node_is ?: J7NODE_I0S, p->is_vid, true, true);
	} else {
		p->sensor_id = J7_SENSOR_NAME_S5K3L2;
		p->ss_vid = J7_VID_SS0;
		p->a3s_vid = J7_VID_30S;
		p->a3c_vid = J7_VID_30C;
		p->is_vid = J7_VID_I0S;
		p->scp_vid = J7_VID_SCP;

		node_open(&p->node[NODE_SS], "ss0",
			  opt.node_ss ?: J7NODE_SS0, p->ss_vid, false, true);
		node_open(&p->node[NODE_3AS], "30s",
			  opt.node_3as ?: J7NODE_30S, p->a3s_vid, true, true);
		node_open(&p->node[NODE_3AC], "30c",
			  opt.node_3ac ?: J7NODE_30C, p->a3c_vid, false, false);
		node_open(&p->node[NODE_IS], "i0s",
			  opt.node_is ?: J7NODE_I0S, p->is_vid, true, true);
	}

	node_open(&p->node[NODE_SCP], "scp",
		  opt.node_scp ?: J7NODE_SCP, p->scp_vid, false, false);

	ae_init(&p->ae);

	return 0;
}

static int pipeline_configure(struct j7_pipeline *p)
{
	struct j7_node *ss = &p->node[NODE_SS];
	struct j7_node *a3s = &p->node[NODE_3AS];
	struct j7_node *a3c = &p->node[NODE_3AC];
	struct j7_node *is = &p->node[NODE_IS];
	struct j7_node *scp = &p->node[NODE_SCP];
	unsigned i;

	/*
	 * 1. Bind the sensor first: fimc_is_sensor_s_input() powers the
	 *    sensor, selects the module (8 = S5K3L2, 17 = S5K5E3) and
	 *    normal (firmware-driven) scenario.
	 */
	if (node_s_input(ss, (J7_SENSOR_SCENARIO_NORMAL <<
			      J7_SENSOR_SCENARIO_SHIFT) | p->sensor_id) < 0)
		return -1;

	/*
	 * 2. Program the frame rate before the mode is resolved: the driver
	 *    picks the sensor mode from width, height *and* frame rate.
	 */
	if (node_s_parm(ss, opt.sensor_fps) < 0)
		return -1;

	/*
	 * 3. Pick the sensor mode.  The driver only accepts the exact
	 *    width/height/fps triples in its per-module mode table.
	 */
	if (node_set_format(ss, opt.sensor_w, opt.sensor_h,
			    V4L2_PIX_FMT_SBGGR10) < 0)
		return -1;

	/*
	 * 4. Tell the driver which camera HAL generation we speak.  This
	 *    only selects the DVFS table (fimc-is-device-ischain.c:2040); the
	 *    metadata ABI is compile-time
	 *    (CONFIG_ENABLE_HAL3_2_META_INTERFACE=y), so 3.2 is the correct
	 *    default for a client that uses struct camera2_shot_ext.
	 */
	if (node_s_ctrl(a3s, J7_CID_IS_HAL_VERSION, opt.hal_version) < 0)
		return -1;

	/*
	 * 5. Open the ischain.  The driver defers real ischain open to the
	 *    V4L2_CID_IS_END_OF_STREAM control (fimc_is_video_s_ctrl ->
	 *    fimc_is_ischain_open_wrap(..., true)); without it the later
	 *    S_INPUT calls fail with -EMFILE.
	 */
	if (node_s_ctrl(a3s, J7_CID_IS_END_OF_STREAM, 1) < 0)
		return -1;

	/*
	 * 6. Group leaders.  Only these two nodes carry a valid
	 *    stream/module/vindex/memory/leader encoding:
	 *      30s/31s: source = sensor node, OTF, stream leader
	 *      i0s:     source = 30c/31c,   OTF
	 *    The second leader's S_INPUT loads the firmware and initialises
	 *    the ischain (fimc_is_ischain_init_wrap).
	 */
	if (node_s_input(a3s, encode_group_input(p->sensor_id, p->ss_vid,
						 opt.otf, true)) < 0)
		return -1;

	/* 7. Capture nodes only take the state-changing S_INPUT. */
	if (node_s_input(a3c, encode_group_input(p->sensor_id, p->ss_vid,
						 opt.otf, false)) < 0)
		return -1;

	if (node_s_input(is, encode_group_input(p->sensor_id, p->a3c_vid,
						opt.otf, false)) < 0)
		return -1;

	if (node_s_input(scp, encode_group_input(p->sensor_id, p->a3c_vid,
						 opt.otf, false)) < 0)
		return -1;

	/*
	 * 8. Formats.  30s/i0s carry Bayer-sized metadata buffers; 30c sits
	 *    on the 3AA BDS output (same size as the sensor); the SCP scaler
	 *    produces the requested YUV420M output.
	 */
	if (node_set_format(a3s, opt.sensor_w, opt.sensor_h,
			    V4L2_PIX_FMT_SBGGR10) < 0)
		return -1;
	if (node_set_format(a3c, opt.sensor_w, opt.sensor_h,
			    V4L2_PIX_FMT_SBGGR10) < 0)
		return -1;
	if (node_set_format(is, opt.sensor_w, opt.sensor_h,
			    V4L2_PIX_FMT_SBGGR10) < 0)
		return -1;
	if (node_set_format(scp, opt.out_w, opt.out_h,
			    V4L2_PIX_FMT_YUV420M) < 0)
		return -1;

	/*
	 * 9. Allocate, fill the shot metadata and queue every buffer before
	 *    streaming.  Four buffers per queue is what the group manager's
	 *    async/sync shot accounting expects at 30 fps.
	 */
	for (i = 0; i < NODE_MAX; i++) {
		if (node_reqbufs(&p->node[i], opt.nbufs) < 0)
			return -1;
	}

	for (i = 0; i < p->node[NODE_SS].nbufs; i++) {
		shot_fill(&p->node[NODE_SS], i, p->ss_vid,
			  opt.sensor_w, opt.sensor_h, NULL, 0);
		if (node_qbuf(&p->node[NODE_SS], i, true) < 0)
			return -1;
	}

	for (i = 0; i < p->node[NODE_3AS].nbufs; i++) {
		struct j7_capture_node caps[1] = {
			{ .vid = p->a3c_vid,
			  .width = opt.sensor_w, .height = opt.sensor_h },
		};

		shot_fill(&p->node[NODE_3AS], i, p->a3s_vid,
			  opt.sensor_w, opt.sensor_h, caps, 1);
		shot_apply_3a(&p->node[NODE_3AS], i, &p->ae);
		if (node_qbuf(&p->node[NODE_3AS], i, true) < 0)
			return -1;
	}

	for (i = 0; i < p->node[NODE_IS].nbufs; i++) {
		struct j7_capture_node caps[1] = {
			{ .vid = p->scp_vid,
			  .width = opt.out_w, .height = opt.out_h },
		};

		shot_fill(&p->node[NODE_IS], i, p->is_vid,
			  opt.sensor_w, opt.sensor_h, caps, 1);
		if (node_qbuf(&p->node[NODE_IS], i, true) < 0)
			return -1;
	}

	for (i = 0; i < p->node[NODE_3AC].nbufs; i++) {
		if (node_qbuf(&p->node[NODE_3AC], i, false) < 0)
			return -1;
	}

	for (i = 0; i < p->node[NODE_SCP].nbufs; i++) {
		if (node_qbuf(&p->node[NODE_SCP], i, false) < 0)
			return -1;
	}

	/*
	 * 10. Stream on.  Every queue must be streaming before the sensor is
	 *    told to run: fimc_is_itf_stream_on() requires the 3AA stream
	 *    leader to be in GROUP_START, and the FLITE/CSI subdevices are
	 *    started from the SSX queue.
	 */
	if (node_streamon(scp) < 0)
		return -1;
	if (node_streamon(a3c) < 0)
		return -1;
	if (node_streamon(is) < 0)
		return -1;
	if (node_streamon(a3s) < 0)
		return -1;
	if (node_streamon(ss) < 0)
		return -1;

	return 0;
}

static int pipeline_start_sensor(struct j7_pipeline *p)
{
	/*
	 * V4L2_CID_IS_S_STREAM on the sensor node: sstream = enable,
	 * instant count = 0 (free running), noblock = 0.  This calls
	 * fimc_is_sensor_front_start() -> fimc_is_itf_stream_on(), i.e. the
	 * firmware stream-on command that actually starts the sensor.
	 */
	return node_s_ctrl(&p->node[NODE_SS], J7_CID_IS_S_STREAM,
			   J7_IS_ENABLE_STREAM);
}

static void pipeline_stop_sensor(struct j7_pipeline *p)
{
	if (p->node[NODE_SS].fd >= 0)
		node_s_ctrl(&p->node[NODE_SS], J7_CID_IS_S_STREAM,
			    J7_IS_DISABLE_STREAM);
}

static void pipeline_requeue_shot(struct j7_pipeline *p, unsigned node_id,
				  unsigned index)
{
	struct j7_node *n = &p->node[node_id];
	struct j7_capture_node a3s_caps[1] = {
		{ .vid = p->a3c_vid,
		  .width = opt.sensor_w, .height = opt.sensor_h },
	};
	struct j7_capture_node is_caps[1] = {
		{ .vid = p->scp_vid,
		  .width = opt.out_w, .height = opt.out_h },
	};

	switch (node_id) {
	case NODE_SS:
		shot_fill(n, index, p->ss_vid, opt.sensor_w, opt.sensor_h,
			  NULL, 0);
		break;
	case NODE_3AS:
		shot_fill(n, index, p->a3s_vid, opt.sensor_w, opt.sensor_h,
			  a3s_caps, 1);
		shot_apply_3a(n, index, &p->ae);
		break;
	case NODE_IS:
		shot_fill(n, index, p->is_vid, opt.sensor_w, opt.sensor_h,
			  is_caps, 1);
		break;
	default:
		break;
	}

	node_qbuf(n, index, n->output);
}

static int pipeline_capture(struct j7_pipeline *p)
{
	struct pollfd pfds[NODE_MAX];
	unsigned map[NODE_MAX];
	unsigned nfds = 0;
	unsigned i;
	struct timespec start, now;
	bool captured = false;
	bool done = false;

	clock_gettime(CLOCK_MONOTONIC, &start);

	while (!done) {
		int ret;

		nfds = 0;
		for (i = 0; i < NODE_MAX; i++) {
			if (!p->node[i].streaming)
				continue;
			pfds[nfds].fd = p->node[i].fd;
			pfds[nfds].events = POLLIN | POLLPRI;
			pfds[nfds].revents = 0;
			map[nfds] = i;
			nfds++;
		}

		ret = poll(pfds, nfds, 500);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			msg("poll: %s", strerror(errno));
			return -1;
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= (time_t)opt.timeout_s) {
			msg("timed out after %us waiting for a frame",
			    opt.timeout_s);
			for (i = 0; i < NODE_MAX; i++)
				msg("  %s: %u buffers completed",
				    p->node[i].role, p->node[i].completed);
			return -1;
		}

		if (ret == 0)
			continue;

		for (i = 0; i < nfds && !done; i++) {
			unsigned node_id = map[i];
			struct j7_node *n = &p->node[node_id];

			if (!(pfds[i].revents & (POLLIN | POLLPRI)))
				continue;

			for (;;) {
				unsigned index;
				bool err = false;
				int dq;

				dq = node_dqbuf(n, &index, &err);
				if (dq == 1)
					break;	/* queue drained */
				if (dq < 0)
					return -1;

				if (err) {
					msg("%s: buffer %u completed with "
					    "V4L2_BUF_FLAG_ERROR, discarded",
					    n->role, index);
				} else if (node_id == NODE_3AS) {
					/*
					 * The firmware's 3A result lives in
					 * this shot; read it before
					 * pipeline_requeue_shot() refills
					 * the buffer.
					 */
					shot_read_fw_3a(n, index, &p->fw);
				} else if (node_id == NODE_SCP) {
					unsigned luma = 0;

					if (p->ae.enabled)
						luma = frame_luma_mean(n, index);

					if (write_ppm(opt.output, n,
						      index) < 0)
						return -1;
					if (opt.raw && write_raw(opt.raw, n,
								 index) < 0)
						return -1;
					captured = true;

					if (p->ae.enabled) {
						ae_update(&p->ae, luma);
						ae_log(p, luma);
						done = p->ae.converged ||
						       p->ae.at_limit ||
						       p->ae.frames >=
							       p->ae.max_frames;
					} else {
						done = true;
					}
				}

				/*
				 * Keep every queue fed: the ischain needs the
				 * next shot before it produces the next frame,
				 * and that shot must carry the AE state the
				 * last measurement produced.
				 */
				if (n->output || n->shot)
					pipeline_requeue_shot(p, node_id,
							      index);
				else
					node_qbuf(n, index, false);

				if (done)
					break;
			}
		}
	}

	if (p->ae.enabled)
		ae_summary(p);

	return captured ? 0 : -1;
}

static void pipeline_close(struct j7_pipeline *p)
{
	unsigned i;

	pipeline_stop_sensor(p);

	for (i = 0; i < NODE_MAX; i++)
		node_streamoff(&p->node[i]);
	for (i = 0; i < NODE_MAX; i++)
		node_release(&p->node[i]);
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void usage(FILE *f)
{
	fprintf(f,
"Usage: %s [options]\n"
"\n"
"Capture a still frame from the Samsung Galaxy J7 (j7elte) FIMC-IS\n"
"camera pipeline and write it as a PPM (P6) image.  With --ae the frame is\n"
"captured through a bounded auto-exposure loop instead of the first frame.\n"
"\n"
"Options:\n"
"  -l, --list                 list video/media nodes and exit\n"
"  -o, --output FILE          PPM output path (default: j7elte-cam.ppm)\n"
"  -r, --raw FILE             also dump raw Y/Cb/Cr planes to FILE.y/.cb/.cr\n"
"  -s, --sensor-size WxH[@FPS] sensor mode (default: 2072x1552@30); FPS is\n"
"                             programmed with VIDIOC_S_PARM before S_FMT\n"
"  -O, --out-size WxH         SCP output size (default: 1280x720; width\n"
"                             must be a multiple of 16, height even, and\n"
"                             neither may exceed the sensor window)\n"
"  -n, --buffers N            buffers per queue, 3..8 (default: 4)\n"
"  -t, --timeout SEC          capture timeout (default: 15)\n"
"  -f, --front                use the front S5K5E3 (ss1/31s/31c/i0s/scp)\n"
"  -m, --memory MODE          otf (default) or dma for the 3AA/ISP input\n"
"      --range MODE           full (default) or limited YUV->RGB range\n"
"      --hal-version VER      1.0 or 3.2 (default: 3.2, selects the DVFS\n"
"                             table; the metadata ABI is HAL 3.2 only)\n"
"      --node-ss NAME         override sensor video node name\n"
"      --node-3as NAME        override 3AA leader node name\n"
"      --node-3ac NAME        override 3AA capture node name\n"
"      --node-is NAME         override ISP leader node name\n"
"      --node-scp NAME        override SCP capture node name\n"
"  -A, --ae                   run a bounded closed-loop AE on the shot\n"
"                             metadata (ctl.aa.aeMode=OFF plus\n"
"                             ctl.sensor.exposureTime/sensitivity from the\n"
"                             measured SCP luma); the PPM is rewritten on\n"
"                             every iteration and holds the last frame\n"
"  -F, --ae-frames N          AE iterations, 1..60 (default: 10)\n"
"  -T, --ae-target N          target mean luma, 1..250 (default: 110)\n"
"  -D, --ae-tol N             convergence tolerance in luma steps\n"
"                             (default: 8)\n"
"      --ae-iso MIN:MAX       sensitivity bounds in ISO (default: 100:800)\n"
"      --ae-exposure-us MIN:MAX  exposure bounds in microseconds\n"
"                             (default: 100:90%% of the frame period)\n"
"  -W, --awb MODE             forward a white balance mode to the firmware\n"
"                             (off, auto, incandescent, fluorescent,\n"
"                             warm-fluorescent, daylight, cloudy, twilight,\n"
"                             shade); no AWB loop\n"
"  -v, --verbose              verbose ioctl logging\n"
"  -h, --help                 this help\n"
"\n"
"Note: no camera HAL and no JPEG.  The FIMC-IS firmware setfile provides\n"
"the ISP defaults.  Without --ae the shot metadata carries no 3A control\n"
"at all (aeMode/awbMode stay 0), exactly as before.  Firmware blobs must\n"
"be installed by firmware-samsung-j7elte (/lib/firmware/\n"
"{fimc_is_fw2_3l2.bin,setfile_3l2.bin,setfile_5e3.bin,fimc_is_fd.bin}).\n",
		progname);
}

static int parse_size(const char *s, unsigned *w, unsigned *h, unsigned *fps)
{
	char *end;

	errno = 0;
	*w = strtoul(s, &end, 0);
	if (errno || end == s || (*end != 'x' && *end != 'X'))
		return -1;
	s = end + 1;

	errno = 0;
	*h = strtoul(s, &end, 0);
	if (errno || end == s)
		return -1;

	*fps = 30;
	if (*end == '@') {
		s = end + 1;
		errno = 0;
		*fps = strtoul(s, &end, 0);
		if (errno || end == s)
			return -1;
	}

	return *end == '\0' ? 0 : -1;
}

static int parse_u64_range(const char *s, uint64_t *lo, uint64_t *hi)
{
	char *end;

	errno = 0;
	*lo = strtoull(s, &end, 0);
	if (errno || end == s)
		return -1;
	if (*end == '\0') {
		*hi = *lo;
		return 0;
	}
	if (*end != ':')
		return -1;

	s = end + 1;
	errno = 0;
	*hi = strtoull(s, &end, 0);
	if (errno || end == s || *end != '\0')
		return -1;

	return *hi >= *lo ? 0 : -1;
}

static int parse_u32_range(const char *s, unsigned *lo, unsigned *hi)
{
	uint64_t a, b;

	if (parse_u64_range(s, &a, &b) < 0)
		return -1;
	if (a > UINT_MAX || b > UINT_MAX)
		return -1;

	*lo = (unsigned)a;
	*hi = (unsigned)b;
	return 0;
}

static int parse_awb_mode(const char *s)
{
	static const struct {
		const char *name;
		int mode;
	} map[] = {
		{ "off",		AA_AWBMODE_OFF },
		{ "auto",		AA_AWBMODE_WB_AUTO },
		{ "incandescent",	AA_AWBMODE_WB_INCANDESCENT },
		{ "fluorescent",	AA_AWBMODE_WB_FLUORESCENT },
		{ "warm-fluorescent",	AA_AWBMODE_WB_WARM_FLUORESCENT },
		{ "daylight",		AA_AWBMODE_WB_DAYLIGHT },
		{ "cloudy",		AA_AWBMODE_WB_CLOUDY_DAYLIGHT },
		{ "twilight",		AA_AWBMODE_WB_TWILIGHT },
		{ "shade",		AA_AWBMODE_WB_SHADE },
	};
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(map); i++)
		if (strcmp(s, map[i].name) == 0)
			return map[i].mode;

	return -1;
}

static void parse_args(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "list",	no_argument,		NULL, 'l' },
		{ "output",	required_argument,	NULL, 'o' },
		{ "raw",	required_argument,	NULL, 'r' },
		{ "sensor-size", required_argument,	NULL, 's' },
		{ "out-size",	required_argument,	NULL, 'O' },
		{ "buffers",	required_argument,	NULL, 'n' },
		{ "timeout",	required_argument,	NULL, 't' },
		{ "front",	no_argument,		NULL, 'f' },
		{ "memory",	required_argument,	NULL, 'm' },
		{ "range",	required_argument,	NULL, 'R' },
		{ "hal-version", required_argument,	NULL, 'H' },
		{ "node-ss",	required_argument,	NULL, '1' },
		{ "node-3as",	required_argument,	NULL, '2' },
		{ "node-3ac",	required_argument,	NULL, '3' },
		{ "node-is",	required_argument,	NULL, '4' },
		{ "node-scp",	required_argument,	NULL, '5' },
		{ "ae",		no_argument,		NULL, 'A' },
		{ "ae-frames",	required_argument,	NULL, 'F' },
		{ "ae-target",	required_argument,	NULL, 'T' },
		{ "ae-tol",	required_argument,	NULL, 'D' },
		{ "ae-iso",	required_argument,	NULL, 1001 },
		{ "ae-exposure-us", required_argument,	NULL, 1002 },
		{ "awb",	required_argument,	NULL, 'W' },
		{ "verbose",	no_argument,		NULL, 'v' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	int c;

	while ((c = getopt_long(argc, argv, "lo:r:s:O:n:t:fm:vhAF:T:D:W:",
				long_opts, NULL)) != -1) {
		switch (c) {
		case 'l':
			opt.list = true;
			break;
		case 'o':
			opt.output = optarg;
			break;
		case 'r':
			opt.raw = optarg;
			break;
		case 's':
			if (parse_size(optarg, &opt.sensor_w, &opt.sensor_h,
				       &opt.sensor_fps) < 0) {
				msg("invalid --sensor-size \"%s\"", optarg);
				exit(EXIT_FAILURE);
			}
			opt.sensor_size_set = true;
			break;
		case 'O': {
			unsigned fps;

			if (parse_size(optarg, &opt.out_w, &opt.out_h,
				       &fps) < 0) {
				msg("invalid --out-size \"%s\"", optarg);
				exit(EXIT_FAILURE);
			}
			break;
		}
		case 'n':
			opt.nbufs = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 't':
			opt.timeout_s = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 'f':
			opt.front = true;
			break;
		case 'm':
			if (strcmp(optarg, "otf") == 0)
				opt.otf = true;
			else if (strcmp(optarg, "dma") == 0)
				opt.otf = false;
			else {
				msg("invalid --memory \"%s\" (otf|dma)", optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'R':
			if (strcmp(optarg, "full") == 0)
				opt.full_range = true;
			else if (strcmp(optarg, "limited") == 0)
				opt.full_range = false;
			else {
				msg("invalid --range \"%s\" (full|limited)",
				    optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'H':
			if (strcmp(optarg, "3.2") == 0)
				opt.hal_version = J7_IS_HAL_VER_3_2;
			else if (strcmp(optarg, "1.0") == 0)
				opt.hal_version = J7_IS_HAL_VER_1_0;
			else {
				msg("invalid --hal-version \"%s\" (1.0|3.2)",
				    optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case '1':
			opt.node_ss = optarg;
			break;
		case '2':
			opt.node_3as = optarg;
			break;
		case '3':
			opt.node_3ac = optarg;
			break;
		case '4':
			opt.node_is = optarg;
			break;
		case '5':
			opt.node_scp = optarg;
			break;
		case 'A':
			opt.ae = true;
			break;
		case 'F':
			opt.ae_frames = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 'T':
			opt.ae_target = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 'D':
			opt.ae_tol = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 1001:
			if (parse_u32_range(optarg, &opt.ae_iso_min,
					    &opt.ae_iso_max) < 0) {
				msg("invalid --ae-iso \"%s\" (MIN:MAX)", optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 1002:
			if (parse_u64_range(optarg, &opt.ae_exp_min_ns,
					    &opt.ae_exp_max_ns) < 0 ||
			    opt.ae_exp_max_ns > 1000000ull) {
				msg("invalid --ae-exposure-us \"%s\" (MIN:MAX, "
				    "at most 1000000 us)", optarg);
				exit(EXIT_FAILURE);
			}
			opt.ae_exp_min_ns *= 1000;
			opt.ae_exp_max_ns *= 1000;
			break;
		case 'W':
			opt.awb_mode = parse_awb_mode(optarg);
			if (opt.awb_mode < 0) {
				msg("invalid --awb \"%s\"", optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'v':
			opt.verbose = true;
			break;
		case 'h':
			usage(stdout);
			exit(EXIT_SUCCESS);
		default:
			usage(stderr);
			exit(EXIT_FAILURE);
		}
	}

	if (optind != argc) {
		msg("unexpected argument \"%s\"", argv[optind]);
		usage(stderr);
		exit(EXIT_FAILURE);
	}

	if (opt.nbufs < 3 || opt.nbufs > MAX_BUFFERS) {
		msg("--buffers must be between 3 and %u", MAX_BUFFERS);
		exit(EXIT_FAILURE);
	}

	/*
	 * The rear default mode (2072x1552@30) does not exist in the front
	 * S5K5E3 table, so fall back to its 1280x960@30 mode when --front is
	 * given without --sensor-size.
	 */
	if (opt.front && !opt.sensor_size_set) {
		opt.sensor_w = 1280;
		opt.sensor_h = 960;
		opt.sensor_fps = 30;
	}

	if (opt.timeout_s == 0) {
		msg("--timeout must be at least 1 second");
		exit(EXIT_FAILURE);
	}

	/*
	 * SCP YUV420M constraints from the driver: fimc_is_subdev_s_format()
	 * (fimc-is-subdev-ctrl.c) rejects a width that is not a multiple of
	 * 16 for V4L2_PIX_FMT_YUV420M, and the chroma planes are sized
	 * width * height / 4, which needs an even height.
	 */
	if (opt.out_w == 0 || opt.out_h == 0 ||
	    (opt.out_w % 16) != 0 || (opt.out_h % 2) != 0) {
		msg("output size must be a multiple of 16x2 for the SCP "
		    "YUV420M queue (got %ux%u)", opt.out_w, opt.out_h);
		exit(EXIT_FAILURE);
	}

	/* AE loop bounds: keep every value inside what the ABI can express. */
	if (opt.ae_frames < 1 || opt.ae_frames > AE_MAX_FRAMES) {
		msg("--ae-frames must be between 1 and %u", AE_MAX_FRAMES);
		exit(EXIT_FAILURE);
	}
	if (opt.ae_target < 1 || opt.ae_target > 250) {
		msg("--ae-target must be between 1 and 250");
		exit(EXIT_FAILURE);
	}
	if (opt.ae_tol > 64) {
		msg("--ae-tol must be at most 64");
		exit(EXIT_FAILURE);
	}
	if (opt.ae_iso_min < 1 || opt.ae_iso_max > 6400) {
		msg("--ae-iso must lie within 1:6400");
		exit(EXIT_FAILURE);
	}
	if (opt.ae_exp_min_ns < 1000 ||
	    opt.ae_exp_max_ns > 1000000000ull) {
		msg("--ae-exposure-us must lie within 1:1000000");
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char **argv)
{
	struct j7_pipeline pipeline;
	int ret;

	parse_args(argc, argv);

	if (opt.list) {
		list_nodes();
		return EXIT_SUCCESS;
	}

	if (!mode_supported(opt.front, opt.sensor_w, opt.sensor_h,
			    opt.sensor_fps)) {
		msg("no %s sensor mode is %ux%u with fps >= %u",
		    opt.front ? "front" : "rear", opt.sensor_w,
		    opt.sensor_h, opt.sensor_fps);
		print_supported_modes(opt.front);
		return EXIT_FAILURE;
	}

	/*
	 * The SCP scales the ISP output; asking for more than the sensor
	 * window would make fimc_is_ischain_scp_tag() clamp the crop output
	 * silently, so reject it here instead.
	 */
	if (opt.out_w > opt.sensor_w || opt.out_h > opt.sensor_h) {
		msg("output size %ux%u exceeds the sensor window %ux%u",
		    opt.out_w, opt.out_h, opt.sensor_w, opt.sensor_h);
		return EXIT_FAILURE;
	}

	/*
	 * The FIMC-IS driver registers no media_device (no media_entity /
	 * media_device_register calls in fimc-is2), so list what exists for
	 * diagnosis but do not depend on it.
	 */
	if (opt.verbose)
		list_nodes();

	msg("opening nodes (rear=%s, memory=%s)", opt.front ? "no" : "yes",
	    opt.otf ? "otf" : "dma");

	pipeline_open(&pipeline);

	ret = pipeline_configure(&pipeline);
	if (ret < 0) {
		msg("configuration failed");
		pipeline_close(&pipeline);
		return EXIT_FAILURE;
	}

	msg("starting sensor stream");
	ret = pipeline_start_sensor(&pipeline);
	if (ret < 0) {
		msg("sensor stream-on failed");
		pipeline_close(&pipeline);
		return EXIT_FAILURE;
	}

	msg("waiting for a frame (timeout %us)", opt.timeout_s);
	ret = pipeline_capture(&pipeline);
	if (ret < 0) {
		pipeline_close(&pipeline);
		return EXIT_FAILURE;
	}

	pipeline_close(&pipeline);
	return EXIT_SUCCESS;
}
