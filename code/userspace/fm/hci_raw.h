#ifndef BCMFM_HCI_RAW_H
#define BCMFM_HCI_RAW_H

/*
 * bcmfm HCI raw transport.
 *
 * Raw HCI socket framing for Broadcom vendor-specific opcode 0xFC15
 * (FM radio on BCM43438). This layer knows packet framing, send, and
 * event filtering only. FM subcommand semantics (payload [cmd, arg, ...])
 * live in the batch-2 subcommand layer, not here.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* H4 / HCI packet indicators (on-wire first byte on raw socket). */
#define BCMFM_HCI_COMMAND_PKT 0x01
#define BCMFM_HCI_ACL_PKT     0x02
#define BCMFM_HCI_SCO_PKT     0x03
#define BCMFM_HCI_EVENT_PKT   0x04

/* HCI event codes we care about. */
#define BCMFM_EVT_CMD_COMPLETE 0x0E
#define BCMFM_EVT_CMD_STATUS   0x0F
#define BCMFM_EVT_VENDOR       0xFF

/* Vendor-specific opcode group + FM opcode. OGF=0x3F, OCF=0x015. */
#define BCMFM_OGF_VENDOR 0x3F
#define BCMFM_OCF_FM     0x015
#define BCMFM_OP_FM_VS   0xFC15 /* (OGF_VENDOR << 10) | OCF_FM */

/* Build a 16-bit HCI opcode from OGF/OCF. */
#define BCMFM_OPCODE(ogf, ocf) \
	((uint16_t)(((uint16_t)(ogf) << 10) | ((uint16_t)(ocf) & 0x03FF)))

/* Max HCI command payload (excluding 0x01 + opcode + plen header). */
#define BCMFM_HCI_MAX_CMD_LEN 255
/* Max raw HCI event packet incl. 1-byte packet indicator. */
#define BCMFM_HCI_MAX_EVENT_LEN 260

/*
 * Open a raw HCI socket bound to hci dev_id, channel HCI_CHANNEL_RAW.
 * dev_id: HCI device index (0 for hci0); pass -1 for HCI_DEV_NONE
 *         (no bind, send path must not assume a device).
 * Returns fd >= 0 on success, or negative -errno on failure.
 */
int open_hci_raw(int dev_id);

/*
 * Read one raw HCI event packet (including leading 0x04 indicator)
 * into buf[0..buflen).
 * timeout_ms < 0 blocks indefinitely; 0 polls once.
 * Returns packet length (>= 3) on success, negative -errno on failure
 * (-ETIMEDOUT on timeout, -EMSGSIZE if buflen too small, -EBADMSG on
 * short/truncated packet).
 * Vendor events (0xFF) are passed through to the caller unfiltered.
 */
ssize_t hci_read_event(int fd, uint8_t *buf, size_t buflen, int timeout_ms);

/*
 * Send a vendor-specific HCI command and wait for its Command Complete.
 * opcode = BCMFM_OPCODE(ogf, ocf); plen bytes from payload (may be NULL
 * when plen == 0).
 *
 * On success, the Command Complete return parameters (the bytes after
 * ncmd + opcode, i.e. status + vendor payload) are copied to rsp_buf
 * (up to rsplen bytes) and the full return-parameter length is returned
 * (>= 1, first byte is status). If rsplen is smaller, parameters are
 * truncated to fit but the full length is still returned.
 *
 * While waiting, Command Complete events for other opcodes and Command
 * Status events are ignored; vendor (0xFF) events arriving inside the
 * command window are discarded. Drain async vendor events with
 * hci_read_event() when idle.
 *
 * timeout_ms < 0 blocks indefinitely; 0 sends and polls once.
 * Returns return-parameter length on success, negative -errno on failure
 * (-ETIMEDOUT on timeout, -EIO on transport/command error).
 */
ssize_t hci_vs_cmd(int fd, uint8_t ogf, uint16_t ocf,
		   const uint8_t *payload, size_t plen,
		   uint8_t *rsp_buf, size_t rsplen,
		   int timeout_ms);

/* Close helper: close(fd) with EINTR retry; kept here for symmetry. */
void close_hci_raw(int fd);

#ifdef __cplusplus
}
#endif

#endif /* BCMFM_HCI_RAW_H */
