/* SPDX-License-Identifier: GPL-2.0 */
/*
 * stub/drm.h — host-only stand-in for include/uapi/drm/drm.h.
 *
 * Real drm.h shape: DRM_IOW(nr, type) = _IOW(DRM_IOCTL_BASE, nr, type)
 * with the size taken via sizeof(type); panfrost_drm.h calls
 * DRM_IOW(DRM_COMMAND_BASE + N, struct ...). _IOC encoding matches
 * asm-generic (verified against the 3.10 tree's
 * include/asm-generic/ioctl.h: _IOC(dir,type,nr,size)).
 * NEVER used in any kernel build.
 */
#ifndef _ABIFUZZ_DRM_H_
#define _ABIFUZZ_DRM_H_

#include <linux/types.h>

#define ABF_IOC_NRBITS		8
#define ABF_IOC_TYPEBITS	8
#define ABF_IOC_SIZEBITS	14
#define ABF_IOC_DIRBITS		2

#define ABF_IOC_NRSHIFT		0
#define ABF_IOC_TYPESHIFT	8
#define ABF_IOC_SIZESHIFT	16
#define ABF_IOC_DIRSHIFT	30

#define ABF_IOC_NONE		0U
#define ABF_IOC_WRITE		1U
#define ABF_IOC_READ		2U

#define ABF_IOC(dir, type, nr, size) \
	(((dir) << ABF_IOC_DIRSHIFT) | ((type) << ABF_IOC_TYPESHIFT) | \
	 ((nr) << ABF_IOC_NRSHIFT) | ((size) << ABF_IOC_SIZESHIFT))

#define ABF_IOC_DIR(nr)		(((nr) >> ABF_IOC_DIRSHIFT) & 3U)
#define ABF_IOC_SIZE(nr)	(((nr) >> ABF_IOC_SIZESHIFT) & 0x3FFFU)

#define DRM_COMMAND_BASE	0x40
#define DRM_COMMAND_END		0xA0

#define DRM_IOCTL_BASE		'd'

#define DRM_IOW(nr, type) \
	ABF_IOC(ABF_IOC_WRITE, DRM_IOCTL_BASE, (nr), sizeof(type))
#define DRM_IOWR(nr, type) \
	ABF_IOC(ABF_IOC_READ | ABF_IOC_WRITE, DRM_IOCTL_BASE, (nr), \
		sizeof(type))

#endif /* _ABIFUZZ_DRM_H_ */
