/* bcmfm CLI: thin wrapper over bcmfm.h printing human results. */
#include "bcmfm.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
	fprintf(stderr,
"Usage: %s [-d dev] [-t ms] <command> [args]\n"
"  Commands:\n"
"    info                  show freq, RSSI, SNR\n"
"    enable [region]       power on (region: eu|us|jp|world, default eu)\n"
"    disable               power off\n"
"    tune <MHz>            tune, e.g. 88.3 or 88.30\n"
"    seek up|down          seek to next station\n"
"    abort                 abort pending tune/seek (W1B 0x09)\n"
"    mute on|off           audio mute (0x05 bit1)\n"
"    rfmute on|off         RF mute (0x05 bit0)\n"
"    vol <0-100>            volume (W2B 0xF8)\n"
"    rds on|off            RDS mode (W1B 0x14 + R 0x80)\n"
"    region eu|us|jp|world set region band (W1B 0x01 + W4B 0xFB)\n"
"    step 50|100|200       scan step kHz (W2B 0xFD)\n"
"    path 0|1|2|3          audio path (W1B 0x4D + W2B 0x05)\n"
"    deemph 50|75          de-emphasis us (W2B 0x05 bit6 + W1B 0x0B)\n"
"    dump                  info + raw RDS block hex\n"
"    raw <hex...>          send arbitrary [cmd,arg,data...] bytes\n"
"  Options:\n"
"    -d dev   HCI device index (default 0)\n"
"    -t ms    per-command timeout ms (default 2000)\n",
		argv0);
}

static int parse_region(const char *s, int *out)
{
	if (!strcmp(s, "eu")) *out = BCMFM_REGION_EU;
	else if (!strcmp(s, "us")) *out = BCMFM_REGION_US;
	else if (!strcmp(s, "jp")) *out = BCMFM_REGION_JP;
	else if (!strcmp(s, "world")) *out = BCMFM_REGION_WORLD;
	else return -1;
	return 0;
}

static int parse_onoff(const char *s, int *out)
{
	if (!strcmp(s, "on") || !strcmp(s, "1")) *out = 1;
	else if (!strcmp(s, "off") || !strcmp(s, "0")) *out = 0;
	else return -1;
	return 0;
}

/* Parse one hex byte token ("0x1a", "1a", "1A"). */
static int parse_hexbyte(const char *s, uint8_t *out)
{
	char *end = NULL;
	long v;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	if (*s == '\0')
		return -1;
	errno = 0;
	v = strtol(s, &end, 16);
	if (errno || end == s || *end != '\0' || v < 0 || v > 0xFF)
		return -1;
	*out = (uint8_t)v;
	return 0;
}

static void print_hex(const uint8_t *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		printf("%s%02x", i ? " " : "", b[i]);
	printf("\n");
}

static int show_info(struct bcmfm_state *s)
{
	uint16_t f = 0;
	int rssi = 0, snr = 0, rc;

	rc = bcmfm_get_freq(s, &f);
	if (rc) { fprintf(stderr, "freq: %s\n", bcmfm_strerror(rc)); return rc; }
	rc = bcmfm_get_rssi(s, &rssi);
	if (rc) { fprintf(stderr, "rssi: %s\n", bcmfm_strerror(rc)); return rc; }
	rc = bcmfm_get_snr(s, &snr);
	if (rc) { fprintf(stderr, "snr: %s\n", bcmfm_strerror(rc)); return rc; }
	printf("freq %.2f MHz  rssi %d  snr %d\n", f / 100.0, rssi, snr);
	return 0;
}

