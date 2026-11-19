/* bcmfm: FM subcommand layer over hci_raw.h (BCM43438, HCI VS 0xFC15).
 *
 * Byte map: fc15-table.md sec 1. Each function cites its table row.
 * Transport (framing + Command Complete wait + status) is hci_raw.h.
 */

#include "bcmfm.h"
#include "hci_raw.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Process-local shadow of cached cb+0xa6 audio word (re-notes.txt item 4:
 * bit positions observed, names inferred). Stock stack keeps it in RAM
 * and RMWs bits; we do the same, seeded at the init_delay value 0x001c. */
static uint16_t aud_shadow = 0x001c;

uint8_t bcmfm_read_arg(uint8_t r1)
{
	uint8_t a = (r1 & 8) ? BCMFM_ARG_R_2B : BCMFM_ARG_R_1B;

	if (r1 & 0x80)
		a |= BCMFM_ARG_RD;
	return a;
}

uint16_t bcmfm_region_band(int region, uint16_t *lo, uint16_t *hi)
{
	uint16_t l = 8750, h = 10800;

	switch (region) {
	case BCMFM_REGION_US:
	case BCMFM_REGION_EU:
		l = 8750; h = 10800; break;
	case BCMFM_REGION_JP:
		l = 7600; h = 9000; break;
	case BCMFM_REGION_WORLD:
		l = 7600; h = 10800; break;
	default:
		break;
	}
	if (lo)
		*lo = l;
	if (hi)
		*hi = h;
	return l;
}

const char *bcmfm_strerror(int rc)
{
	if (rc >= 0)
		return "ok";
	switch (-rc) {
	case ETIMEDOUT: return "command complete timeout";
	case EIO:       return "transport or controller error";
	case EBADF:     return "device not open";
	case EINVAL:    return "invalid argument";
	case ENOMEM:    return "out of memory";
	default:        return "error";
	}
}

int bcmfm_open(struct bcmfm_state *s, int dev_id, int timeout_ms)
{
	int fd;

	if (!s)
		return -EINVAL;
	fd = open_hci_raw(dev_id);
	if (fd < 0)
		return fd;
	s->fd = fd;
	s->dev_id = dev_id;
	s->timeout_ms = timeout_ms;
	return 0;
}

void bcmfm_close(struct bcmfm_state *s)
{
	if (!s)
		return;
	if (s->fd >= 0)
		close_hci_raw(s->fd);
	s->fd = -1;
}

int bcmfm_cmd(struct bcmfm_state *s, const uint8_t *payload, size_t plen,
	      struct bcmfm_rsp *rsp)
{
	uint8_t buf[256];
	ssize_t r;

	if (!s || s->fd < 0)
		return -EBADF;
	if (!payload || plen < 2 || plen > BCMFM_HCI_MAX_CMD_LEN)
		return -EINVAL;
	/* Table sec 0: every FC15 payload starts [cmd, arg, ...]. */
	r = hci_vs_cmd(s->fd, BCMFM_OGF_VENDOR, BCMFM_OCF_FM,
		       payload, plen, buf, sizeof(buf), s->timeout_ms);
	if (r < 0)
		return (int)r;
	if (r < 1)
		return -EIO;
	if (rsp) {
		size_t n = (size_t)r < sizeof(rsp->buf) ? (size_t)r : sizeof(rsp->buf);

		memcpy(rsp->buf, buf, n);
		rsp->status = buf[0];
		rsp->len = (size_t)r;
	}
	if (buf[0] != 0)
		return (int)buf[0]; /* positive HCI status byte */
	return 0;
}

int bcmfm_raw(struct bcmfm_state *s, const uint8_t *payload, size_t plen,
	      struct bcmfm_rsp *rsp)
{
	return bcmfm_cmd(s, payload, plen, rsp);
}

/* ---- payload builders (table sec 0 helper encoding) ---- */
static size_t mk_w1b(uint8_t *o, uint8_t cmd, uint8_t d, uint8_t flags)
{
	o[0] = cmd; o[1] = (uint8_t)(flags | BCMFM_ARG_W1B); o[2] = d;
	return 3;
}

static size_t mk_w2b(uint8_t *o, uint8_t cmd, uint16_t w, uint8_t flags)
{
	o[0] = cmd; o[1] = (uint8_t)(flags | BCMFM_ARG_W2B);
	o[2] = (uint8_t)(w & 0xFF); o[3] = (uint8_t)(w >> 8);
	return 4;
}

