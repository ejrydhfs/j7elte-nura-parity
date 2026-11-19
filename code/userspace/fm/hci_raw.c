/* bcmfm HCI raw transport: AF_BLUETOOTH raw socket, H4-style framing. */

#include "hci_raw.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* Prefer BlueZ headers when present; else fall back to inline defs. */
#if defined(__has_include)
#  if __has_include(<bluetooth/bluetooth.h>) && \
	__has_include(<bluetooth/hci.h>)
#    include <bluetooth/bluetooth.h>
#    include <bluetooth/hci.h>
#    define BCMFM_HAVE_BLUEZ 1
#  endif
#endif

#ifndef BCMFM_HAVE_BLUEZ
#  ifndef AF_BLUETOOTH
#    define AF_BLUETOOTH 31
#  endif
#  ifndef BTPROTO_HCI
#    define BTPROTO_HCI 1
#  endif
#  ifndef HCI_CHANNEL_RAW
#    define HCI_CHANNEL_RAW 0
#  endif
#  ifndef HCI_DEV_NONE
#    define HCI_DEV_NONE 0xFFFF
#  endif
struct bcmfm_sockaddr_hci {
	uint16_t hci_family;
	uint16_t hci_dev;
	uint16_t hci_channel;
};
#  define sockaddr_hci bcmfm_sockaddr_hci
#else
#  ifndef HCI_DEV_NONE
#    define HCI_DEV_NONE 0xFFFF
#  endif
#endif

/* HCI event header: event code + total param length (after 0x04 byte). */
#define EVT_HDR_LEN 3 /* 0x04 + evt + plen */

static int poll_wait(int fd, int timeout_ms)
{
	struct pollfd pfd;
	int r;

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	do {
		r = poll(&pfd, 1, timeout_ms);
	} while (r < 0 && errno == EINTR);
	if (r == 0)
		return -ETIMEDOUT;
	if (r < 0)
		return -errno;
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
		return -EIO;
	if (!(pfd.revents & POLLIN))
		return -ETIMEDOUT; /* defensive */
	return 0;
}

/* Read exactly len bytes with EINTR retry; fd must already be readable.
 * Short reads (SOCK_RAW datagram truncation should not happen, but
 * partial stream reads can) are looped until len or error/EOF. */
static ssize_t read_full(int fd, uint8_t *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t r = read(fd, buf + off, len - off);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return (ssize_t)-errno;
		}
		if (r == 0)
			return (ssize_t)-EIO; /* EOF on HCI socket */
		off += (size_t)r;
	}
	return (ssize_t)off;
}

static ssize_t write_full(int fd, const uint8_t *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t r = write(fd, buf + off, len - off);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return (ssize_t)-errno;
		}
		off += (size_t)r;
	}
	return (ssize_t)off;
}

int open_hci_raw(int dev_id)
{
	int fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	struct sockaddr_hci addr;
	int r;

	if (fd < 0)
		return -errno;
	if (dev_id < 0)
		return fd; /* unbound: caller handles device selection */

	memset(&addr, 0, sizeof(addr));
#ifdef BCMFM_HAVE_BLUEZ
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = (uint16_t)dev_id;
	addr.hci_channel = HCI_CHANNEL_RAW;
#else
	addr.hci_family = (uint16_t)AF_BLUETOOTH;
	addr.hci_dev = (uint16_t)dev_id;
	addr.hci_channel = (uint16_t)HCI_CHANNEL_RAW;
#endif
	r = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (r < 0) {
		r = -errno;
		close(fd);
		return r;
	}
	return fd;
}

void close_hci_raw(int fd)
{
	if (fd < 0)
		return;
	while (close(fd) < 0 && errno == EINTR)
		;
}

