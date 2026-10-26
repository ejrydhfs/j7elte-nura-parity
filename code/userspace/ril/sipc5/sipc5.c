// SPDX-License-Identifier: MIT
/*
 * sipc5.c - SIPC5 link-layer codec and /dev/umts_* transport helpers.
 *
 * Evidence for every constant: parity/userspace/ril/spec/framing.md.
 */

#include "sipc5.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Link layer                                                          */
/* ------------------------------------------------------------------ */

bool sipc5_start_valid(uint8_t cfg)
{
	return (cfg & SIPC5_START_MASK) == SIPC5_START_MASK;
}

unsigned sipc5_hdr_len(uint8_t cfg)
{
	if (!(cfg & SIPC5_EXT_FIELD_EXIST))
		return SIPC5_MIN_HEADER_SIZE;
	if ((cfg & SIPC5_EXT_FIELD_MASK) == SIPC5_MULTI_FRAME_CFG)
		return SIPC5_HEADER_SIZE_WITH_CTL_FLD;
	return SIPC5_HEADER_SIZE_WITH_EXT_LEN;
}

unsigned sipc5_frame_len(const uint8_t *hdr, size_t avail)
{
	if (avail < SIPC5_MIN_HEADER_SIZE)
		return 0;
	if (!(hdr[0] & SIPC5_EXT_FIELD_EXIST))
		return (unsigned)hdr[2] | ((unsigned)hdr[3] << 8);
	if ((hdr[0] & SIPC5_EXT_FIELD_MASK) == SIPC5_MULTI_FRAME_CFG)
		return (unsigned)hdr[2] | ((unsigned)hdr[3] << 8);
	if (avail < SIPC5_HEADER_SIZE_WITH_EXT_LEN)
		return 0;
	return (unsigned)hdr[2] | ((unsigned)hdr[3] << 8) |
	       ((unsigned)hdr[4] << 16) | ((unsigned)hdr[5] << 24);
}

unsigned sipc5_pad_len(unsigned frame_len, bool padding_exist)
{
	unsigned residue;

	if (!padding_exist)
		return 0;
	residue = frame_len & (SIPC5_ALIGN - 1);
	return residue ? SIPC5_ALIGN - residue : 0;
}

unsigned sipc5_total_len(const uint8_t *hdr, size_t avail)
{
	unsigned len = sipc5_frame_len(hdr, avail);

	if (!len)
		return 0;
	return len + sipc5_pad_len(len, (hdr[0] & SIPC5_PADDING_EXIST) != 0);
}

int sipc5_link_parse(const uint8_t *buf, size_t avail, struct sipc5_link *out)
{
	unsigned hdr_len, len, total;

	if (!buf || avail < SIPC5_MIN_HEADER_SIZE)
		return -ENOSPC;

	memset(out, 0, sizeof(*out));
	out->cfg = buf[0];
	if (!sipc5_start_valid(out->cfg))
		return -EINVAL;

	hdr_len = sipc5_hdr_len(out->cfg);
	if (avail < hdr_len)
		return -ENOSPC;

	out->ch = buf[1];
	len = sipc5_frame_len(buf, avail);
	if (len < hdr_len)
		return -EINVAL;

	if ((out->cfg & SIPC5_EXT_FIELD_EXIST) &&
	    (out->cfg & SIPC5_EXT_FIELD_MASK) == SIPC5_MULTI_FRAME_CFG)
		out->ctrl = buf[4];
	else if ((out->cfg & SIPC5_EXT_FIELD_EXIST) &&
		 (out->cfg & SIPC5_EXT_FIELD_MASK) == SIPC5_EXT_LENGTH_CFG)
		out->ext_len = (uint16_t)((unsigned)buf[4] | ((unsigned)buf[5] << 8));

	out->len = (uint16_t)len;

	total = len + sipc5_pad_len(len, (out->cfg & SIPC5_PADDING_EXIST) != 0);
	if (avail < total)
		return -ENOSPC;

	return 0;
}

