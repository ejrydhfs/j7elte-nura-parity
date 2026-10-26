// SPDX-License-Identifier: MIT
/*
 * sipc5-probe - SIPC5 frame/transport probe for the j7elte SS310AP modem.
 *
 *   sipc5-probe selftest
 *       Run the codec/framing tests (no device needed).  This pins the exact
 *       byte layouts used by drivers/misc/modem_v1 and by the stock
 *       libsec-ril.so builders.
 *
 *   sipc5-probe decode <hex>
 *       Decode a complete SIPC5 link frame (as it appears on the wire, e.g.
 *       captured from the BOOT channel or a ramdump) and, when its channel
 *       is FMT/RFS, the enclosed 7-byte FMT header.
 *
 *   sipc5-probe monitor <fmt-dev> [rfs-dev] [timeout-ms] [count]
 *       Open the channels (both, so the kernel can answer CP_START with
 *       INIT_END), read payloads as the kernel delivers them and reassemble
 *       multi-fragment messages.  Read-only.
 *
 *   sipc5-probe send <dev> <hex>
 *       Write one payload with one write(2), i.e. exactly one SIPC5 frame.
 *
 *   sipc5-probe status <dev>
 *       ioctl(IOCTL_MODEM_STATUS).
 */

#include "sipc5.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
	if (cond) { \
		printf("ok   %s\n", #cond); \
	} else { \
		printf("FAIL %s (line %d)\n", #cond, __LINE__); \
		failures++; \
	} \
} while (0)

static void dump_hex(const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t i;

	for (i = 0; i < len; i++)
		printf("%02x%s", p[i], (i + 1 == len) ? "" : " ");
}

static size_t parse_hex(const char *s, uint8_t *out, size_t cap)
{
	size_t n = 0;
	int hi = -1;

	for (; *s; s++) {
		int v;

		if (*s == ' ' || *s == ':' || *s == ',' || *s == '-' ||
		    (*s == '0' && (s[1] == 'x' || s[1] == 'X'))) {
			if (*s == '0' && (s[1] == 'x' || s[1] == 'X'))
				s++;
			continue;
		}
		if (*s >= '0' && *s <= '9')
			v = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			v = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			v = *s - 'A' + 10;
		else
			return (size_t)-1;

		if (hi < 0) {
			hi = v;
		} else {
			if (n == cap)
				return (size_t)-1;
			out[n++] = (uint8_t)((hi << 4) | v);
			hi = -1;
		}
	}
	if (hi >= 0)
		return (size_t)-1;
	return n;
}

static const char *cmd_type_name(uint8_t t)
{
	switch (t) {
	case SIPC5_CMD_EXEC: return "EXEC";
	case SIPC5_CMD_GET: return "GET";
	case SIPC5_CMD_SET: return "SET";
	case SIPC5_CMD_CFRM: return "CFRM";
	case SIPC5_CMD_EVENT: return "EVENT";
	default: return "?";
	}
}

static const char *ch_name(uint8_t ch)
{
	static char buf[16];

	if (ch >= SIPC5_CH_ID_FMT_0 && ch <= SIPC5_CH_ID_FMT_9) {
		snprintf(buf, sizeof(buf), "FMT_%u", ch - SIPC5_CH_ID_FMT_0);
		return buf;
	}
	if (ch >= SIPC5_CH_ID_RFS_0 && ch <= SIPC5_CH_ID_RFS_9) {
		snprintf(buf, sizeof(buf), "RFS_%u", ch - SIPC5_CH_ID_RFS_0);
		return buf;
	}
	if (ch >= SIPC5_CH_ID_BOOT_0 && ch < SIPC5_CH_ID_BOOT_0 + 10) {
		snprintf(buf, sizeof(buf), "BOOT_%u", ch - SIPC5_CH_ID_BOOT_0);
		return buf;
	}
	if (ch >= SIPC5_CH_ID_DUMP_0 && ch < SIPC5_CH_ID_DUMP_0 + 10) {
		snprintf(buf, sizeof(buf), "DUMP_%u", ch - SIPC5_CH_ID_DUMP_0);
		return buf;
	}
	return "ch";
}

static void print_link(const struct sipc5_link *l)
{
	printf("  cfg  0x%02x%s%s%s (%u-byte header)\n", l->cfg,
	       (l->cfg & SIPC5_PADDING_EXIST) ? " PAD" : "",
	       (l->cfg & SIPC5_EXT_FIELD_EXIST) ? " EXT" : "",
	       sipc5_hdr_len(l->cfg) == SIPC5_HEADER_SIZE_WITH_CTL_FLD ? " CTRL" : "",
	       sipc5_hdr_len(l->cfg));
	printf("  ch   %u (%s)\n", l->ch, ch_name(l->ch));
	printf("  len  %u (header + payload)\n", l->len);
	if (sipc5_hdr_len(l->cfg) == SIPC5_HEADER_SIZE_WITH_CTL_FLD)
		printf("  ctrl 0x%02x (more=%u id=%u)\n", l->ctrl,
		       l->ctrl >> 7, l->ctrl & SIPC5_CTRL_ID_MASK);
	printf("  pad  %u\n", sipc5_pad_len(l->len, l->cfg & SIPC5_PADDING_EXIST));
}

static int cmd_selftest(void)
{
	static const uint8_t poweroff[] = { 0x07, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01 };
	static const uint8_t sapstatus[] = { 0x07, 0x00, 0x00, 0x00, 0x12, 0x02, 0x02 };
	static const uint8_t bbversion[] = { 0x08, 0x00, 0x00, 0x00, 0x0a, 0x01, 0x02, 0xff };
	static const uint8_t radiopower[] = { 0x08, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x03 };
	uint8_t buf[128], body[64];
	struct sipc5_link l;
	struct sipc5_msg_asm a;
	unsigned n;
	size_t len;
	int r;

	printf("== link-layer header lengths\n");
	CHECK(sipc5_hdr_len(0xF8) == 4);
	CHECK(sipc5_hdr_len(0xFB) == 5);	/* MULTI_FRAME_CFG */
	CHECK(sipc5_hdr_len(0xFA) == 6);	/* EXT_LENGTH_CFG */

	printf("== FMT frame on umts_ipc0 (kernel builds this)\n");
	memset(buf, 0xCC, sizeof(buf));
	len = sipc5_fmt_build(body, sizeof(body), 1, 2, SIPC5_CMD_EXEC, 0, 0, NULL, 0);
	CHECK(len == 7);
	CHECK(memcmp(body, poweroff, 7) == 0);
	n = sipc5_link_build(buf, sizeof(buf), 0xF8, SIPC5_CH_ID_FMT_0, 7, 0);
	CHECK(n == 4);
	CHECK(buf[0] == 0xF8 && buf[1] == SIPC5_CH_ID_FMT_0);
	CHECK((buf[2] | (buf[3] << 8)) == 11);
	CHECK(sipc5_link_parse(buf, 11, &l) == 0);
	CHECK(l.cfg == 0xF8 && l.ch == SIPC5_CH_ID_FMT_0 && l.len == 11);
	CHECK(sipc5_pad_len(l.len, false) == 0);

	printf("== aligned BOOT frame (cfg PAD bit -> 8-byte padding)\n");
	memset(buf, 0, sizeof(buf));
	n = sipc5_link_build(buf, sizeof(buf), 0xFC, SIPC5_CH_ID_BOOT_0, 16, 0);
	CHECK(n == 4);
	CHECK(sipc5_pad_len(20, true) == 4);
	CHECK(sipc5_pad_len(16, true) == 0);
	CHECK(sipc5_pad_len(20, false) == 0);
	CHECK(sipc5_link_parse(buf, 24, &l) == 0);
	CHECK(l.len == 20);

	printf("== post-DL_START FMT frame: aligned link device (cfg 0xFC)\n");
	memset(buf, 0, sizeof(buf));
	n = sipc5_link_build(buf, sizeof(buf), 0xFC, SIPC5_CH_ID_FMT_0, 7, 0);
	CHECK(n == 4);
	CHECK(sipc5_frame_len(buf, 4) == 11);
	CHECK(sipc5_pad_len(11, true) == 5);
	CHECK(sipc5_total_len(buf, 4) == 16);
	CHECK(sipc5_link_parse(buf, 16, &l) == 0);
	CHECK(l.len == 11 && sipc5_hdr_len(l.cfg) == 4);

	printf("== multi-frame control field\n");
	memset(buf, 0, sizeof(buf));
	n = sipc5_link_build(buf, sizeof(buf), 0xFB, SIPC5_CH_ID_FMT_0, 10, 0x81);
	CHECK(n == 5);
	CHECK((buf[2] | (buf[3] << 8)) == 15);
	CHECK(sipc5_link_parse(buf, 15, &l) == 0);
	CHECK(l.ctrl == 0x81);
	CHECK((l.ctrl >> 7) == 1 && (l.ctrl & SIPC5_CTRL_ID_MASK) == 1);

	printf("== extended-length field\n");
	memset(buf, 0, sizeof(buf));
	n = sipc5_link_build(buf, sizeof(buf), 0xFA, SIPC5_CH_ID_FMT_0, 3, 0);
	CHECK(n == 6);
	CHECK(sipc5_frame_len(buf, 6) == 9);
	CHECK(sipc5_link_parse(buf, 9, &l) == 0);
	CHECK(l.len == 9);

	printf("== malformed header\n");
	buf[0] = 0x00;
	CHECK(sipc5_link_parse(buf, 8, &l) == -EINVAL);

	printf("== stock FMT message layouts (byte-exact)\n");
	len = sipc5_fmt_build(buf, sizeof(buf), 1, 7, SIPC5_CMD_EXEC, 0, 0,
			      (const uint8_t[]){ 3 }, 1);
	CHECK(len == 8 && memcmp(buf, radiopower, 8) == 0);
	len = sipc5_fmt_build(buf, sizeof(buf), 0x12, 2, SIPC5_CMD_GET, 0, 0, NULL, 0);
	CHECK(len == 7 && memcmp(buf, sapstatus, 7) == 0);
	len = sipc5_fmt_build(buf, sizeof(buf), 0x0a, 1, SIPC5_CMD_GET, 0, 0,
			      (const uint8_t[]){ 0xff }, 1);
	CHECK(len == 8 && memcmp(buf, bbversion, 8) == 0);

	printf("== FMT parse round-trip\n");
	{
		struct sipc5_fmt_hdr h;
		const uint8_t *pbody;
		size_t blen;

		len = sipc5_fmt_build(buf, sizeof(buf), 0x12, 2, SIPC5_CMD_GET,
				      9, 4, (const uint8_t[]){ 0xaa, 0xbb }, 2);
		CHECK(len == 9);
		CHECK(sipc5_fmt_parse(buf, len, &h, &pbody, &blen) == len);
		CHECK(h.len == 9 && h.msg_seq == 9 && h.ack_seq == 4);
		CHECK(h.main_cmd == 0x12 && h.sub_cmd == 2 &&
		      h.cmd_type == SIPC5_CMD_GET);
		CHECK(blen == 2 && pbody[0] == 0xaa && pbody[1] == 0xbb);
	}

	printf("== fragment reassembly (kernel splices multi-frame skbs)\n");
	sipc5_asm_init(&a, buf, sizeof(buf));
	r = sipc5_asm_feed(&a, poweroff, 3, NULL);
	CHECK(r == 0);
	r = sipc5_asm_feed(&a, poweroff + 3, 4, NULL);
	CHECK(r == 1 && a.want == 0 && a.got == 0);
	sipc5_asm_init(&a, buf, sizeof(buf));
	r = sipc5_asm_feed(&a, bbversion, 8, NULL);
	CHECK(r == 1);
	sipc5_asm_init(&a, buf, sizeof(buf));
	r = sipc5_asm_feed(&a, "\x03\x00\x00\x00\x00\x00\x00", 7, NULL);
	CHECK(r == -EINVAL);	/* declared len < header */

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}

static int cmd_decode(const char *hex)
{
	uint8_t frame[512];
	struct sipc5_link l;
	size_t n = parse_hex(hex, frame, sizeof(frame));
	unsigned hdr_len, pay;

	if (n == (size_t)-1 || n < SIPC5_MIN_HEADER_SIZE) {
		fprintf(stderr, "decode: bad hex\n");
		return 2;
	}
	if (sipc5_link_parse(frame, n, &l) < 0) {
		fprintf(stderr, "decode: not a valid SIPC5 link frame\n");
		return 1;
	}
	print_link(&l);
	hdr_len = sipc5_hdr_len(l.cfg);
	pay = l.len - hdr_len;
	printf("  payload (%u bytes): ", pay);
	dump_hex(frame + hdr_len, pay);
	printf("\n");

	if (pay >= SIPC5_FMT_HDR_LEN) {
		struct sipc5_fmt_hdr fh;
		const uint8_t *body;
		size_t blen;

		if (sipc5_fmt_parse(frame + hdr_len, pay, &fh, &body, &blen)) {
			printf("  FMT  len=%u seq=%u ack=%u main=%u sub=%u "
			       "type=%u (%s) body=%zu bytes\n",
			       fh.len, fh.msg_seq, fh.ack_seq, fh.main_cmd,
			       fh.sub_cmd, fh.cmd_type,
			       cmd_type_name(fh.cmd_type), blen);
		} else {
			printf("  FMT  header does not cover the payload\n");
		}
	}
	return 0;
}

static int cmd_monitor(int argc, char **argv)
{
	uint8_t buf[65535];
	static uint8_t asmbuf[65535];
	struct sipc5_msg_asm a;
	int timeout_ms = 1000, want_count = -1;
	const char *fmt = argv[1];
	const char *rfs = argc > 2 ? argv[2] : NULL;
	int ffd, rfd, count = 0;
	size_t msglen = 0;

	if (argc > 3)
		timeout_ms = atoi(argv[3]);
	if (argc > 4)
		want_count = atoi(argv[4]);

	if (rfs && *rfs) {
		int r = sipc5_open_channels(fmt, rfs, &ffd, &rfd);

		if (r) {
			fprintf(stderr, "open %s + %s: %s\n", fmt, rfs,
				strerror(-r));
			return 1;
		}
		printf("opened %s (fd %d) and %s (fd %d)\n", fmt, ffd, rfs, rfd);
	} else {
		ffd = sipc5_open(fmt);
		rfd = -1;
		if (ffd < 0) {
			fprintf(stderr, "open %s: %s\n", fmt, strerror(errno));
			return 1;
		}
		printf("opened %s (fd %d) - RFS not opened, kernel will not "
		       "send INIT_END\n", fmt, ffd);
	}

	sipc5_asm_init(&a, asmbuf, sizeof(asmbuf));

	for (;;) {
		ssize_t n = sipc5_read_msg(ffd, buf, sizeof(buf), timeout_ms);

		if (n == 0) {
			if (want_count >= 0)
				break;
			continue;
		}
		if (n < 0) {
			fprintf(stderr, "read: %s\n", strerror((int)-n));
			break;
		}

		printf("[%d] read %zd bytes: ", count, n);
		dump_hex(buf, (size_t)n < 32 ? (size_t)n : 32);
		printf("%s\n", (size_t)n > 32 ? " ..." : "");

		msglen = 0;
		switch (sipc5_asm_feed(&a, buf, (size_t)n, &msglen)) {
		case 1: {
			struct sipc5_fmt_hdr fh;
			const uint8_t *body;
			size_t blen = 0;

			if (sipc5_fmt_parse(asmbuf, msglen, &fh, &body, &blen))
				printf("    msg len=%u seq=%u ack=%u "
				       "main=%u sub=%u type=%u (%s) "
				       "body=%zu bytes\n",
				       fh.len, fh.msg_seq, fh.ack_seq,
				       fh.main_cmd, fh.sub_cmd,
				       fh.cmd_type,
				       cmd_type_name(fh.cmd_type), blen);
			else
				printf("    message len=%zu (bad FMT header)\n",
				       msglen);
			count++;
			if (want_count >= 0 && count >= want_count)
				goto done;
			break;
		}
		case 0:
			break;
		case -EMSGSIZE:
			fprintf(stderr, "    fragment overruns declared length\n");
			goto done;
		case -EINVAL:
			fprintf(stderr, "    malformed message header\n");
			goto done;
		}
	}
done:
	sipc5_close(ffd);
	if (rfd >= 0)
		sipc5_close(rfd);
	return 0;
}

static int cmd_send(const char *dev, const char *hex)
{
	uint8_t buf[65535];
	size_t n = parse_hex(hex, buf, sizeof(buf));
	int fd, r;

	if (n == (size_t)-1 || n == 0) {
		fprintf(stderr, "send: bad hex\n");
		return 2;
	}
	fd = sipc5_open(dev);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	r = sipc5_write_msg(fd, buf, n);
	if (r)
		fprintf(stderr, "write %zu bytes: %s\n", n, strerror(-r));
	else
		printf("wrote %zu bytes as one SIPC5 frame on %s\n", n, dev);
	sipc5_close(fd);
	return r ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s selftest\n"
			"       %s decode <hex-frame>\n"
			"       %s monitor <fmt-dev> [rfs-dev|-] [timeout-ms] [count]\n"
			"       %s send <dev> <hex-payload>\n"
			"       %s status <dev>\n",
			argv[0], argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "selftest"))
		return cmd_selftest();
	if (!strcmp(argv[1], "decode") && argc == 3)
		return cmd_decode(argv[2]);
	if (!strcmp(argv[1], "monitor") && argc >= 3)
		return cmd_monitor(argc - 1, argv + 1);
	if (!strcmp(argv[1], "send") && argc == 4)
		return cmd_send(argv[2], argv[3]);
	if (!strcmp(argv[1], "status") && argc == 3) {
		int fd = sipc5_open(argv[2]);
		int r;

		if (fd < 0) {
			fprintf(stderr, "open %s: %s\n", argv[2], strerror(errno));
			return 1;
		}
		r = sipc5_modem_status(fd);
		printf("IOCTL_MODEM_STATUS: %s\n", r ? strerror(-r) : "ok");
		sipc5_close(fd);
		return r ? 1 : 0;
	}

	fprintf(stderr, "bad arguments\n");
	return 2;
}
