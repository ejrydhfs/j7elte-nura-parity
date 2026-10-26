// SPDX-License-Identifier: MIT
/*
 * j7elte-cbd - CP (baseband) boot loader for the Samsung Galaxy J7
 *              (SM-J700F/M, "j7elte", Exynos 7580 + Shannon SS310AP).
 *
 * Replacement for the stock Android /sbin/cbd, which cannot run on a musl
 * rootfs (bionic, statically linked).  It drives the kernel's
 * drivers/misc/modem_v1 SHMEM ("mem" link) interface to download the CP
 * firmware from the RADIO partition into the baseband.
 *
 * Everything below is derived from two sources only:
 *
 *  A) the kernel UAPI, drivers/misc/modem_v1:
 *     - ioctls        modem_prj.h:74-115
 *     - struct modem_firmware          modem_prj.h:169-176
 *     - struct std_dload_info          include/sipc5.h:250-254
 *     - shmem_xmit_boot()              link_device_shmem.c:1683  (mode 0 ->
 *       mld->boot_start + m_offset, mld->boot_size == shmem,ipc_offset)
 *     - shmem_start_download()         link_device_shmem.c:1762  (IOCTL_MODEM_DL_START)
 *     - shmem_send() IPC_BOOT -> xmit_udl() (raw byte pipe into the UDL ring,
 *       link_device_shmem.c:1524,946).  The BOOT iodev has iod,attrs = <0x02>
 *       (ATTR_SIPC5 only, no ATTR_NO_LINK_HEADER), so misc_write() prepends a
 *       4-byte SIPC5 link header to every write() (modem_io_device.c:879) and
 *       misc_read()/rx_raw_misc() strips it again: one UDL frame must be one
 *       write(), and userspace sees payloads only.
 *     - ss310ap_{on,boot_on,boot_off,boot_done}  modem_ctrl_ss310ap.c:202-360
 *
 *  B) the stock loader, boot_out/rd/sbin/cbd (ARM32, stripped; objdump +
 *     capstone, see parity/reports/ril-userspace.md for the addresses):
 *     - TOC parsing, stage table, and the SS310 boot sequence
 *       (`shannon_normal_boot' -> std_boot_load_loader/modem_on/dload_on/
 *        dload_start/dload/dload_off/done)
 *     - the CP bootloader byte protocol (0x9xxx request / 0xAxxx response)
 *
 * Hardware notes:
 *  - the CP must already be powered by IOCTL_MODEM_ON; the boot region is
 *    filled *before* power-on (stock order).
 *  - IOCTL_MODEM_BOOT_OFF blocks in the kernel for up to MIF_INIT_TIMEOUT
 *    (15 s) until the CP sends CMD_PHONE_START; that is expected.
 *  - calls/SMS/data need the RIL (SIPC5 FMT/RFS channels); this program only
 *    boots the baseband.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Kernel ABI: drivers/misc/modem_v1/modem_prj.h                       */
/* ------------------------------------------------------------------ */
#define IOCTL_MODEM_ON			_IO('o', 0x19)
#define IOCTL_MODEM_BOOT_ON		_IO('o', 0x22)
#define IOCTL_MODEM_BOOT_OFF		_IO('o', 0x23)
#define IOCTL_MODEM_BOOT_DONE		_IO('o', 0x24)
#define IOCTL_MODEM_STATUS		_IO('o', 0x27)
#define IOCTL_MODEM_DL_START		_IO('o', 0x28)
#define IOCTL_MODEM_FW_UPDATE		_IO('o', 0x29)
#define IOCTL_MODEM_XMIT_BOOT		_IO('o', 0x40)

/* modem_prj.h:169 - __packed, sizeof == 28 on both 32- and 64-bit ABIs. */
struct modem_firmware {
	uint64_t binary;	/* __user pointer */
	uint32_t size;		/* total image size (bounds check) */
	uint32_t m_offset;	/* offset inside the boot region */
	uint32_t b_offset;	/* unused by shmem_xmit_boot() */
	uint32_t mode;		/* 0 = boot region, 1 = whole shmem */
	uint32_t len;		/* bytes to copy from `binary' */
} __attribute__((packed));

/* Guard against ABI drift: _IO('o', nr) is (0x6f << 8) | nr on every
 * architecture, and the values below are the ones the kernel dispatches on
 * (verified against the ROM kernel's misc_ioctl jump table). */
_Static_assert(IOCTL_MODEM_ON == 0x6f19, "ioctl ABI");
_Static_assert(IOCTL_MODEM_BOOT_ON == 0x6f22, "ioctl ABI");
_Static_assert(IOCTL_MODEM_BOOT_OFF == 0x6f23, "ioctl ABI");
_Static_assert(IOCTL_MODEM_BOOT_DONE == 0x6f24, "ioctl ABI");
_Static_assert(IOCTL_MODEM_STATUS == 0x6f27, "ioctl ABI");
_Static_assert(IOCTL_MODEM_DL_START == 0x6f28, "ioctl ABI");
_Static_assert(IOCTL_MODEM_FW_UPDATE == 0x6f29, "ioctl ABI");
_Static_assert(IOCTL_MODEM_XMIT_BOOT == 0x6f40, "ioctl ABI");

