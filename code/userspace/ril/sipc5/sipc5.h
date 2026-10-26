/* SPDX-License-Identifier: MIT */
/*
 * sipc5.h - SIPC5 link-layer codec and /dev/umts_* transport helpers for the
 *           Samsung Shannon SS310AP (Exynos 7580, j7elte).
 *
 * Everything here mirrors drivers/misc/modem_v1/include/sipc5.h (the kernel
 * that owns the wire framing on this device) and the payload layout used by
 * the stock libsec-ril.so.  See parity/userspace/ril/spec/framing.md for the
 * evidence trail and the exact addresses.
 *
 * Layer split on j7elte:
 *
 *   userspace payload  <- this library builds/parses only this part
 *        |
 *        +-- FMT/RFS IPC message: struct sipc5_fmt_hdr (7 bytes) + body
 *   ------------------ write(2)/read(2) boundary on /dev/umts_ipc0 ----------
 *        |
 *        +-- SIPC5 link header {cfg, ch, len[, ctrl|ext_len]}  <- kernel only
 *        +-- optional padding to 8 bytes                       <- kernel only
 *
 * The kernel adds the link header on write() and removes it again on read()
 * (modem_io_device.c:845-1030, 216-290), so a RIL never writes one.  The
 * link-layer codec is still provided because the same framing appears in
 * cbd/BOOT traffic, in ramdump captures and in any pcap-style trace, and
 * because the selftest pins the kernel's exact field layout.
 */

#ifndef J7ELTE_SIPC5_H
#define J7ELTE_SIPC5_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* SIPC5 link layer (kernel-owned; include/sipc5.h)                    */
/* ------------------------------------------------------------------ */

#define SIPC5_START_MASK	0xF8	/* cfg & 0xF8 == 0xF8 */
#define SIPC5_CONFIG_MASK	0x07
#define SIPC5_EXT_FIELD_MASK	0x03

#define SIPC5_PADDING_EXIST	0x04
#define SIPC5_EXT_FIELD_EXIST	0x02
#define SIPC5_CTL_FIELD_EXIST	0x01

#define SIPC5_MULTI_FRAME_CFG	0x03
#define SIPC5_EXT_LENGTH_CFG	0x02

#define SIPC5_MIN_HEADER_SIZE		4
#define SIPC5_HEADER_SIZE_WITH_CTL_FLD	5
#define SIPC5_HEADER_SIZE_WITH_EXT_LEN	6
#define SIPC5_MAX_HEADER_SIZE		6

/* The kernel pads to sizeof(size_t); this tree is arm64, so 8. */
#define SIPC5_ALIGN		8

/* Multi-frame control byte: bit7 = more, bits6..0 = reassembly id. */
#define SIPC5_CTRL_MORE		0x80
#define SIPC5_CTRL_ID_MASK	0x7F

/* Channel IDs (modem_v1.h:114-193). */
#define SIPC5_CH_ID_FMT_0	235
#define SIPC5_CH_ID_FMT_9	244
#define SIPC5_CH_ID_RFS_0	245
#define SIPC5_CH_ID_RFS_9	254
#define SIPC5_CH_ID_BOOT_0	215
#define SIPC5_CH_ID_DUMP_0	225

/* Device ioctls used by the stock RIL (modem_prj.h:74-105). */
#define SIPC5_IOCTL_MODEM_STATUS	0x6f27	/* _IO('o',0x27) */
#define SIPC5_IOCTL_MODEM_FORCE_CRASH	0x6f34	/* _IO('o',0x34) */

/* Decoded link-layer header. */
struct sipc5_link {
	uint8_t cfg;
	uint8_t ch;
	uint16_t len;	/* header + payload, padding NOT included */
	uint8_t ctrl;	/* valid only when cfg has MULTI_FRAME_CFG */
	uint16_t ext_len; /* valid only when cfg has EXT_LENGTH_CFG */
};

/* ------------------------------------------------------------------ */
/* FMT/RFS payload layer (userspace-owned)                             */
/* ------------------------------------------------------------------ */

/*
 * modem_prj.h:205-212, confirmed byte-for-byte against the stock builders
 * (libsec-ril.so IpcProtocol40::IpcTx*): 7 bytes, little-endian, len includes
 * the header itself and the body.
 */
struct sipc5_fmt_hdr {
	uint16_t len;
	uint8_t msg_seq;
	uint8_t ack_seq;
	uint8_t main_cmd;
	uint8_t sub_cmd;
	uint8_t cmd_type;
} __attribute__((packed));

#define SIPC5_FMT_HDR_LEN	7

/* Request cmd_type values (IpcProtocol40::GetCmdTypeName). */
#define SIPC5_CMD_EXEC		1
#define SIPC5_CMD_GET		2
#define SIPC5_CMD_SET		3
#define SIPC5_CMD_CFRM		4
#define SIPC5_CMD_EVENT		5

/* ------------------------------------------------------------------ */
/* Link-layer codec                                                    */
/* ------------------------------------------------------------------ */

