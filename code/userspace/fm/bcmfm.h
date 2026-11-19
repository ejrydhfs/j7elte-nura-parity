#ifndef BCMFM_H
#define BCMFM_H

/*
 * bcmfm: BCM43438 FM radio over raw HCI vendor opcode 0xFC15.
 *
 * Byte map: see fc15-table.md. Every subcommand below cites the table row
 * it implements. Transport (framing, Command Complete wait) is hci_raw.h.
 *
 * Payload rule (fc15-table.md sec 0, helper encoding):
 *   W1B(cmd,d,flags): [cmd, flags|0x10, d]
 *   W2B(cmd,w,flags): [cmd, flags|0x20, lo, hi]
 *   W4B(lo,hi):       [0xFB, flags|0x40, lo0,hi0, lo1,hi1]
 *   R(cmd,r1):         [cmd, (r1&8 ? 0x11 : 0x19) | (r1&0x80 ? 0x80 : 0)]
 *     e.g. R(x,0,x) -> arg 0x19; R(x,0x80,x) -> arg 0x99.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- subcommand ids (fc15-table.md sec 1, first column) ---- */
#define BCMFM_SUB_POWER_INIT   0x00 /* W1B region class (enable path) */
#define BCMFM_SUB_REGION       0x01 /* W1B region class (sub_2264) */
#define BCMFM_SUB_CHIPFLAGS    0x02 /* W1B 2/3 init (+0x88 variant) */
#define BCMFM_SUB_AUDCTL       0x05 /* W2B audio control word */
#define BCMFM_SUB_SNRTHR       0x07 /* W1B search SNR-ish threshold */
#define BCMFM_SUB_RSSITHR      0x08 /* W1B RSSI threshold (gated) */
#define BCMFM_SUB_TUNE_CTL     0x09 /* W1B tune-go / abort */
#define BCMFM_SUB_FREQ         0x0A /* W2B tune target / R freq word */
#define BCMFM_SUB_DEEMPH_CMP   0x0B /* W1B de-emphasis compare value */
#define BCMFM_SUB_RSSI         0x0F /* R RSSI */
#define BCMFM_SUB_MASK         0x10 /* W2B event mask (set_mask) */
#define BCMFM_SUB_MASKRD       0x12 /* R mask-cache refresh */
#define BCMFM_SUB_SCANCTL      0x14 /* W1B scan park / RDS cfg */
#define BCMFM_SUB_PRESET_HI    0x16 /* W2B seek preset word */
#define BCMFM_SUB_PRESET_LO    0x18 /* W2B seek preset word cont. */
#define BCMFM_SUB_AUDIOPATH    0x4D /* W1B audio-path/power bit; R init */
#define BCMFM_SUB_RDS          0x80 /* R RDS/group data */
#define BCMFM_SUB_SCHCRIT      0xDE /* W1B search-criteria byte cf */
#define BCMFM_SUB_SNR          0xDF /* R SNR / audio quality */
#define BCMFM_SUB_VOLUME       0xF8 /* W2B volume + R verify */
#define BCMFM_SUB_BAND         0xFB /* W4B search-boundary pair */
#define BCMFM_SUB_STEREO       0xFC /* W1B stereo blend / R seek result */
#define BCMFM_SUB_STEP         0xFD /* W2B scan-step table value */
#define BCMFM_SUB_AFJUMP       0xFE /* W1B AF-jump freq word low */
#define BCMFM_SUB_CHIPVER      0xFF /* R chip-ver / RSSI cal */

/* ---- arg length classes (fc15-table.md sec 0) ---- */
#define BCMFM_ARG_W1B 0x10
#define BCMFM_ARG_W2B 0x20
#define BCMFM_ARG_W4B 0x40
#define BCMFM_ARG_R_1B 0x11 /* read, r1 bit3 clear */
#define BCMFM_ARG_R_2B 0x19 /* read, r1 bit3 set   */
#define BCMFM_ARG_RD   0x80 /* ORed in when r1 bit7 set */

/* ---- audio control word bits (fc15-table.md sec 1 tail + re-notes.txt
 *      item 4: bit positions observed, names inferred) ---- */
#define BCMFM_AUD_RFMUTE  0x0001 /* bit0 RF-mute */
#define BCMFM_AUD_MUTE    0x0002 /* bit1 mute */
#define BCMFM_AUD_DEEMPH  0x0010 /* bit4 de-emphasis select delta [INFERENCE] */
#define BCMFM_AUD_I2S     0x0020 /* bit5 I2S/digital path [INFERENCE] */
#define BCMFM_AUD_BLEND   0x0020 /* alias: blend bit per mute_aud context */
#define BCMFM_AUD_DEEMPH6 0x0040 /* bit6 de-emphasis toggle (set_deemphasis) */

/* ---- frequency / region / step constants (fm-bringup-seq.md) ---- */
/* App range 8750..10800, framework accepts 7600..10800 (10 kHz units);
 * defaults 8750 (or 8830 when FEATURE_DEFAULTCHANNEL == "88.3"). */