static size_t mk_w4b(uint8_t *o, uint16_t lo, uint16_t hi, uint8_t flags)
{
	o[0] = BCMFM_SUB_BAND; o[1] = (uint8_t)(flags | BCMFM_ARG_W4B);
	o[2] = (uint8_t)(lo & 0xFF); o[3] = (uint8_t)(lo >> 8);
	o[4] = (uint8_t)(hi & 0xFF); o[5] = (uint8_t)(hi >> 8);
	return 6;
}

static size_t mk_rd(uint8_t *o, uint8_t cmd, uint8_t r1)
{
	o[0] = cmd; o[1] = bcmfm_read_arg(r1);
	return 2;
}

/* ---- power / setup ---- */

/* Table row 0x00: bta_fm_enable = W1B(0x00, r7=1/3, 0).
 * Data 1|3 = region class via `tst r2,#0x30` (re-notes.txt item 3:
 * which region is which is [INFERENCE]). */
int bcmfm_enable(struct bcmfm_state *s, int region)
{
	uint8_t p[3];
	uint8_t cls = (region == BCMFM_REGION_US) ? 1 : 3;

	mk_w1b(p, BCMFM_SUB_POWER_INIT, cls, 0x00);
	return bcmfm_cmd(s, p, sizeof(p), NULL);
}

/* Table row 0x4D/0x05/0x00: bta_fm_disable = W1B(0x4D, a5&0x7f, 0) +
 * W2B(0x05, 2, 8) + W1B(0x00, 0, 0x88). a5 cache unreadable from
 * userspace; 0 keeps the power-domain bit clear [INFERENCE]. */
int bcmfm_disable(struct bcmfm_state *s)
{
	uint8_t p[4];
	int rc;

	mk_w1b(p, BCMFM_SUB_AUDIOPATH, 0x00, 0x00);
	rc = bcmfm_cmd(s, p, 3, NULL);
	if (rc)
		return rc;
	mk_w2b(p, BCMFM_SUB_AUDCTL, 0x0002, 0x08);
	rc = bcmfm_cmd(s, p, sizeof(p), NULL);
	if (rc)
		return rc;
	mk_w1b(p, BCMFM_SUB_POWER_INIT, 0x00, 0x88);
	return bcmfm_cmd(s, p, 3, NULL);
}

/* Table row 0x01 + 0xFB: sub_2264 = W1B(0x01, class, 0x88) +
 * W4B(0xFB, lo, hi, 0x08); band edges from the region table
 * (bta_fm_cfg). Class mapping EU/US->observed 1|3 is [INFERENCE]. */
int bcmfm_set_region(struct bcmfm_state *s, int region)
{
	uint8_t p[6];
	uint16_t lo, hi;
	uint8_t cls;
	int rc;

	bcmfm_region_band(region, &lo, &hi);
	cls = (region == BCMFM_REGION_US) ? 1 : 3;
	mk_w1b(p, BCMFM_SUB_REGION, cls, 0x88);
	rc = bcmfm_cmd(s, p, 3, NULL);
	if (rc)
		return rc;
	mk_w4b(p, lo, hi, 0x08);
	return bcmfm_cmd(s, p, sizeof(p), NULL);
}

/* Table row 0xFD: bta_fm_set_step = W2B(0xFD, table[r3], 0x88);
 * table bcmfm_sch_step_size = 100/50 kHz (re-notes.txt). 200 kHz
 * encoding 0xC8 is [INFERENCE]. */
int bcmfm_set_step(struct bcmfm_state *s, int step_khz)
{
	uint8_t p[4];
	uint16_t v;

	switch (step_khz) {
	case BCMFM_STEP_50KHZ:  v = 0x32; break;
	case BCMFM_STEP_200KHZ: v = 0xC8; break;
	case BCMFM_STEP_100KHZ:
	default:                v = 0x64; break;
	}
	mk_w2b(p, BCMFM_SUB_STEP, v, 0x88);
	return bcmfm_cmd(s, p, sizeof(p), NULL);
}

/* Table audio-path row: bta_fm_set_audio_path = W1B(0x4D, a5', 8) +
 * W2B(0x05, word|0x20/&~0x20 (I2S?) |+/-0x10, 0x88). Path bit placement
 * in the word is [INFERENCE]; low 2 bits carry path 0..3. */
int bcmfm_set_audio_path(struct bcmfm_state *s, int path)
{
	uint8_t p[4];
	uint16_t word;
	int rc;

	if (path < 0 || path > 3)
		return -EINVAL;
	mk_w1b(p, BCMFM_SUB_AUDIOPATH, (uint8_t)(path & 0x7F), 0x08);
	rc = bcmfm_cmd(s, p, 3, NULL);
	if (rc)
		return rc;
	word = (uint16_t)(0x001C | (path & 0x03));
	aud_shadow = word;
	mk_w2b(p, BCMFM_SUB_AUDCTL, word, 0x88);
	return bcmfm_cmd(s, p, sizeof(p), NULL);
}