int main(int argc, char **argv)
{
	struct bcmfm_state st = { .fd = -1, .dev_id = 0, .timeout_ms = 2000 };
	int dev = 0, timeout = 2000, rc = 0, i;
	const char *cmd;

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		}
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dev = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			timeout = atoi(argv[++i]);
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (i >= argc) {
		usage(argv[0]);
		return 2;
	}
	cmd = argv[i++];

	if (!strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
		usage(argv[0]);
		return 0;
	}

	/* raw help without opening HCI (still needs device for send). */
	rc = bcmfm_open(&st, dev, timeout);
	if (rc) {
		fprintf(stderr, "open hci%d: %s\n", dev, bcmfm_strerror(rc));
		return 1;
	}

	if (!strcmp(cmd, "info")) {
		rc = show_info(&st);
	} else if (!strcmp(cmd, "enable")) {
		int region = BCMFM_REGION_EU;

		if (i < argc && parse_region(argv[i], &region)) {
			fprintf(stderr, "bad region (eu|us|jp|world)\n");
			rc = 2;
		} else {
			rc = bcmfm_enable(&st, region);
			if (!rc) printf("enabled region=%d\n", region);
		}
		if (!rc && i < argc) i++;
	} else if (!strcmp(cmd, "disable")) {
		rc = bcmfm_disable(&st);
		if (!rc) printf("disabled\n");
	} else if (!strcmp(cmd, "tune")) {
		double mhz;
		uint16_t f10;

		if (i >= argc) { fprintf(stderr, "tune needs MHz\n"); rc = 2; }
		else {
			mhz = atof(argv[i++]);
			f10 = (uint16_t)(mhz * 100.0 + 0.5);
			rc = bcmfm_tune(&st, f10);
			if (!rc) printf("tuned %.2f MHz\n", f10 / 100.0);
		}
	} else if (!strcmp(cmd, "seek")) {
		int up;
		uint16_t cur = 0, found = 0;

		if (i >= argc || (strcmp(argv[i], "up") && strcmp(argv[i], "down"))) {
			fprintf(stderr, "seek needs up|down\n"); rc = 2;
		} else {
			up = !strcmp(argv[i++], "up");
			if (bcmfm_get_freq(&st, &cur))
				cur = 8750; /* fall back if unreadable */
			rc = bcmfm_seek(&st, up, cur, &found);
			if (!rc) printf("seek %s -> %.2f MHz\n",
					up ? "up" : "down", found / 100.0);
		}
	} else if (!strcmp(cmd, "abort")) {
		rc = bcmfm_abort(&st);
		if (!rc) printf("aborted\n");
	} else if (!strcmp(cmd, "mute") || !strcmp(cmd, "rfmute")) {
		int on;

		if (i >= argc || parse_onoff(argv[i++], &on)) {
			fprintf(stderr, "need on|off\n"); rc = 2;
		} else if (!strcmp(cmd, "mute")) {
			rc = bcmfm_mute(&st, on);
			if (!rc) printf("mute %s\n", on ? "on" : "off");
		} else {
			rc = bcmfm_set_rfmute(&st, on);
			if (!rc) printf("rfmute %s\n", on ? "on" : "off");
		}
	} else if (!strcmp(cmd, "vol")) {
		if (i >= argc) { fprintf(stderr, "vol needs 0-100\n"); rc = 2; }
		else {
			int v = atoi(argv[i++]);

			rc = bcmfm_volume(&st, v);
			if (!rc) printf("volume %d\n", v);
		}
	} else if (!strcmp(cmd, "rds")) {
		int on;

		if (i >= argc || parse_onoff(argv[i++], &on)) {
			fprintf(stderr, "need on|off\n"); rc = 2;
		} else {
			rc = bcmfm_rds(&st, on);
			if (!rc) printf("rds %s\n", on ? "on" : "off");
		}
	} else if (!strcmp(cmd, "region")) {
		int region;

		if (i >= argc || parse_region(argv[i++], &region)) {
			fprintf(stderr, "need eu|us|jp|world\n"); rc = 2;
		} else {
			rc = bcmfm_set_region(&st, region);
			if (!rc) printf("region %d\n", region);
		}
	} else if (!strcmp(cmd, "step")) {
		if (i >= argc) { fprintf(stderr, "need 50|100|200\n"); rc = 2; }
		else {
			int stp = atoi(argv[i++]);

			rc = bcmfm_set_step(&st, stp);
			if (!rc) printf("step %d kHz\n", stp);
		}
	} else if (!strcmp(cmd, "path")) {
		if (i >= argc) { fprintf(stderr, "need 0..3\n"); rc = 2; }
		else {
			int p = atoi(argv[i++]);

			rc = bcmfm_set_audio_path(&st, p);
			if (!rc) printf("audio path %d\n", p);
		}
	} else if (!strcmp(cmd, "deemph")) {
		if (i >= argc) { fprintf(stderr, "need 50|75\n"); rc = 2; }
		else {
			int d = atoi(argv[i++]);

			rc = bcmfm_set_deemphasis(&st, d);
			if (!rc) printf("deemphasis %d us\n", d);
		}
	} else if (!strcmp(cmd, "dump")) {
		uint8_t rds[256];
		size_t n = 0;

		rc = show_info(&st);
		if (!rc)
			rc = bcmfm_read_rds(&st, rds, sizeof(rds), &n);
		if (!rc) {
			printf("rds %zu bytes: ", n);
			print_hex(rds, n);
		}
	} else if (!strcmp(cmd, "raw")) {
		uint8_t p[256];
		size_t n = 0;
		struct bcmfm_rsp rsp;
		int j;

		for (j = i; j < argc; j++) {
			if (n >= sizeof(p)) { fprintf(stderr, "raw too long\n"); rc = 2; break; }
			if (parse_hexbyte(argv[j], &p[n])) {
				fprintf(stderr, "bad hex byte: %s\n", argv[j]);
				rc = 2; break;
			}
			n++;
		}
		if (!rc && n < 2) { fprintf(stderr, "raw needs [cmd,arg,data...]\n"); rc = 2; }
		if (!rc) {
			rc = bcmfm_raw(&st, p, n, &rsp);
			if (!rc) {
				printf("status 0x%02x, %zu bytes: ", rsp.status, rsp.len);
				print_hex(rsp.buf, rsp.len);
			}
		}
	} else {
		fprintf(stderr, "unknown command: %s\n", cmd);
		usage(argv[0]);
		rc = 2;
	}

	if (rc > 0) /* positive HCI status byte */
		fprintf(stderr, "controller status 0x%02x\n", rc & 0xFF);
	else if (rc < 0 && rc != 2)
		fprintf(stderr, "%s %s\n", cmd, bcmfm_strerror(rc));
	bcmfm_close(&st);
	if (rc == 2)
		return 2;
	return rc ? 1 : 0;
}