unsigned sipc5_link_build(uint8_t *out, size_t cap, uint8_t cfg, uint8_t ch,
			  unsigned payload_len, uint8_t ctrl)
{
	unsigned hdr_len = sipc5_hdr_len(cfg);
	unsigned len = hdr_len + payload_len;

	if (!out || cap < hdr_len)
		return 0;
	if (!sipc5_start_valid(cfg))
		return 0;
	if (!(cfg & SIPC5_EXT_FIELD_EXIST) && len > 0xFFFF)
		return 0;

	out[0] = cfg;
	out[1] = ch;
	out[2] = (uint8_t)(len & 0xFF);
	out[3] = (uint8_t)((len >> 8) & 0xFF);
	if (hdr_len == SIPC5_HEADER_SIZE_WITH_CTL_FLD)
		out[4] = ctrl;
	else if (hdr_len == SIPC5_HEADER_SIZE_WITH_EXT_LEN) {
		/* EXT_LENGTH_CFG: 32-bit length field, unused by this device
		 * (the branch is compiled out in sipc5_build_config()). */
		out[2] = (uint8_t)(len & 0xFF);
		out[3] = (uint8_t)((len >> 8) & 0xFF);
		out[4] = (uint8_t)((len >> 16) & 0xFF);
		out[5] = (uint8_t)((len >> 24) & 0xFF);
	}
	return hdr_len;
}

/* ------------------------------------------------------------------ */
/* FMT payload layer                                                   */
/* ------------------------------------------------------------------ */

size_t sipc5_fmt_build(uint8_t *out, size_t cap, uint8_t main_cmd,
		       uint8_t sub_cmd, uint8_t cmd_type, uint8_t msg_seq,
		       uint8_t ack_seq, const void *body, size_t body_len)
{
	size_t total = SIPC5_FMT_HDR_LEN + body_len;

	if (!out || cap < total || total > 0xFFFF)
		return 0;

	out[0] = (uint8_t)(total & 0xFF);
	out[1] = (uint8_t)((total >> 8) & 0xFF);
	out[2] = msg_seq;
	out[3] = ack_seq;
	out[4] = main_cmd;
	out[5] = sub_cmd;
	out[6] = cmd_type;
	if (body_len)
		memcpy(out + SIPC5_FMT_HDR_LEN, body, body_len);
	return total;
}

size_t sipc5_fmt_parse(const uint8_t *buf, size_t avail,
		       struct sipc5_fmt_hdr *hdr, const uint8_t **body,
		       size_t *body_len)
{
	size_t len;

	if (!buf || avail < SIPC5_FMT_HDR_LEN)
		return 0;

	len = (size_t)buf[0] | ((size_t)buf[1] << 8);
	if (len < SIPC5_FMT_HDR_LEN || len > avail)
		return 0;

	if (hdr) {
		hdr->len = (uint16_t)len;
		hdr->msg_seq = buf[2];
		hdr->ack_seq = buf[3];
		hdr->main_cmd = buf[4];
		hdr->sub_cmd = buf[5];
		hdr->cmd_type = buf[6];
	}
	if (body)
		*body = buf + SIPC5_FMT_HDR_LEN;
	if (body_len)
		*body_len = len - SIPC5_FMT_HDR_LEN;
	return len;
}

/* ------------------------------------------------------------------ */
/* Reassembler                                                         */
/* ------------------------------------------------------------------ */

void sipc5_asm_init(struct sipc5_msg_asm *a, void *buf, size_t cap)
{
	a->buf = buf;
	a->cap = cap;
	a->got = 0;
	a->want = 0;
}