/* Table de-emphasis row: set_deemphasis toggles bit6 (0x40) of cb+0xa6
 * via W2B(0x05) and sends W1B(0x0B, cmp, 0). cmp value mapping is
 * [INFERENCE]. */
int bcmfm_set_deemphasis(struct bcmfm_state *s, int deemph_us)
{
	uint8_t p[4];
	int rc;

	if (deemph_us != BCMFM_DEEMPH_50US && deemph_us != BCMFM_DEEMPH_75US)
		return -EINVAL;
	rc = bcmfm_rmw_audbit(s, BCMFM_AUD_DEEMPH6,
			      deemph_us == BCMFM_DEEMPH_50US);
	if (rc)
		return rc;
	mk_w1b(p, BCMFM_SUB_DEEMPH_CMP,
	       deemph_us == BCMFM_DEEMPH_50US ? 0x40 : 0x00, 0x00);
	return bcmfm_cmd(s, p, 3, NULL);
}

/* ---- tune / seek ---- */

/* Table tune row: sub_21fc = W2B(0x0A, u16, 0) + W1B(0x09, 0, 8).
 * Freq word is 10 kHz units [INFERENCE, see header]. */
int bcmfm_tune(struct bcmfm_state *s, uint16_t freq_10khz)
{
	uint8_t p[4];
	int rc;

	if (freq_10khz < BCMFM_FREQ_MIN_10KHZ ||
	    freq_10khz > BCMFM_FREQ_MAX_10KHZ)
		return -EINVAL;
	mk_w2b(p, BCMFM_SUB_FREQ, freq_10khz, 0x00);
	rc = bcmfm_cmd(s, p, sizeof(p), NULL);
	if (rc)
		return rc;
	mk_w1b(p, BCMFM_SUB_TUNE_CTL, 0x00, 0x08);
	return bcmfm_cmd(s, p, 3, NULL);
}

/* Table seek rows: scan_freq engine W1B(0x14,...) park + W2B(0x16,
 * 0x3e0/0x400, 0) + W2B(0x18, r8, 0) presets + R(0xFC, 0, 1) result.
 * Direction-to-preset assignment is [INFERENCE]; R(0xFC) result layout
 * (cb+0xce) is [INFERENCE: single status byte, freq re-read via 0x0A]. */
int bcmfm_seek(struct bcmfm_state *s, int up, uint16_t cur_10khz,
	       uint16_t *found_10khz)
{
	uint8_t p[4];
	struct bcmfm_rsp rsp;
	int rc;

	mk_w1b(p, BCMFM_SUB_SCANCTL, up ? 0x02 : 0x01, 0x00);
	rc = bcmfm_cmd(s, p, 3, NULL);
	if (rc)
		return rc;
	mk_w2b(p, BCMFM_SUB_PRESET_HI, up ? 0x0400 : 0x03E0, 0x00);
	rc = bcmfm_cmd(s, p, sizeof(p), NULL);
	if (rc)
		return rc;
	mk_w2b(p, BCMFM_SUB_PRESET_LO, cur_10khz, 0x00);
	rc = bcmfm_cmd(s, p, sizeof(p), NULL);
	if (rc)
		return rc;
	mk_rd(p, BCMFM_SUB_STEREO, 0x00);
	rc = bcmfm_cmd(s, p, 2, &rsp);
	if (rc)
		return rc;
	if (found_10khz) {
		uint16_t f = 0;

		rc = bcmfm_get_freq(s, &f);
		if (rc)
			return rc;
		*found_10khz = f;
	}
	return 0;
}

/* Table row 0x09: bta_fm_abort = W1B(0x09, 1, 8|88 by ac&4). Userspace
 * has no ac cache; flags 0x08 matches the live_update leg. */
int bcmfm_abort(struct bcmfm_state *s)
{
	uint8_t p[3];

	mk_w1b(p, BCMFM_SUB_TUNE_CTL, 0x01, 0x08);
	return bcmfm_cmd(s, p, sizeof(p), NULL);
}

/* ---- audio ---- */

int bcmfm_rmw_audbit(struct bcmfm_state *s, uint16_t bit, int on)
{
	uint8_t p[4];

	if (on)
		aud_shadow |= bit;
	else
		aud_shadow &= (uint16_t)~bit;
	mk_w2b(p, BCMFM_SUB_AUDCTL, aud_shadow, 0x88);
	return bcmfm_cmd(s, p, sizeof(p), NULL);
}

/* Table mute row: mute_aud sets/clears bit1 of cb+0xa6, W2B(0x05,0x88). */
int bcmfm_mute(struct bcmfm_state *s, int on)
{
	return bcmfm_rmw_audbit(s, BCMFM_AUD_MUTE, on);
}