#define BCMFM_FREQ_MIN_10KHZ 7600
#define BCMFM_FREQ_MAX_10KHZ 10800
#define BCMFM_FREQ_DEF_10KHZ 8750
#define BCMFM_FREQ_DEF_ALT_10KHZ 8830
/* Chip tune word is 10 kHz units: 8750 fits u16, 87500 (kHz) does not,
 * so W2B(0x0A) carries 10 kHz units [INFERENCE from sub_21fc + JNI
 * tuneNative freq*10 chain]. */
#define BCMFM_MHZ_TO_10KHZ(mhz) ((uint16_t)((mhz) * 100.0 + 0.5))
#define BCMFM_10KHZ_TO_MHZ(f) ((f) / 100.0)

/* Region ids: service default mWorldRegion=0, mStepSize=0, mAudioPath=1,
 * mDeemphasisTime=64 (fm-bringup-seq.md sec 4.4). Band edges follow the
 * framework BAND_* names (BAND_87500_108000 / BAND_76000_90000 /
 * BAND_76000_108000); numeric region ordinals are framework-compiled
 * ([INFERENCE], names only observed). */
enum bcmfm_region {
	BCMFM_REGION_EU = 0,   /* 87.5-108 MHz, 100 kHz, 50 us */
	BCMFM_REGION_US = 1,   /* 87.5-108 MHz, 200 kHz, 75 us */
	BCMFM_REGION_JP = 2,   /* 76.0-90.0 MHz */
	BCMFM_REGION_WORLD = 3 /* 76.0-108 MHz */
};
#define BCMFM_STEP_50KHZ  50
#define BCMFM_STEP_100KHZ 100
#define BCMFM_STEP_200KHZ 200

/* De-emphasis times (framework DEEMPHASIS_* names observed, ordinals
 * inferred; service default mDeemphasisTime=64). */
#define BCMFM_DEEMPH_50US 50
#define BCMFM_DEEMPH_75US 75

struct bcmfm_state {
	int fd;          /* raw HCI socket, or -1 if unopened */
	int dev_id;      /* HCI device index */
	int timeout_ms;  /* per-command Command Complete wait */
};

/* Last-response introspection for CLI printing. */
struct bcmfm_rsp {
	uint8_t status;      /* return-param byte 0 */
	uint8_t buf[256];    /* full return params incl. status */
	size_t len;          /* full return-param length */
};

/* Lifecycle */
int bcmfm_open(struct bcmfm_state *s, int dev_id, int timeout_ms);
void bcmfm_close(struct bcmfm_state *s);

/* Low-level: send one FC15 payload, wait Command Complete, check status.
 * rsp may be NULL. Returns 0 on status==0, negative -errno, or +status
 * (positive HCI status byte) when the controller reports failure. */
int bcmfm_cmd(struct bcmfm_state *s, const uint8_t *payload, size_t plen,
	      struct bcmfm_rsp *rsp);

/* Raw escape hatch: caller-supplied [cmd, arg, data...] bytes. */
int bcmfm_raw(struct bcmfm_state *s, const uint8_t *payload, size_t plen,
	      struct bcmfm_rsp *rsp);

/* Power / setup */
int bcmfm_enable(struct bcmfm_state *s, int region);
int bcmfm_disable(struct bcmfm_state *s);
int bcmfm_set_region(struct bcmfm_state *s, int region);
int bcmfm_set_step(struct bcmfm_state *s, int step_khz);
int bcmfm_set_audio_path(struct bcmfm_state *s, int path);
int bcmfm_set_deemphasis(struct bcmfm_state *s, int deemph_us);

/* Tune / seek */
int bcmfm_tune(struct bcmfm_state *s, uint16_t freq_10khz);
int bcmfm_seek(struct bcmfm_state *s, int up, uint16_t cur_10khz,
	       uint16_t *found_10khz);
int bcmfm_abort(struct bcmfm_state *s);

/* Audio */
int bcmfm_mute(struct bcmfm_state *s, int on);
int bcmfm_set_rfmute(struct bcmfm_state *s, int on);
int bcmfm_volume(struct bcmfm_state *s, int vol_0_100);
int bcmfm_rmw_audbit(struct bcmfm_state *s, uint16_t bit, int on);

/* RDS */
int bcmfm_rds(struct bcmfm_state *s, int on);

/* Queries */
int bcmfm_get_freq(struct bcmfm_state *s, uint16_t *freq_10khz);
int bcmfm_get_rssi(struct bcmfm_state *s, int *rssi);
int bcmfm_get_snr(struct bcmfm_state *s, int *snr);
int bcmfm_read_rds(struct bcmfm_state *s, uint8_t *buf, size_t cap,
		   size_t *out_len);

/* Helpers exposed for CLI/tests */
uint16_t bcmfm_region_band(int region, uint16_t *lo, uint16_t *hi);
uint8_t bcmfm_read_arg(uint8_t r1);
const char *bcmfm_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* BCMFM_H */