ssize_t hci_read_event(int fd, uint8_t *buf, size_t buflen, int timeout_ms)
{
	uint8_t hdr[EVT_HDR_LEN];
	size_t pkt_len;
	ssize_t r;

	if (buf == NULL || buflen < EVT_HDR_LEN)
		return -EINVAL;
	r = poll_wait(fd, timeout_ms);
	if (r < 0)
		return r;
	r = read_full(fd, hdr, sizeof(hdr));
	if (r < 0)
		return r;
	if (hdr[0] != BCMFM_HCI_EVENT_PKT)
		return -EBADMSG; /* raw channel should only yield events */
	pkt_len = (size_t)EVT_HDR_LEN + (size_t)hdr[2];
	if (pkt_len > buflen) {
		/* Drain the rest so the next read starts at a boundary. */
		uint8_t tmp[64];
		size_t left = pkt_len - (size_t)EVT_HDR_LEN;

		while (left > 0) {
			size_t n = left > sizeof(tmp) ? sizeof(tmp) : left;
			ssize_t k = read_full(fd, tmp, n);

			if (k < 0)
				return k;
			left -= (size_t)k;
		}
		return -EMSGSIZE;
	}
	memcpy(buf, hdr, sizeof(hdr));
	if (hdr[2] > 0) {
		r = read_full(fd, buf + EVT_HDR_LEN, hdr[2]);
		if (r < 0)
			return r;
	}
	return (ssize_t)pkt_len;
}

/*
 * Parse a Command Complete packet. On entry buf holds a full event
 * (leading 0x04). Outputs ncmd, opcode, and pointer/len of the return
 * parameters (status first). Returns 0 on success, -1 if not a valid
 * Command Complete.
 */
static int parse_cmd_complete(const uint8_t *buf, size_t len,
			      uint8_t *ncmd, uint16_t *opcode,
			      const uint8_t **params, size_t *params_len)
{
	uint8_t plen;

	if (len < (size_t)(EVT_HDR_LEN + 3))
		return -1;
	if (buf[0] != BCMFM_HCI_EVENT_PKT)
		return -1;
	if (buf[1] != BCMFM_EVT_CMD_COMPLETE)
		return -1;
	plen = buf[2];
	if ((size_t)plen + (size_t)EVT_HDR_LEN != len)
		return -1;
	if (plen < 3)
		return -1;
	*ncmd = buf[3];
	*opcode = (uint16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
	*params = buf + 6;
	*params_len = (size_t)plen - 3;
	return 0;
}

ssize_t hci_vs_cmd(int fd, uint8_t ogf, uint16_t ocf,
		   const uint8_t *payload, size_t plen,
		   uint8_t *rsp_buf, size_t rsplen,
		   int timeout_ms)
{
	uint8_t cmd[1 + 2 + 1 + BCMFM_HCI_MAX_CMD_LEN];
	uint8_t evt[BCMFM_HCI_MAX_EVENT_LEN];
	uint16_t opcode = BCMFM_OPCODE(ogf, ocf);
	ssize_t r;
	size_t cmd_len;

	if (fd < 0)
		return -EBADF;
	if (plen > BCMFM_HCI_MAX_CMD_LEN)
		return -EINVAL;
	if (plen > 0 && payload == NULL)
		return -EINVAL;

	/* H4-style command: 0x01 + opcode LE + plen + payload. */
	cmd[0] = BCMFM_HCI_COMMAND_PKT;
	cmd[1] = (uint8_t)(opcode & 0xFF);
	cmd[2] = (uint8_t)((opcode >> 8) & 0xFF);
	cmd[3] = (uint8_t)plen;
	if (plen > 0)
		memcpy(&cmd[4], payload, plen);
	cmd_len = (size_t)4 + plen;

	r = write_full(fd, cmd, cmd_len);
	if (r < 0)
		return r;

	/* Event loop: match Command Complete on our opcode. */
	for (;;) {
		/* Remaining budget handling: poll_wait consumes the full
		 * timeout per iteration; for simplicity each event wait
		 * gets the full timeout (documented behavior). */
		r = hci_read_event(fd, evt, sizeof(evt), timeout_ms);
		if (r < 0)
			return r;
		if (evt[1] == BCMFM_EVT_CMD_COMPLETE) {
			uint8_t ncmd = 0;
			uint16_t ec_op = 0;
			const uint8_t *params = NULL;
			size_t params_len = 0;

			(void)ncmd;
			if (parse_cmd_complete(evt, (size_t)r, &ncmd,
					       &ec_op, &params,
					       &params_len) < 0)
				continue; /* malformed; keep waiting */
			if (ec_op != opcode)
				continue; /* not ours; ignore */
			if (rsp_buf != NULL && rsplen > 0 && params_len > 0) {
				size_t n = params_len < rsplen
					? params_len : rsplen;

				memcpy(rsp_buf, params, n);
			}
			return (ssize_t)params_len;
		}
		/*
		 * Anything else inside the command window (Command Status,
		 * vendor 0xFF, LE meta, ...) is discarded; the caller
		 * drains async vendor events while idle via hci_read_event.
		 */
	}
}