bool sipc5_start_valid(uint8_t cfg);
unsigned sipc5_hdr_len(uint8_t cfg);
unsigned sipc5_frame_len(const uint8_t *hdr, size_t avail);
unsigned sipc5_pad_len(unsigned frame_len, bool padding_exist);
unsigned sipc5_total_len(const uint8_t *hdr, size_t avail);

/*
 * Parse a complete link frame.  `avail` must cover at least the header.
 * Returns 0 on success, -EINVAL for a malformed header, -ENOSPC when
 * `avail` is shorter than the encoded frame.
 */
int sipc5_link_parse(const uint8_t *buf, size_t avail, struct sipc5_link *out);

/*
 * Build a link header into `out` (must hold SIPC5_MAX_HEADER_SIZE bytes).
 * `payload_len` excludes the header and any padding.  Returns the header
 * length, or 0 if cfg is invalid.
 */
unsigned sipc5_link_build(uint8_t *out, size_t cap, uint8_t cfg, uint8_t ch,
			  unsigned payload_len, uint8_t ctrl);

/* ------------------------------------------------------------------ */
/* FMT payload helpers                                                 */
/* ------------------------------------------------------------------ */

/*
 * Build a complete FMT message (7-byte header + body) into `out`.
 * Returns the message length, or 0 on overflow.  `msg_seq`/`ack_seq` are the
 * values the stock builders leave at 0; a sender may stamp them later.
 */
size_t sipc5_fmt_build(uint8_t *out, size_t cap, uint8_t main_cmd,
		       uint8_t sub_cmd, uint8_t cmd_type, uint8_t msg_seq,
		       uint8_t ack_seq, const void *body, size_t body_len);

/*
 * Parse an FMT message.  Returns the declared message length, or 0 if the
 * buffer is shorter than SIPC5_FMT_HDR_LEN or the declared length is
 * inconsistent (< 7 or > avail).  `*body`/`*body_len` are set on success.
 */
size_t sipc5_fmt_parse(const uint8_t *buf, size_t avail,
		       struct sipc5_fmt_hdr *hdr, const uint8_t **body,
		       size_t *body_len);

/*
 * Reassembler for FMT/RFS payloads.
 *
 * The kernel strips the SIPC5 link header before queueing, and for a
 * multi-frame message it splices every fragment into the io_device RX queue
 * as a separate skb (gather_multi_frame(), modem_io_device.c:216-255), so one
 * userspace read() can return a *fragment* of a payload.  Fragments arrive in
 * order and the first one carries the 7-byte FMT header whose `len` is the
 * total message length, so accumulating until `len` bytes have been seen
 * reproduces the message.  That is what this helper does.
 */
struct sipc5_msg_asm {
	uint8_t *buf;
	size_t cap;
	size_t got;
	size_t want;	/* 0 until the FMT header has been seen */
};

void sipc5_asm_init(struct sipc5_msg_asm *a, void *buf, size_t cap);

/*
 * Feed one read() result.  Returns:
 *   1  a complete message is available in a->buf; *msglen is its length
 *   0  more fragments are needed
 *  -EINVAL  malformed/oversized message
 *  -EMSGSIZE fragment overruns the declared length
 * The reassembler resets itself after returning 1.
 */
int sipc5_asm_feed(struct sipc5_msg_asm *a, const void *frag, size_t n,
		   size_t *msglen);

/* ------------------------------------------------------------------ */
/* Transport: /dev/umts_ipc0, /dev/umts_rfs0                           */
/* ------------------------------------------------------------------ */

int sipc5_open(const char *path);
void sipc5_close(int fd);

/*
 * Write one payload as one SIPC5 frame.  The kernel's misc_write() turns a
 * single write() into one frame (umts_ipc0 has no ul_buffer_size, hence no
 * multi-frame chunking), so a short write is a protocol error, not something
 * to paper over.  Returns 0, or -errno.
 */
int sipc5_write_msg(int fd, const void *buf, size_t len);

/*
 * Read one payload (link header already stripped by the kernel).  Returns the
 * number of bytes, 0 on timeout, -ECONNRESET when the kernel reports POLLHUP
 * (CP crash/CRASH_EXIT for an IPC_FMT device), or another -errno.
 */
ssize_t sipc5_read_msg(int fd, void *buf, size_t cap, int timeout_ms);

int sipc5_modem_status(int fd);	/* ioctl 0x6f27 */
int sipc5_force_crash_exit(int fd); /* ioctl 0x6f34 */

/*
 * Open FMT and RFS in that order.  The kernel answers the CP's CP_START with
 * INIT_END only when both iod->opened counters are non-zero
 * (link_device_shmem.c:261-285, shmem_init_comm()), so a RIL that opens only
 * ipc0 leaves the CP retrying forever.  Returns 0 on success.
 */
int sipc5_open_channels(const char *fmt_path, const char *rfs_path,
			int *fmt_fd, int *rfs_fd);

#endif /* J7ELTE_SIPC5_H */