/* Table RF-mute row: set_rfmute sets/clears bit0 via W2B(0x05,0x88). */
int bcmfm_set_rfmute(struct bcmfm_state *s, int on)
{
	return bcmfm_rmw_audbit(s, BCMFM_AUD_RFMUTE, on);
}

/* Table volume row: volume_ctrl = W2B(0xF8, vol, 0) + R(0xF8, 0x80, 2).
 * CLI 0..100 maps to the u16 0..255 range [INFERENCE]. */
int bcmfm_volume(struct bcmfm_state *s, int vol_0_100)
{
	uint8_t p[4];
	uint16_t v;

	if (vol_0_100 < 0 || vol_0_100 > 100)
		return -EINVAL;
	v = (uint16_t)((vol_0_100 * 255 + 50) / 100);
	mk_w2b(p, BCMFM_SUB_VOLUME, v, 0x00);
	return bcmfm_cmd(s, p, sizeof(p), NULL);
}

/* ---- RDS ---- */

/* Table RDS row: set_rds_mode = W1B(0x14, rdsbit, 0) + R(0x80, 0, 0xF0)
 * + set_mask(88,0,0x3a98). Mask events only steer kernel callbacks, so
 * userspace sends the W1B and primes with R(0x80) when turning on. */
int bcmfm_rds(struct bcmfm_state *s, int on)
{
	uint8_t p[3];
	int rc;

	mk_w1b(p, BCMFM_SUB_SCANCTL, on ? 0x01 : 0x00, 0x00);
	rc = bcmfm_cmd(s, p, sizeof(p), NULL);
	if (rc || !on)
		return rc;
	mk_rd(p, BCMFM_SUB_RDS, 0x00);
	return bcmfm_cmd(s, p, 2, NULL);
}

/* ---- queries ---- */

/* Table row 0x0A: R(0x0A, 0, 2) -> freq/status word. */
int bcmfm_get_freq(struct bcmfm_state *s, uint16_t *freq_10khz)
{
	uint8_t p[2];
	struct bcmfm_rsp rsp;
	int rc;

	if (!freq_10khz)
		return -EINVAL;
	mk_rd(p, BCMFM_SUB_FREQ, 0x00);
	rc = bcmfm_cmd(s, p, sizeof(p), &rsp);
	if (rc)
		return rc;
	if (rsp.len < 3) /* status + LE u16 */
		return -EBADMSG;
	*freq_10khz = (uint16_t)(rsp.buf[1] | ((uint16_t)rsp.buf[2] << 8));
	return 0;
}

/* Table RSSI row: get_rssi = R(0x0F, 0x80, 1). */
int bcmfm_get_rssi(struct bcmfm_state *s, int *rssi)
{
	uint8_t p[2];
	struct bcmfm_rsp rsp;
	int rc;

	if (!rssi)
		return -EINVAL;
	mk_rd(p, BCMFM_SUB_RSSI, 0x80);
	rc = bcmfm_cmd(s, p, sizeof(p), &rsp);
	if (rc)
		return rc;
	if (rsp.len < 2)
		return -EBADMSG;
	*rssi = (int8_t)rsp.buf[1];
	return 0;
}

/* Table SNR row: get_snr = R(0xDF, 0x80, 1). */
int bcmfm_get_snr(struct bcmfm_state *s, int *snr)
{
	uint8_t p[2];
	struct bcmfm_rsp rsp;
	int rc;

	if (!snr)
		return -EINVAL;
	mk_rd(p, BCMFM_SUB_SNR, 0x80);
	rc = bcmfm_cmd(s, p, sizeof(p), &rsp);
	if (rc)
		return rc;
	if (rsp.len < 2)
		return -EBADMSG;
	*snr = (int8_t)rsp.buf[1];
	return 0;
}

/* Table row 0x80: read_rds = R(0x80, 0, 0xF0) -> RDS/group data. */
int bcmfm_read_rds(struct bcmfm_state *s, uint8_t *buf, size_t cap,
		   size_t *out_len)
{
	uint8_t p[2];
	struct bcmfm_rsp rsp;
	size_t n;
	int rc;

	if (!buf || cap == 0 || !out_len)
		return -EINVAL;
	mk_rd(p, BCMFM_SUB_RDS, 0x00);
	rc = bcmfm_cmd(s, p, sizeof(p), &rsp);
	if (rc)
		return rc;
	n = rsp.len > 1 ? rsp.len - 1 : 0; /* strip status byte */
	if (n > cap)
		n = cap;
	memcpy(buf, rsp.buf + 1, n);
	*out_len = n;
	return 0;
}