/* include/sipc5.h:250 */
struct std_dload_info {
	uint32_t size;
	uint32_t mtu;
	uint32_t num_frames;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* CP image (RADIO partition) layout - reverse engineered from cbd     */
/* ------------------------------------------------------------------ */
/*
 * File offset 0: 0x80-byte TOC header, validated by cbd with strcmp on
 * "TOC" @0x00, "BOOT" @0x20, "MAIN" @0x40, "NV" @0x60.
 *   0x00 char magic[4] = "TOC\0"
 *   0x04 .. 0x13 reserved (0)
 *   0x14 u32 data_start   (0x200: first image offset == header size)
 *   0x18 u32 reserved
 *   0x1c u32 num_images   (1: number of flashable images)
 *   0x20 entry BOOT  (id 0)   load 0x40000000
 *   0x40 entry MAIN  (id 2)   load 0x40010000
 *   0x60 entry NV    (id 3)   load 0x44900000, no payload in the file
 *   0x80 entry OFFSET(id 4)   consumed by Samsung factory tools, unused here
 *
 * Entry (0x20 bytes):
 *   0x00 char name[12]
 *   0x0c u32  file offset of the image data
 *   0x10 u32  CP load address
 *   0x14 u32  size
 *   0x18 u32  CRC32 (sent verbatim to the CP in the stage CRC frame)
 *   0x1c u32  image id
 */
#define TOC_HDR_SIZE	0x80
#define IMG_NAME_LEN	12
#define IMG_ENTRY_SIZE	0x20

struct img_entry {
	char name[IMG_NAME_LEN];
	uint32_t offset;
	uint32_t load_addr;
	uint32_t size;
	uint32_t crc;
	uint32_t id;
} __attribute__((packed));

struct cp_toc {
	char magic[4];
	uint32_t reserved[4];
	uint32_t data_start;
	uint32_t reserved2;
	uint32_t num_images;
	struct img_entry boot;
	struct img_entry main;
	struct img_entry nv;
	struct img_entry offset;
} __attribute__((packed));

_Static_assert(sizeof(struct modem_firmware) == 28, "modem_firmware ABI");
_Static_assert(sizeof(struct std_dload_info) == 12, "std_dload_info ABI");
_Static_assert(sizeof(struct img_entry) == IMG_ENTRY_SIZE, "TOC entry size");
_Static_assert(sizeof(struct cp_toc) == 0xa0, "TOC header size");

/* ------------------------------------------------------------------ */
/* CP bootloader protocol (BOOT channel, raw bytes)                    */
/* ------------------------------------------------------------------ */
/*
 * Command word layout, from cbd:
 *   ctrl. request  = (stage << 8) | 0x9000   expected response = ...|0xA000
 *   ctrl. done     = (stage << 8) | 0x900D   expected response = ...|0xA00D
 *   data frame     = (stage << 8) | 0x9001   header {cmd, num_frames, seq, len}
 *   crc  frame     = (stage << 8) | 0x900C   header {cmd, crc}
 * Data frames are 16-byte header + `len' payload bytes, len <= 0x800 and
 * written with a single write() to /dev/umts_boot0.  mtu reported by
 * IOCTL_MODEM_FW_UPDATE is 0x810 (16 + 0x800).
 *
 * After the LAST data frame of a stage cbd writes nothing and waits for one
 * (stage << 8) | 0xA001; after the CRC frame it waits for
 * (stage << 8) | 0xA00C.  Individual data frames are not acknowledged
 * (cbd: std_udl_req_resp(fd, 0, resp) at 0x1d016 and 0x1d0c6).
 *
 * Stage table built by cbd's prepare_boot_args() (5 slots, stride 0x28,
 * flags: +0x04 start, +0x08 data, +0x0c crc, +0x10 done):
 *   0  BOOT  no framed payload (uploaded with IOCTL_MODEM_XMIT_BOOT
 *            before IOCTL_MODEM_ON); only the DONE handshake is sent
 *   1  TOC   file[0 .. data_start), no CRC frame
 *   2  MAIN  file[main.offset .. +main.size), CRC frame from the TOC
 *   3  NV    local NV file (/efs/nv_data.bin), size from the TOC, no CRC
 *   15       START handshake only (boot-finalise request)
 */
#define UDL_CMD_CTRL		0x9000
#define UDL_CMD_DATA		0x9001
#define UDL_CMD_CRC		0x900c
#define UDL_CMD_DONE		0x900d
#define UDL_RESP_DELTA		0x1000	/* 0x9xxx -> 0xAxxx */
#define UDL_MTU			0x810
#define UDL_MAX_PAYLOAD		0x800
#define UDL_FRAME_HDR		16

#define STAGE_BOOT		0
#define STAGE_TOC		1
#define STAGE_MAIN		2
#define STAGE_NV		3
#define STAGE_FINAL		15	/* cbd: start handshake only */

/* Defaults; overridable from the command line / /etc/conf.d/j7elte-cbd. */
#define DEF_BOOT_DEV		"/dev/umts_boot0"
#define DEF_NV_FILE		"/efs/nv_data.bin"
#define DEF_TIMEOUT_MS		30000	/* cbd: poll(..., 30000) */
#define DEF_TRIGGER_MS		3000	/* MODEM_ON -> CP status settle */

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */
static struct {
	const char *boot_dev;
	const char *image_path;
	const char *nv_path;
	int timeout_ms;
	int trigger_ms;
	bool verbose;
	bool dry_run;
	bool daemon;
	bool xmit_boot;
	bool create_nv;
	bool send_nv;
	int boot_fd;
	int image_fd;
} g = {
	.boot_dev = DEF_BOOT_DEV,
	.nv_path = DEF_NV_FILE,
	.timeout_ms = DEF_TIMEOUT_MS,
	.trigger_ms = DEF_TRIGGER_MS,
	.xmit_boot = true,
	.create_nv = true,
	.send_nv = true,
	.boot_fd = -1,
	.image_fd = -1,
};

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
static void logmsg(const char *fmt, ...)
{
	va_list ap;

	fputs("j7elte-cbd: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

#define LOG(...)  logmsg(__VA_ARGS__)
#define VLOG(...) do { if (g.verbose) logmsg(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static int xopen(const char *path, int flags)
{
	int fd;

	do {
		fd = open(path, flags);
	} while (fd < 0 && errno == EINTR);
	return fd;
}

/* Read exactly `len' bytes at the current position. */
static int read_full(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = read(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = ENODATA;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/*
 * Write to the BOOT channel, retrying while the kernel's UDL ring is full.
 * The kernel retries internally for a while (xmit_udl()), so ENOSPC here
 * really means "CP stopped draining": give up after the poll timeout.
 */
static int boot_write(const void *buf, size_t len)
{
	int64_t deadline = g.timeout_ms;

	for (;;) {
		if (write_full(g.boot_fd, buf, len) == 0)
			return 0;
		if (errno != ENOSPC && errno != EAGAIN && errno != EBUSY)
			return -1;
		if (deadline <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		usleep(10000);
		deadline -= 10;
	}
}

static void msleep(int ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* ------------------------------------------------------------------ */
/* Device helpers                                                      */
/* ------------------------------------------------------------------ */
static int cbd_ioctl(unsigned long req, void *arg, const char *name)
{
	int ret;

	if (g.dry_run) {
		LOG("[dry-run] ioctl %s(0x%lx)", name, req);
		return 0;
	}

	do {
		ret = ioctl(g.boot_fd, req, arg);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0)
		LOG("ioctl %s failed: %s", name, strerror(errno));
	else
		VLOG("ioctl %s ok", name);
	return ret;
}

/* Wait for one 4-byte response word from the CP bootloader. */
static int read_resp(uint32_t *resp, const char *what, unsigned stage)
{
	struct pollfd pfd = { .fd = g.boot_fd, .events = POLLIN };
	int ret;

	if (g.dry_run) {
		LOG("[dry-run] waiting for %s response (stage %u)", what, stage);
		*resp = 0;
		return 0;
	}

	ret = poll(&pfd, 1, g.timeout_ms);
	if (ret < 0) {
		if (errno == EINTR)
			return -1;
		LOG("%s: poll failed: %s", what, strerror(errno));
		return -1;
	}
	if (ret == 0) {
		LOG("%s: timeout after %d ms waiting for the CP", what,
		    g.timeout_ms);
		errno = ETIMEDOUT;
		return -1;
	}
	if (!(pfd.revents & POLLIN)) {
		LOG("%s: unexpected poll events 0x%x (CP crashed?)", what,
		    pfd.revents);
		errno = EIO;
		return -1;
	}

	if (read_full(g.boot_fd, resp, sizeof(*resp)) < 0) {
		LOG("%s: read failed: %s", what, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Wait for an unsolicited response word from the CP bootloader (no request is
 * written).  cbd's std_udl_req_resp(fd, 0, exp) does exactly this after the
 * last data frame of a stage and after a CRC frame: the CP acknowledges the
 * completed transfer, it does not ack individual frames.
 */
static int wait_resp(uint32_t exp, unsigned stage, const char *what)
{
	uint32_t resp = 0;

	VLOG("stage %u: waiting for %s ack 0x%08x", stage, what, exp);
	if (g.dry_run) {
		LOG("[dry-run] stage %u: wait for %s ack 0x%08x", stage, what,
		    exp);
		return 0;
	}
	if (read_resp(&resp, what, stage) < 0)
		return -1;
	if (resp != exp) {
		LOG("stage %u: %s ack got 0x%08x, expected 0x%08x", stage,
		    what, resp, exp);
		errno = EPROTO;
		return -1;
	}
	VLOG("stage %u: %s ack -> 0x%08x", stage, what, resp);
	return 0;
}

/*
 * Control command: write the request word, wait for the response word.
 * cbd's std_udl_req_resp(): write(fd, &req, 4); poll(); read(fd, &resp, 4).
 */
static int ctrl_req(uint32_t req, uint32_t exp, unsigned stage,
		    const char *what)
{
	VLOG("stage %u: %s req 0x%08x (expect 0x%08x)", stage, what, req, exp);
	if (g.dry_run) {
		LOG("[dry-run] stage %u: %s cmd 0x%08x", stage, what, req);
		return 0;
	}
	if (boot_write(&req, sizeof(req)) < 0) {
		LOG("stage %u: %s write failed: %s", stage, what,
		    strerror(errno));
		return -1;
	}
	return wait_resp(exp, stage, what);
}

static int stage_start(unsigned stage)
{
	uint32_t req = (stage << 8) | UDL_CMD_CTRL;	/* 0x9000 */

	return ctrl_req(req, req + UDL_RESP_DELTA /* 0xA000 */, stage,
			"START");
}

static int stage_done(unsigned stage)
{
	uint32_t req = (stage << 8) | UDL_CMD_DONE;	/* 0x900D */

	return ctrl_req(req, req + UDL_RESP_DELTA /* 0xA00D */, stage,
			"DONE");
}

/*
 * Byte source for a stage: either a file offset or an in-memory blob
 * (the NV image).  Keeping one frame loop for both means the frame layout
 * is exercised identically for every stage.
 */
struct stage_src {
	int fd;			/* >= 0: read from this file */
	const uint8_t *mem;	/* != NULL: read from memory instead */
	size_t pos;
	size_t size;
};

static int src_seek(struct stage_src *src, size_t off)
{
	if (src->mem) {
		if (off > src->size) {
			errno = EINVAL;
			return -1;
		}
		src->pos = off;
		return 0;
	}
	return lseek(src->fd, (off_t)off, SEEK_SET) < 0 ? -1 : 0;
}

static int src_read(struct stage_src *src, void *buf, size_t len)
{
	if (src->mem) {
		if (len > src->size - src->pos) {
			errno = ENODATA;
			return -1;
		}
		memcpy(buf, src->mem + src->pos, len);
		src->pos += len;
		return 0;
	}
	return read_full(src->fd, buf, len);
}

/*
 * Upload one stage: [offset, offset+size) of `src'.  The kernel is told the
 * geometry with IOCTL_MODEM_FW_UPDATE first (shmem_update_firm_info() ->
 * mld->img_info, link_device_shmem.c:1808) and the frames are written to the
 * BOOT channel with the CP bootloader's 16-byte frame headers.
 */
static int stage_upload(unsigned stage, struct stage_src *src,
			uint32_t offset, uint32_t size, bool send_crc,
			uint32_t crc)
{
	struct std_dload_info info = {
		.size = size,
		.mtu = UDL_MTU,
		.num_frames = (size + UDL_MAX_PAYLOAD - 1) / UDL_MAX_PAYLOAD,
	};
	uint8_t frame[UDL_FRAME_HDR + UDL_MAX_PAYLOAD];
	uint32_t hdr[4];
	uint32_t remaining = size;
	uint32_t seq = 0;

	if (src_seek(src, offset) < 0) {
		LOG("stage %u: seek(%u) failed: %s", stage, offset,
		    strerror(errno));
		return -1;
	}

	LOG("stage %u: %u bytes from offset 0x%x in %u frame(s)%s",
	    stage, size, offset, info.num_frames,
	    send_crc ? ", with CRC frame" : "");
	VLOG("stage %u: FW_UPDATE size=%u mtu=%u frames=%u", stage,
	     info.size, info.mtu, info.num_frames);

	if (!g.dry_run && cbd_ioctl(IOCTL_MODEM_FW_UPDATE, &info,
				    "MODEM_FW_UPDATE") < 0)
		return -1;

	while (remaining) {
		uint32_t len = remaining > UDL_MAX_PAYLOAD ?
			       UDL_MAX_PAYLOAD : remaining;

		memset(frame, 0, sizeof(frame));
		if (src_read(src, frame + UDL_FRAME_HDR, len) < 0) {
			LOG("stage %u: source read failed: %s", stage,
			    strerror(errno));
			return -1;
		}

		seq++;
		hdr[0] = (stage << 8) | UDL_CMD_DATA;
		hdr[1] = info.num_frames;
		hdr[2] = seq;
		hdr[3] = len;
		memcpy(frame, hdr, sizeof(hdr));

		if (g.dry_run) {
			if (seq <= 2 || remaining == len)
				LOG("[dry-run] stage %u frame %u/%u cmd=0x%04x len=%u",
				    stage, seq, info.num_frames,
				    (stage << 8) | UDL_CMD_DATA, len);
		} else if (boot_write(frame, UDL_FRAME_HDR + len) < 0) {
			LOG("stage %u: frame %u/%u write failed: %s", stage,
			    seq, info.num_frames, strerror(errno));
			return -1;
		}
		remaining -= len;
	}
	VLOG("stage %u: %u frame(s) sent", stage, seq);

	/* cbd waits for one unsolicited 0xA001 after the last data frame. */
	if (wait_resp((stage << 8) | (UDL_CMD_DATA + UDL_RESP_DELTA), stage,
		      "DATA") < 0)
		return -1;

	if (send_crc) {
		uint32_t crcframe[2] = { (stage << 8) | UDL_CMD_CRC, crc };

		VLOG("stage %u: CRC frame cmd=0x%04x crc=0x%08x", stage,
		     crcframe[0], crc);
		if (g.dry_run)
			LOG("[dry-run] stage %u CRC frame crc=0x%08x", stage,
			    crc);
		else if (boot_write(crcframe, sizeof(crcframe)) < 0) {
			LOG("stage %u: CRC write failed: %s", stage,
			    strerror(errno));
			return -1;
		}
		/* ... and for 0xA00C once the CRC has been checked. */
		if (wait_resp((stage << 8) | (UDL_CMD_CRC + UDL_RESP_DELTA),
			      stage, "CRC") < 0)
			return -1;
	}
	return 0;
}

static int run_stage(unsigned stage, struct stage_src *src, uint32_t offset,
		     uint32_t size, bool crc_en, uint32_t crc)
{
	if (size == 0) {
		LOG("stage %u: empty payload, skipped", stage);
		return 0;
	}
	if (stage_start(stage) < 0)
		return -1;
	if (stage_upload(stage, src, offset, size, crc_en, crc) < 0)
		return -1;
	return stage_done(stage);
}

/* ------------------------------------------------------------------ */
/* CP image                                                            */
/* ------------------------------------------------------------------ */
static int image_open(const char *path)
{
	int fd = xopen(path, O_RDONLY);

	if (fd < 0)
		return -1;
	g.image_fd = fd;
	g.image_path = path;
	return 0;
}

static int image_read_toc(struct cp_toc *toc)
{
	/* cbd reads 0x80 bytes here (read(fd, buf, 0x80)) and validates the
	 * three boot entries.  We additionally read the OFFSET entry at 0x80
	 * for --dump-toc; it belongs to Samsung factory tools and is never
	 * used for booting (the stage-1 payload still covers file[0 ..
	 * data_start), exactly like cbd). */
	memset(toc, 0, sizeof(*toc));
	if (lseek(g.image_fd, 0, SEEK_SET) < 0)
		return -1;
	if (read_full(g.image_fd, toc, sizeof(*toc)) < 0)
		return -1;

	if (memcmp(toc->magic, "TOC\0", 4) != 0) {
		LOG("%s: bad TOC magic (%02x %02x %02x %02x)", g.image_path,
		    toc->magic[0], toc->magic[1], toc->magic[2],
		    toc->magic[3]);
		return -1;
	}
	if (memcmp(toc->boot.name, "BOOT", 4) != 0) {
		LOG("%s: TOC entry 0 is not BOOT", g.image_path);
		return -1;
	}
	if (memcmp(toc->main.name, "MAIN", 4) != 0) {
		LOG("%s: TOC entry 1 is not MAIN", g.image_path);
		return -1;
	}
	if (memcmp(toc->nv.name, "NV", 2) != 0) {
		LOG("%s: TOC entry 2 is not NV", g.image_path);
		return -1;
	}
	if (toc->data_start < TOC_HDR_SIZE || toc->data_start > 0x1000000 ||
	    toc->main.size == 0) {
		LOG("%s: implausible TOC (data_start 0x%x, main size %u)",
		    g.image_path, toc->data_start, toc->main.size);
		return -1;
	}
	return 0;
}

static void toc_dump(const struct cp_toc *toc, uint64_t file_size)
{
	const struct img_entry *ent[4] = { &toc->boot, &toc->main, &toc->nv,
					   &toc->offset };
	int i;

	printf("CP image: %s (%" PRIu64 " bytes)\n", g.image_path, file_size);
	printf("TOC: data_start=0x%08x num_images=%u\n", toc->data_start,
	       toc->num_images);
	printf("%-8s %-4s %10s %10s %10s %10s\n",
	       "name", "id", "offset", "size", "load_addr", "crc");
	for (i = 0; i < 4; i++) {
		const struct img_entry *e = ent[i];

		if (!e->name[0])
			continue;
		printf("%-8.12s %-4u 0x%08x 0x%08x 0x%08x 0x%08x%s\n",
		       e->name, e->id, e->offset, e->size, e->load_addr,
		       e->crc,
		       (uint64_t)e->offset + e->size > file_size ?
		       "  (past EOF!)" : "");
	}
}

/* ------------------------------------------------------------------ */
/* NV image                                                            */
/* ------------------------------------------------------------------ */
/*
 * Stock cbd opens /efs/nv_data.bin and, if it does not exist, creates a
 * default (all-zero) file of exactly the NV size from the TOC, then sends
 * that file as stage 3.  We build the same default in memory instead and
 * never write to /efs: NV restoration is the RIL's job (RFS_NV_RESTORE),
 * not the loader's.
 */
static int nv_open(const struct cp_toc *toc, uint8_t **buf, uint32_t *len)
{
	int fd;
	uint8_t *b;

	fd = xopen(g.nv_path, O_RDONLY);
	if (fd >= 0) {
		b = malloc(toc->nv.size ? toc->nv.size : 1);
		if (!b) {
			close(fd);
			return -1;
		}
		if (read_full(fd, b, toc->nv.size) < 0) {
			LOG("%s: NV shorter than %u bytes: %s", g.nv_path,
			    toc->nv.size, strerror(errno));
			free(b);
			close(fd);
			return -1;
		}
		close(fd);
		*buf = b;
		*len = toc->nv.size;
		LOG("NV image: %s (%u bytes)", g.nv_path, toc->nv.size);
		return 0;
	}

	if (!g.create_nv) {
		LOG("NV image %s not readable: %s", g.nv_path,
		    strerror(errno));
		return -1;
	}

	LOG("WARNING: %s not found (%s); sending a zero-filled default NV of %u bytes",
	    g.nv_path, strerror(errno), toc->nv.size);
	LOG("WARNING: the modem will have no IMEI until NV is restored");
	b = calloc(1, toc->nv.size ? toc->nv.size : 1);
	if (!b)
		return -1;
	*buf = b;
	*len = toc->nv.size;
	return 0;
}

/* ------------------------------------------------------------------ */
/* CP image discovery                                                  */
/* ------------------------------------------------------------------ */
/*
 * The kernel has no PIT parser (no "by-name" string in the Nura tree), so
 * postmarketOS does not get the Android by-name links.  Try, in order:
 *   1. the GPT/udev label (CONFIG_EFI_PARTITION=y),
 *   2. the Android by-name link, if something created it,
 *   3. /dev/mbin0 - stock init.baseband.rc symlinks it to mmcblk0p5,
 *   4. mmcblk0p5 itself (RADIO, per init.baseband.rc),
 *   5. a firmware file, in case the blob is installed later.
 */
static const char *const cp_image_candidates[] = {
	"/dev/disk/by-partlabel/RADIO",
	"/dev/block/platform/13540000.dwmmc0/by-name/RADIO",
	"/dev/mbin0",
	"/dev/block/mmcblk0p5",
	"/lib/firmware/postmarketos/samsung-j7elte/modem.bin",
	NULL,
};

static const char *find_cp_image(void)
{
	int i;

	for (i = 0; cp_image_candidates[i]; i++) {
		struct cp_toc toc;
		int fd = xopen(cp_image_candidates[i], O_RDONLY);

		if (fd < 0)
			continue;
		if (lseek(fd, 0, SEEK_SET) == 0 &&
		    read_full(fd, &toc, TOC_HDR_SIZE) == 0 &&
		    memcmp(&toc, "TOC\0", 4) == 0) {
			close(fd);
			return cp_image_candidates[i];
		}
		close(fd);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Boot                                                                */
/* ------------------------------------------------------------------ */
static int xmit_bootloader(const struct cp_toc *toc)
{
	struct modem_firmware mf;
	uint8_t *img;
	int ret = -1;

	if (toc->boot.size == 0) {
		LOG("TOC has no BOOT image; skipping xmit");
		return 0;
	}

	img = malloc(toc->boot.size);
	if (!img) {
		LOG("out of memory for the BOOT image");
		return -1;
	}
	if (lseek(g.image_fd, (off_t)toc->boot.offset, SEEK_SET) < 0 ||
	    read_full(g.image_fd, img, toc->boot.size) < 0) {
		LOG("cannot read BOOT image (offset 0x%x size 0x%x): %s",
		    toc->boot.offset, toc->boot.size, strerror(errno));
		goto out;
	}

	memset(&mf, 0, sizeof(mf));
	mf.binary = (uint64_t)(uintptr_t)img;
	mf.size = toc->boot.size;
	mf.m_offset = 0;
	mf.b_offset = 0;
	mf.mode = 0;		/* shmem_xmit_boot(): boot_start + m_offset */
	mf.len = toc->boot.size;

	LOG("BOOT image: 0x%x bytes @file 0x%x -> shmem boot region",
	    toc->boot.size, toc->boot.offset);
	if (g.dry_run) {
		LOG("[dry-run] ioctl MODEM_XMIT_BOOT {binary=%p size=0x%x m_offset=0 mode=0 len=0x%x}",
		    (void *)img, mf.size, mf.len);
		ret = 0;
		goto out;
	}

	/* Must happen before IOCTL_MODEM_ON (stock order). */
	if (cbd_ioctl(IOCTL_MODEM_XMIT_BOOT, &mf, "MODEM_XMIT_BOOT") < 0)
		goto out;
	ret = 0;
out:
	free(img);
	return ret;
}

static int boot_cp(const struct cp_toc *toc)
{
	uint8_t *nv_buf = NULL;
	uint32_t nv_len = 0;
	struct stage_src img = { .fd = -1, .size = 0 };
	struct stage_src nv = { .fd = -1, .size = 0 };
	int ret = -1;

	if (g.send_nv) {
		if (nv_open(toc, &nv_buf, &nv_len) < 0)
			return -1;
	}

	if (!g.dry_run) {
		g.boot_fd = xopen(g.boot_dev, O_RDWR);
		if (g.boot_fd < 0) {
			LOG("%s: %s (is CONFIG_SEC_MODEM_V1 loaded?)",
			    g.boot_dev, strerror(errno));
			goto out;
		}
	} else {
		LOG("[dry-run] would open %s O_RDWR", g.boot_dev);
	}

	/* 1. bootloader into the CP boot region, before power-on. */
	if (g.xmit_boot && xmit_bootloader(toc) < 0)
		goto out;

	/* 2. power the CP and enter download mode (stock order). */
	if (cbd_ioctl(IOCTL_MODEM_ON, NULL, "MODEM_ON") < 0)
		goto out;
	if (!g.dry_run)
		msleep(g.trigger_ms);	/* ss310ap_on(): msleep(300) + settle */
	if (cbd_ioctl(IOCTL_MODEM_BOOT_ON, NULL, "MODEM_BOOT_ON") < 0)
		goto out;
	if (cbd_ioctl(IOCTL_MODEM_DL_START, NULL, "MODEM_DL_START") < 0)
		goto out;

	/* 3. stage 0 (BOOT): only the DONE handshake - the image is already
	 *    in the boot region and is validated by the CP's boot ROM. */
	LOG("stage 0 (BOOT): done handshake");
	if (stage_done(STAGE_BOOT) < 0)
		goto out;

	/* 4. stage 1 (TOC header): file[0 .. data_start), no CRC. */
	img.fd = g.image_fd;
	img.size = (size_t)lseek(g.image_fd, 0, SEEK_END);
	if (run_stage(STAGE_TOC, &img, 0, toc->data_start, false, 0) < 0)
		goto out;

	/* 5. stage 2 (MAIN): CRC frame carries the TOC CRC verbatim. */
	if (run_stage(STAGE_MAIN, &img, toc->main.offset, toc->main.size,
		      true, toc->main.crc) < 0)
		goto out;

	/* 6. stage 3 (NV).  The NV payload comes from a local file, not from
	 *    the CP image (cbd: NV stage fd = /efs/nv_data.bin, offset 0). */
	if (nv_len) {
		nv.mem = nv_buf;
		nv.size = nv_len;
		if (run_stage(STAGE_NV, &nv, 0, nv_len, false, 0) < 0)
			goto out;
	} else {
		LOG("no NV stage (--no-nv): the CP may reject the TOC");
	}

	/* 7. final stage: START-only handshake (cbd stage 15). */
	LOG("stage %u: final START handshake", STAGE_FINAL);
	if (stage_start(STAGE_FINAL) < 0)
		goto out;

	/* 7. wait for the CP to report CMD_PHONE_START (blocks up to 15 s in
	 *    the kernel), then finalise. */
	LOG("waiting for the CP to start (up to 15 s)");
	if (cbd_ioctl(IOCTL_MODEM_BOOT_OFF, NULL, "MODEM_BOOT_OFF") < 0) {
		LOG("the CP did not report PHONE_START; firmware may not have started");
		goto out;
	}
	if (cbd_ioctl(IOCTL_MODEM_BOOT_DONE, NULL, "MODEM_BOOT_DONE") < 0)
		goto out;

	LOG("CP boot complete");
	ret = 0;
out:
	free(nv_buf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Daemon mode: watch the BOOT channel for a CP state change           */
/* ------------------------------------------------------------------ */
static int watch_cp(void)
{
	struct pollfd pfd = { .fd = g.boot_fd, .events = POLLIN };

	LOG("watching %s for CP state changes", g.boot_dev);
	for (;;) {
		int ret = poll(&pfd, 1, -1);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			LOG("poll failed: %s", strerror(errno));
			return 1;
		}
		if (pfd.revents & POLLHUP) {
			LOG("CP reported a state change (POLLHUP: crash/offline)");
			LOG("no ramdump collection is implemented; exiting so the "
			    "service manager can restart the CP");
			return 1;
		}
		if (pfd.revents & POLLIN) {
			uint8_t junk[512];

			/* Drain; nothing is expected on the BOOT channel once
			 * the CP is online. */
			if (read(g.boot_fd, junk, sizeof(junk)) < 0 &&
			    errno != EAGAIN)
				return 1;
		}
	}
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */
static void usage(const char *argv0)
{
	fprintf(stderr,
"Usage: %s [options]\n"
"\n"
"Boot the SS310AP baseband of the Samsung Galaxy J7 (j7elte) by downloading\n"
"the CP firmware from the RADIO partition into the Exynos shmem boot region.\n"
"\n"
"Stock-compatible options (accepted so /sbin/cbd command lines work):\n"
"  -d                    stay resident and watch the CP after boot\n"
"  -t<type>              modem type; only 'ss310' is supported\n"
"  -b<link> -m<link>     boot/main link; only the SHMEM ('m') link exists\n"
"  -P<partition>         CP image partition (path below /dev/block/, or absolute)\n"
"\n"
"Options:\n"
"  -C, --cp-image FILE   CP image file (default: the RADIO partition)\n"
"  -B, --boot-dev PATH   modem boot device (default: " DEF_BOOT_DEV ")\n"
"  -N, --nv-file PATH    NV image (default: " DEF_NV_FILE ")\n"
"      --no-nv           do not send a NV stage at all\n"
"      --no-nv-create    fail instead of sending a zero-filled default NV\n"
"      --no-xmit-boot    do not upload the BOOT image (debugging)\n"
"  -T, --timeout SEC     CP response timeout (default: %d)\n"
"      --dump-toc        print the CP image table and exit\n"
"  -n, --dry-run         print the ioctl/frame sequence, touch no device\n"
"      --daemon          stay resident after boot (same as -d)\n"
"      --status          print the modem state and exit\n"
"  -v, --verbose         verbose logging\n"
"  -h, --help            this text\n",
		argv0, DEF_TIMEOUT_MS / 1000);
}

static int parse_args(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "cp-image",	required_argument, NULL, 'C' },
		{ "boot-dev",	required_argument, NULL, 'B' },
		{ "nv-file",	required_argument, NULL, 'N' },
		{ "timeout",	required_argument, NULL, 'T' },
		{ "daemon",	no_argument,	   NULL, 'd' },
		{ "dump-toc",	no_argument,	   NULL, 1000 },
		{ "dry-run",	no_argument,	   NULL, 'n' },
		{ "status",	no_argument,	   NULL, 1001 },
		{ "no-nv",	no_argument,	   NULL, 1002 },
		{ "no-nv-create", no_argument,	   NULL, 1003 },
		{ "no-xmit-boot", no_argument,	   NULL, 1004 },
		{ "verbose",	no_argument,	   NULL, 'v' },
		{ "help",	no_argument,	   NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	bool dump = false, status = false;
	struct cp_toc toc;
	uint64_t fsize;
	int c;

	while ((c = getopt_long(argc, argv, "dt:b:m:P:C:B:N:T:nvh",
				opts, NULL)) != -1) {
		switch (c) {
		case 'd':
			g.daemon = true;
			break;
		case 't':
			if (strncmp(optarg, "ss310", 5) != 0) {
				LOG("modem type '%s' is not supported (only ss310)",
				    optarg);
				return -1;
			}
			break;
		case 'b':
		case 'm':
			if (optarg[0] && optarg[0] != 'm') {
				LOG("link '%s' is not supported (only the SHMEM link 'm')",
				    optarg);
				return -1;
			}
			break;
		case 'P':
			if (optarg[0] == '/')
				g.image_path = optarg;
			else {
				static char path[512];

				snprintf(path, sizeof(path), "/dev/block/%s",
					 optarg);
				g.image_path = path;
			}
			break;
		case 'C':
			g.image_path = optarg;
			break;
		case 'B':
			g.boot_dev = optarg;
			break;
		case 'N':
			g.nv_path = optarg;
			break;
		case 'T':
			g.timeout_ms = atoi(optarg) * 1000;
			if (g.timeout_ms <= 0) {
				LOG("invalid timeout '%s'", optarg);
				return -1;
			}
			break;
		case 'n':
			g.dry_run = true;
			break;
		case 'v':
			g.verbose = true;
			break;
		case 1000:
			dump = true;
			break;
		case 1001:
			status = true;
			break;
		case 1002:
			g.send_nv = false;
			break;
		case 1003:
			g.create_nv = false;
			break;
		case 1004:
			g.xmit_boot = false;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			return -1;
		}
	}

	if (status) {
		int st;

		g.boot_fd = xopen(g.boot_dev, O_RDWR);
		if (g.boot_fd < 0) {
			LOG("%s: %s", g.boot_dev, strerror(errno));
			return -2;
		}
		st = ioctl(g.boot_fd, IOCTL_MODEM_STATUS, 0);
		if (st < 0) {
			LOG("IOCTL_MODEM_STATUS failed: %s", strerror(errno));
			return -2;
		}
		printf("modem state: %d\n", st);
		return 1;	/* handled */
	}

	if (!g.image_path) {
		g.image_path = find_cp_image();
		if (!g.image_path) {
			LOG("no RADIO partition with a CP image found; tried:");
			for (int i = 0; cp_image_candidates[i]; i++)
				LOG("  %s", cp_image_candidates[i]);
			LOG("use -C FILE or -P <partition>");
			return -1;
		}
		LOG("using CP image %s", g.image_path);
	}
	if (image_open(g.image_path) < 0) {
		LOG("%s: %s", g.image_path, strerror(errno));
		return -1;
	}
	if (image_read_toc(&toc) < 0)
		return -1;

	fsize = (uint64_t)lseek(g.image_fd, 0, SEEK_END);

	if (dump) {
		toc_dump(&toc, fsize);
		return 1;
	}

	if ((uint64_t)toc.boot.offset + toc.boot.size > fsize ||
	    (uint64_t)toc.main.offset + toc.main.size > fsize ||
	    toc.data_start > fsize) {
		LOG("%s: TOC points past the end of the file (%" PRIu64 " bytes)",
		    g.image_path, fsize);
		return -1;
	}

	if (boot_cp(&toc) < 0)
		return -2;

	if (g.daemon && !g.dry_run)
		return watch_cp() ? -2 : 0;

	return 0;
}

int main(int argc, char **argv)
{
	int ret = parse_args(argc, argv);

	if (g.image_fd >= 0)
		close(g.image_fd);
	if (g.boot_fd >= 0)
		close(g.boot_fd);

	/* 0 = booted, 1 = informational command handled, <0 = failure */
	return ret < 0 ? 1 : 0;
}