int sipc5_asm_feed(struct sipc5_msg_asm *a, const void *frag, size_t n,
		   size_t *msglen)
{
	const uint8_t *p = frag;
	size_t room, take;

	if (!a || !a->buf || (!frag && n))
		return -EINVAL;
	if (!n)
		return 0;

	if (a->want == 0) {
		size_t need = SIPC5_FMT_HDR_LEN - a->got;

		take = n < need ? n : need;
		if (a->got + take > a->cap)
			return -EINVAL;
		memcpy(a->buf + a->got, p, take);
		a->got += take;
		p += take;
		n -= take;

		if (a->got < SIPC5_FMT_HDR_LEN)
			return 0;

		a->want = (size_t)a->buf[0] | ((size_t)a->buf[1] << 8);
		if (a->want < SIPC5_FMT_HDR_LEN || a->want > a->cap)
			return -EINVAL;
	}

	room = a->want - a->got;
	if (n > room)
		return -EMSGSIZE;

	memcpy(a->buf + a->got, p, n);
	a->got += n;

	if (a->got < a->want)
		return 0;

	if (msglen)
		*msglen = a->want;
	a->got = 0;
	a->want = 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

int sipc5_open(const char *path)
{
	int fd;

	do {
		fd = open(path, O_RDWR | O_CLOEXEC);
	} while (fd < 0 && errno == EINTR);
	return fd;
}

void sipc5_close(int fd)
{
	if (fd >= 0)
		close(fd);
}

static int wait_writable(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLOUT };

	for (;;) {
		int r = poll(&p, 1, 1000);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			return -errno;
		if (r == 0)
			return -ETIMEDOUT;
		if (p.revents & (POLLERR | POLLNVAL))
			return -EIO;
		return 0;
	}
}

int sipc5_write_msg(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t done = 0;

	if (fd < 0)
		return -EBADF;
	if (!buf || len == 0 || len > 0xFFFF)
		return -EINVAL;

	/*
	 * misc_write() copies its whole argument and emits exactly one frame,
	 * so the only acceptable outcomes are "all bytes" or an error.  Never
	 * split one payload across two write() calls.
	 */
	while (done < len) {
		ssize_t n = write(fd, p + done, len - done);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				int r = wait_writable(fd);

				if (r)
					return r;
				continue;
			}
			return -errno;
		}
		if (n == 0)
			return -EIO;
		done += (size_t)n;
	}
	return 0;
}

ssize_t sipc5_read_msg(int fd, void *buf, size_t cap, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	int r;

	if (fd < 0)
		return -EBADF;
	if (!buf || cap == 0)
		return -EINVAL;

	for (;;) {
		r = poll(&p, 1, timeout_ms);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			return -errno;
		if (r == 0)
			return 0;
		/* misc_poll() reports POLLHUP for IPC_FMT on CP crash. */
		if (p.revents & POLLHUP)
			return -ECONNRESET;
		if (p.revents & (POLLERR | POLLNVAL))
			return -EIO;
		if (p.revents & POLLIN)
			break;
	}

	for (;;) {
		ssize_t n = read(fd, buf, cap);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			return -errno;
		}
		return n;
	}
}

int sipc5_modem_status(int fd)
{
	if (fd < 0)
		return -EBADF;
	if (ioctl(fd, SIPC5_IOCTL_MODEM_STATUS) < 0)
		return -errno;
	return 0;
}

int sipc5_force_crash_exit(int fd)
{
	if (fd < 0)
		return -EBADF;
	if (ioctl(fd, SIPC5_IOCTL_MODEM_FORCE_CRASH) < 0)
		return -errno;
	return 0;
}

int sipc5_open_channels(const char *fmt_path, const char *rfs_path,
			int *fmt_fd, int *rfs_fd)
{
	int f, r;

	*fmt_fd = -1;
	*rfs_fd = -1;

	f = sipc5_open(fmt_path);
	if (f < 0)
		return -errno;
	r = sipc5_open(rfs_path);
	if (r < 0) {
		int err = errno;

		close(f);
		return -err;
	}

	*fmt_fd = f;
	*rfs_fd = r;
	return 0;
}
