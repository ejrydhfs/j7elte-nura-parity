/*
 * Samsung FIMC-IS2 userspace ABI constants used by j7elte-cam.
 *
 * The driver exposes the ischain control interface through private V4L2
 * controls.  These are NOT in mainline linux/videodev2.h; the authoritative
 * definitions live in the Nura kernel tree:
 *
 *   include/uapi/linux/videodev2_exynos_camera_ext.h
 *   include/uapi/linux/videodev2_exynos_camera.h
 *   drivers/media/platform/exynos/fimc-is2/fimc-is-video.h
 *   drivers/media/platform/exynos/fimc-is2/fimc-is-device-sensor.h
 *   arch/arm64/mach-exynos/include/mach/exynos-fimc-is-sensor.h
 *
 * Values are copied here so the program can be built against a vanilla
 * (musl/postmarketOS) libc without the vendor UAPI headers installed.
 */

#ifndef J7ELTE_CAM_IOCTL_H
#define J7ELTE_CAM_IOCTL_H

#include <linux/videodev2.h>

/* videodev2_exynos_camera.h: V4L2_CID_FIMC_IS_BASE */
#define J7_FIMC_IS_BASE			(V4L2_CTRL_CLASS_CAMERA | 0x1000)

/* V4L2_CID_IS_* (videodev2_exynos_camera_ext.h) */
#define J7_CID_IS_END_OF_STREAM		(J7_FIMC_IS_BASE + 58)
#define J7_CID_IS_S_STREAM		(J7_FIMC_IS_BASE + 14)
#define J7_CID_IS_SET_SETFILE		(J7_FIMC_IS_BASE + 51)
#define J7_CID_IS_S_BNS			(J7_FIMC_IS_BASE + 52)
#define J7_CID_IS_HAL_VERSION		(J7_FIMC_IS_BASE + 902)

/* enum is_set_stream (same header) */
#define J7_IS_DISABLE_STREAM		0
#define J7_IS_ENABLE_STREAM		1

/* enum v4l2_is_hal_version */
#define J7_IS_HAL_VER_1_0		0
#define J7_IS_HAL_VER_3_2		1

/*
 * VIDIOC_S_INPUT encoding (fimc-is-video.h: INPUT_*).
 * Only the group leader nodes (30s/31s and i0s/i1s) consume this; the
 * capture nodes just validate it.
 */
#define J7_INPUT_STREAM_SHIFT		24
#define J7_INPUT_MODULE_SHIFT		16
#define J7_INPUT_VINDEX_SHIFT		8
#define J7_INPUT_MEMORY_SHIFT		4
#define J7_INPUT_LEADER_SHIFT		0

/*
 * The "memory" field is 0xF0 in the ABI.  Contrary to the header comment in
 * fimc-is-video.h, a non-zero value selects the on-the-fly path: the HAL's
 * ExynosCameraPipe::m_isOtf() returns (input & 0xF0) != 0, and the driver
 * only sets FIMC_IS_GROUP_OTF_INPUT when this field is non-zero
 * (fimc_is_group_init()).
 */
#define J7_INPUT_MEMORY_DMA		0
#define J7_INPUT_MEMORY_OTF		1

/* VIDIOC_S_INPUT encoding for the SSX (sensor) video node */
#define J7_SENSOR_SCENARIO_SHIFT	28
#define J7_SENSOR_SCENARIO_NORMAL	0
#define J7_SENSOR_NAME_S5K3L2		8	/* rear */
#define J7_SENSOR_NAME_S5K5E3		17	/* front */

/*
 * Video node numbers (fimc-is-video.h enum fimc_is_video_dev_num).  The
 * device minor is 100 + number (arch/arm64/mach-exynos/include/mach/
 * videonode-exynos5.h: EXYNOS_VIDEONODE_FIMC_IS) but the program resolves
 * the nodes through /sys/class/video4linux/videoN/name instead of hardcoding
 * /dev/videoN.
 */
#define J7_VID_SS0			1
#define J7_VID_SS1			2
#define J7_VID_30S			10
#define J7_VID_30C			11
#define J7_VID_30P			12
#define J7_VID_31S			20
#define J7_VID_31C			21
#define J7_VID_31P			22
#define J7_VID_I0S			30
#define J7_VID_I0C			31
#define J7_VID_I0P			32
#define J7_VID_I1S			40
#define J7_VID_I1C			41
#define J7_VID_I1P			42
#define J7_VID_DIS			50
#define J7_VID_SCC			51
#define J7_VID_SCP			52

/* v4l2_device/video node names created by fimc-is-video.c */
#define J7NODE_SS0			"exynos-fimc-is-ss0"
#define J7NODE_SS1			"exynos-fimc-is-ss1"
#define J7NODE_30S			"exynos-fimc-is-30s"
#define J7NODE_30C			"exynos-fimc-is-30c"
#define J7NODE_31S			"exynos-fimc-is-31s"
#define J7NODE_31C			"exynos-fimc-is-31c"
#define J7NODE_I0S			"exynos-fimc-is-i0s"
#define J7NODE_I0C			"exynos-fimc-is-i0c"
#define J7NODE_I1S			"exynos-fimc-is-i1s"
#define J7NODE_SCP			"exynos-fimc-is-scp"

/* Sensor modes, from sensor/module/fimc-is-device-{3l2,5e3}.c */
struct j7_sensor_mode {
	unsigned width;
	unsigned height;
	unsigned fps;
	unsigned mode;
};

#define J7_MODE(w, h, f, m) { .width = (w), .height = (h), .fps = (f), .mode = (m) }

#endif /* J7ELTE_CAM_IOCTL_H */
