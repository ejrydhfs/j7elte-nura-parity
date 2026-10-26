/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dbus-wire.c - minimal D-Bus binary protocol implementation
 *
 * Only the subset needed by net.hadess.SensorProxy is implemented.  The
 * code is deliberately dependency-free (no libdbus, no GLib) so that the
 * daemon can be built for musl and statically linked.
 */
#include "dbus-wire.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "dbus-wire.c only implements the little-endian D-Bus wire format"
#endif

#define DW_MAX_MESSAGE (4u * 1024u * 1024u)
#define DW_HEADER_SIZE 16u

struct dw_writer {
	uint8_t *buf;
	size_t len;
	size_t cap;
};

struct dw_conn {
	int fd;
	uint32_t serial;
	uint8_t *in;
	size_t inlen;
	size_t incap;
	char unique[128];
};

/* ------------------------------------------------------------------ */
/* little-endian helpers                                              */
/* ------------------------------------------------------------------ */

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static size_t align_up(size_t v, size_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (!err || errlen == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ */
/* writer                                                             */
/* ------------------------------------------------------------------ */

static void w_reserve(struct dw_writer *w, size_t extra)
{
	size_t need = w->len + extra;
	size_t cap;
	uint8_t *nb;

	if (need <= w->cap)
		return;
	cap = w->cap ? w->cap : 512;
	while (cap < need)
		cap *= 2;
	nb = realloc(w->buf, cap);
	if (!nb) {
		fprintf(stderr, "j7elte-sensors: out of memory\n");
		exit(1);
	}
	w->buf = nb;
	w->cap = cap;
}

static void w_bytes(struct dw_writer *w, const void *p, size_t n)
{
	w_reserve(w, n);
	memcpy(w->buf + w->len, p, n);
	w->len += n;
}

static void w_u8(struct dw_writer *w, uint8_t v)
{
	w_bytes(w, &v, 1);
}

static void w_pad(struct dw_writer *w, size_t align)
{
	size_t target = align_up(w->len, align);

	if (target > w->len) {
		size_t n = target - w->len;
		w_reserve(w, n);
		memset(w->buf + w->len, 0, n);
		w->len += n;
	}
}

void dw_w_align(struct dw_writer *w, size_t align)
{
	w_pad(w, align);
}

void dw_w_uint32(struct dw_writer *w, uint32_t v)
{
	uint8_t b[4];

	w_pad(w, 4);
	put_le32(b, v);
	w_bytes(w, b, 4);
}

void dw_w_bool(struct dw_writer *w, int v)
{
	dw_w_uint32(w, v ? 1 : 0);
}

void dw_w_double(struct dw_writer *w, double v)
{
	union {
		double d;
		uint64_t u;
	} u;
	uint8_t b[8];

	u.d = v;
	w_pad(w, 8);
	put_le32(b, (uint32_t)(u.u & 0xffffffffu));
	put_le32(b + 4, (uint32_t)(u.u >> 32));
	w_bytes(w, b, 8);
}

void dw_w_string(struct dw_writer *w, const char *s)
{
	size_t n = strlen(s);

	dw_w_uint32(w, (uint32_t)n);
	w_bytes(w, s, n + 1);
}

void dw_w_object_path(struct dw_writer *w, const char *s)
{
	dw_w_string(w, s);
}

void dw_w_signature(struct dw_writer *w, const char *s)
{
	size_t n = strlen(s);

	if (n > 255) {
		fprintf(stderr, "j7elte-sensors: signature too long\n");
		exit(1);
	}
	w_u8(w, (uint8_t)n);
	w_bytes(w, s, n + 1);
}

void dw_w_variant_string(struct dw_writer *w, const char *s)
{
	dw_w_signature(w, "s");
	w_pad(w, 4);
	dw_w_string(w, s);
}

void dw_w_variant_bool(struct dw_writer *w, int v)
{
	dw_w_signature(w, "b");
	w_pad(w, 4);
	dw_w_bool(w, v);
}

void dw_w_variant_double(struct dw_writer *w, double v)
{
	dw_w_signature(w, "d");
	w_pad(w, 8);
	dw_w_double(w, v);
}

struct dw_array_marker dw_w_array_begin(struct dw_writer *w,
					size_t elem_align)
{
	struct dw_array_marker m;

	dw_w_uint32(w, 0); /* length placeholder */
	m.length_offset = w->len - 4;
	w_pad(w, elem_align);
	m.data_start = w->len;
	return m;
}

void dw_w_array_end(struct dw_writer *w, struct dw_array_marker m)
{
	uint32_t len = (uint32_t)(w->len - m.data_start);

	put_le32(w->buf + m.length_offset, len);
}

/* ------------------------------------------------------------------ */
/* reader                                                             */
/* ------------------------------------------------------------------ */

static int r_need(struct dw_reader *r, size_t n)
{
	return r->off + n <= r->len;
}

static int r_pad(struct dw_reader *r, size_t align)
{
	size_t target = align_up(r->off, align);

	if (target > r->len)
		return -1;
	r->off = target;
	return 0;
}

static int r_u8(struct dw_reader *r, uint8_t *v)
{
	if (!r_need(r, 1))
		return -1;
	*v = r->p[r->off++];
	return 0;
}

int dw_r_uint32(struct dw_reader *r, uint32_t *v)
{
	if (r_pad(r, 4) < 0 || !r_need(r, 4))
		return -1;
	*v = get_le32(r->p + r->off);
	r->off += 4;
	return 0;
}

int dw_r_bool(struct dw_reader *r, int *v)
{
	uint32_t u;

	if (dw_r_uint32(r, &u) < 0)
		return -1;
	*v = u ? 1 : 0;
	return 0;
}

static int r_bytes(struct dw_reader *r, const char **s, size_t *n, size_t len)
{
	if (r->off + len > r->len)
		return -1;
	*s = (const char *)(r->p + r->off);
	if (n)
		*n = len;
	r->off += len;
	return 0;
}

int dw_r_string(struct dw_reader *r, const char **s, size_t *n)
{
	uint32_t len;

	if (dw_r_uint32(r, &len) < 0)
		return -1;
	if (len >= DW_MAX_MESSAGE)
		return -1;
	if (r->off + (size_t)len + 1 > r->len)
		return -1;
	*s = (const char *)(r->p + r->off);
	if (n)
		*n = len;
	r->off += (size_t)len + 1;
	return 0;
}

int dw_r_signature(struct dw_reader *r, const char **s, size_t *n)
{
	uint8_t len;

	if (r_u8(r, &len) < 0)
		return -1;
	return r_bytes(r, s, n, (size_t)len + 1);
}

int dw_r_variant(struct dw_reader *r, char sig[8], struct dw_reader *value)
{
	const char *s;
	size_t n;

	if (dw_r_signature(r, &s, &n) < 0)
		return -1;
	if (n == 0 || n > 7)
		return -1;
	memcpy(sig, s, n);
	sig[n - 1] = '\0';
	if (value)
		*value = *r;
	return 0;
}

void dw_r_skip(struct dw_reader *r, size_t n)
{
	if (r->off + n <= r->len)
		r->off += n;
	else
		r->off = r->len;
}

/* Skip the value of a variant with type signature sig.  Header fields this
 * implementation does not interpret are ignored as the D-Bus specification
 * requires; in practice that is UNIX_FDS (9), which GLib/GDBus clients put
 * on every method call, with signature "u". */
static int skip_variant_value(struct dw_reader *r, const char *sig)
{
	uint32_t u;
	uint8_t y;
	int b;
	const char *s;
	size_t n;

	switch (sig[0]) {
	case 'u':
		return dw_r_uint32(r, &u);
	case 'b':
		return dw_r_bool(r, &b);
	case 'y':
		return r_u8(r, &y);
	case 's':
	case 'o':
		return dw_r_string(r, &s, &n);
	case 'g':
		return dw_r_signature(r, &s, &n);
	default:
		return -1;
	}
}

/* ------------------------------------------------------------------ */
/* outgoing messages                                                  */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd pfd = { .fd = fd, .events = POLLOUT };

				if (poll(&pfd, 1, 5000) <= 0)
					return -1;
				continue;
			}
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

enum header_field {
	HF_PATH = 1,
	HF_INTERFACE = 2,
	HF_MEMBER = 3,
	HF_ERROR_NAME = 4,
	HF_REPLY_SERIAL = 5,
	HF_DESTINATION = 6,
	HF_SENDER = 7,
	HF_SIGNATURE = 8,
	HF_UNIX_FDS = 9,
};

static void add_field_str(struct dw_writer *w, enum header_field code,
			  char sigchar, const char *value)
{
	w_pad(w, 8);
	w_u8(w, (uint8_t)code);
	dw_w_signature(w, sigchar == 'g' ? "g" : (sigchar == 'o' ? "o" : "s"));
	if (sigchar == 'g') {
		dw_w_signature(w, value);
	} else {
		w_pad(w, 4);
		dw_w_string(w, value);
	}
}

static void add_field_u32(struct dw_writer *w, enum header_field code,
			  uint32_t value)
{
	w_pad(w, 8);
	w_u8(w, (uint8_t)code);
	dw_w_signature(w, "u");
	w_pad(w, 4);
	dw_w_uint32(w, value);
}

static int send_message(struct dw_conn *c, uint8_t type, uint8_t flags,
			const char *dest, const char *path, const char *iface,
			const char *member, const char *error_name,
			uint32_t reply_serial, const char *sig,
			void (*fill)(struct dw_writer *, void *), void *ud)
{
	struct dw_writer w = { 0 };
	uint32_t serial = ++c->serial;
	size_t fields_len, body_start, body_len;
	int ret;

	w_reserve(&w, DW_HEADER_SIZE);
	memset(w.buf, 0, DW_HEADER_SIZE);
	w.len = DW_HEADER_SIZE;

	if (path)
		add_field_str(&w, HF_PATH, 'o', path);
	if (iface)
		add_field_str(&w, HF_INTERFACE, 's', iface);
	if (member)
		add_field_str(&w, HF_MEMBER, 's', member);
	if (error_name)
		add_field_str(&w, HF_ERROR_NAME, 's', error_name);
	if (reply_serial)
		add_field_u32(&w, HF_REPLY_SERIAL, reply_serial);
	if (dest)
		add_field_str(&w, HF_DESTINATION, 's', dest);
	if (sig && sig[0])
		add_field_str(&w, HF_SIGNATURE, 'g', sig);

	fields_len = w.len - DW_HEADER_SIZE;
	w_pad(&w, 8);
	body_start = w.len;
	if (fill)
		fill(&w, ud);
	body_len = w.len - body_start;

	w.buf[0] = 'l';
	w.buf[1] = type;
	w.buf[2] = flags;
	w.buf[3] = 1;
	put_le32(w.buf + 4, (uint32_t)body_len);
	put_le32(w.buf + 8, serial);
	put_le32(w.buf + 12, (uint32_t)fields_len);

	ret = send_all(c->fd, w.buf, w.len) < 0 ? -1 : 0;
	free(w.buf);
	if (ret < 0)
		return -1;
	return (int)serial;
}

int dw_send_method(struct dw_conn *c, const char *dest, const char *path,
		   const char *iface, const char *member, const char *sig,
		   void (*fill)(struct dw_writer *, void *), void *ud,
		   uint32_t flags)
{
	return send_message(c, DW_TYPE_METHOD_CALL, (uint8_t)flags, dest, path,
			    iface, member, NULL, 0, sig, fill, ud);
}

int dw_send_return(struct dw_conn *c, const struct dw_msg *call,
		   const char *sig,
		   void (*fill)(struct dw_writer *, void *), void *ud)
{
	if (call->flags & DW_FLAG_NO_REPLY_EXPECTED)
		return 0;
	return send_message(c, DW_TYPE_METHOD_RETURN, 0, call->sender,
			    NULL, NULL, NULL, NULL, call->serial, sig, fill, ud);
}

struct error_ctx {
	const char *message;
};

static void fill_error_string(struct dw_writer *w, void *ud)
{
	struct error_ctx *ctx = ud;

	dw_w_string(w, ctx->message);
}

int dw_send_error(struct dw_conn *c, const struct dw_msg *call,
		  const char *error_name, const char *message)
{
	struct error_ctx ctx = { .message = message };

	if (call->flags & DW_FLAG_NO_REPLY_EXPECTED)
		return 0;
	if (send_message(c, DW_TYPE_ERROR, 0, call->sender, NULL, NULL, NULL,
			 error_name, call->serial, "s", fill_error_string,
			 &ctx) < 0)
		return -1;
	return 0;
}

int dw_send_signal(struct dw_conn *c, const char *path, const char *iface,
		   const char *member, const char *sig,
		   void (*fill)(struct dw_writer *, void *), void *ud)
{
	return send_message(c, DW_TYPE_SIGNAL, 0, NULL, path, iface, member,
			    NULL, 0, sig, fill, ud);
}

/* ------------------------------------------------------------------ */
/* incoming messages                                                  */
/* ------------------------------------------------------------------ */

static void copy_field(char *dst, size_t dstlen, const char *src, size_t n)
{
	if (n >= dstlen)
		n = dstlen - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static int parse_header_fields(const uint8_t *raw, size_t fields_len,
			       struct dw_msg *m)
{
	struct dw_reader r = { .p = raw, .len = DW_HEADER_SIZE + fields_len,
			       .off = DW_HEADER_SIZE };

	while (r.off < r.len) {
		uint8_t code;
		char sig[8];
		struct dw_reader value;
		const char *s;
		size_t n;
		uint32_t u;

		if (r_pad(&r, 8) < 0)
			return -1;
		if (r.off == r.len)
			break;
		if (r_u8(&r, &code) < 0)
			return -1;
		if (dw_r_variant(&r, sig, &value) < 0)
			return -1;
		switch (code) {
		case HF_PATH:
		case HF_INTERFACE:
		case HF_MEMBER:
		case HF_DESTINATION:
		case HF_SENDER:
			if (strcmp(sig, "s") && strcmp(sig, "o"))
				return -1;
			if (dw_r_string(&value, &s, &n) < 0)
				return -1;
			if (code == HF_PATH)
				copy_field(m->path, sizeof(m->path), s, n);
			else if (code == HF_INTERFACE)
				copy_field(m->iface, sizeof(m->iface), s, n);
			else if (code == HF_MEMBER)
				copy_field(m->member, sizeof(m->member), s, n);
			else if (code == HF_DESTINATION)
				copy_field(m->dest, sizeof(m->dest), s, n);
			else if (code == HF_SENDER)
				copy_field(m->sender, sizeof(m->sender), s, n);
			break;
		case HF_ERROR_NAME:
			if (strcmp(sig, "s"))
				return -1;
			if (dw_r_string(&value, &s, &n) < 0)
				return -1;
			copy_field(m->error_name, sizeof(m->error_name), s, n);
			break;
		case HF_SIGNATURE:
			if (strcmp(sig, "g"))
				return -1;
			if (dw_r_signature(&value, &s, &n) < 0)
				return -1;
			copy_field(m->sig, sizeof(m->sig), s, n);
			break;
		case HF_REPLY_SERIAL:
			if (strcmp(sig, "u"))
				return -1;
			if (dw_r_uint32(&value, &u) < 0)
				return -1;
			m->reply_serial = u;
			m->has_reply_serial = 1;
			break;
		case HF_UNIX_FDS:
			if (strcmp(sig, "u"))
				return -1;
			if (dw_r_uint32(&value, &u) < 0)
				return -1;
			break;
		default:
			/* Unknown header field: ignore its value (D-Bus spec
			 * requires this; unknown types remain an error). */
			if (skip_variant_value(&value, sig) < 0)
				return -1;
			break;
		}
		/* values are parsed through a copy; advance the outer reader */
		r.off = value.off;
	}
	return 0;
}

int dw_next_message(struct dw_conn *c, struct dw_msg *m)
{
	uint32_t body_len, fields_len;
	size_t header_end, total, pad;
	const uint8_t *raw;

	memset(m, 0, sizeof(*m));
	if (c->inlen < DW_HEADER_SIZE)
		return 0;
	raw = c->in;
	if (raw[0] != 'l')
		return -1;
	if (raw[3] != 1)
		return -1;
	body_len = get_le32(raw + 4);
	fields_len = get_le32(raw + 12);
	if (body_len > DW_MAX_MESSAGE || fields_len > DW_MAX_MESSAGE)
		return -1;
	header_end = DW_HEADER_SIZE + fields_len;
	pad = align_up(header_end, 8) - header_end;
	total = header_end + pad + body_len;
	if (total > DW_MAX_MESSAGE)
		return -1;
	if (c->inlen < total)
		return 0;

	m->type = raw[1];
	m->flags = raw[2];
	m->serial = get_le32(raw + 8);
	m->body_len = body_len;
	m->raw = raw;
	m->raw_len = total;
	m->body_off = header_end + pad;
	if (parse_header_fields(raw, fields_len, m) < 0)
		return -1;

	memmove(c->in, c->in + total, c->inlen - total);
	c->inlen -= total;
	return 1;
}

int dw_conn_fill(struct dw_conn *c, int timeout_ms)
{
	struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
	ssize_t n;
	int r;

	if (c->inlen == c->incap) {
		size_t cap = c->incap ? c->incap * 2 : 8192;
		uint8_t *nb = realloc(c->in, cap);

		if (!nb)
			return -1;
		c->in = nb;
		c->incap = cap;
	}
	if (timeout_ms >= 0) {
		r = poll(&pfd, 1, timeout_ms);
		if (r < 0)
			return errno == EINTR ? 0 : -1;
		if (r == 0)
			return 0;
	}
	n = recv(c->fd, c->in + c->inlen, c->incap - c->inlen, 0);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;
		return -1;
	}
	if (n == 0)
		return -1; /* peer closed */
	c->inlen += (size_t)n;
	return 1;
}

/* ------------------------------------------------------------------ */
/* authentication and connection setup                                */
/* ------------------------------------------------------------------ */

static int wait_fd(int fd, short events, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = events };
	int r;

	do {
		r = poll(&pfd, 1, timeout_ms);
	} while (r < 0 && errno == EINTR);
	return r;
}

static int read_auth_line(int fd, char *buf, size_t buflen, int timeout_ms)
{
	size_t off = 0;

	while (off + 1 < buflen) {
		char ch;
		ssize_t n;

		if (wait_fd(fd, POLLIN, timeout_ms) <= 0)
			return -1;
		n = recv(fd, &ch, 1, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		if (ch == '\r')
			continue;
		if (ch == '\n') {
			buf[off] = '\0';
			return 0;
		}
		buf[off++] = ch;
	}
	return -1;
}

static int hex_encode(const char *in, char *out, size_t outlen)
{
	static const char hex[] = "0123456789abcdef";
	size_t i, n = strlen(in);

	if (n * 2 + 1 > outlen)
		return -1;
	for (i = 0; i < n; i++) {
		out[i * 2] = hex[(unsigned char)in[i] >> 4];
		out[i * 2 + 1] = hex[(unsigned char)in[i] & 0x0f];
	}
	out[n * 2] = '\0';
	return 0;
}

static int sockaddr_from_address(const char *address, struct sockaddr_un *sa,
				 socklen_t *salen)
{
	const char *p;

	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	if (!address || !*address)
		address = "unix:path=/run/dbus/system_bus_socket";

	p = strstr(address, "unix:path=");
	if (p) {
		const char *path = p + strlen("unix:path=");
		size_t n = strcspn(path, ";,");

		if (n >= sizeof(sa->sun_path))
			return -1;
		memcpy(sa->sun_path, path, n);
		sa->sun_path[n] = '\0';
		*salen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
		return 0;
	}
	p = strstr(address, "unix:abstract=");
	if (p) {
		const char *name = p + strlen("unix:abstract=");
		size_t n = strcspn(name, ";,");

		if (n + 1 >= sizeof(sa->sun_path))
			return -1;
		sa->sun_path[0] = '\0';
		memcpy(sa->sun_path + 1, name, n);
		sa->sun_path[n + 1] = '\0';
		*salen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
		return 0;
	}
	return -1;
}

static int wait_for_reply(struct dw_conn *c, int serial, struct dw_msg *out,
			  char *err, size_t errlen)
{
	int guard = 0;

	for (;;) {
		struct dw_msg m;
		int r;

		while ((r = dw_next_message(c, &m)) == 1) {
			if ((m.type == DW_TYPE_METHOD_RETURN ||
			     m.type == DW_TYPE_ERROR) &&
			    m.has_reply_serial &&
			    m.reply_serial == (uint32_t)serial) {
				*out = m;
				if (m.type == DW_TYPE_ERROR) {
					set_err(err, errlen, "D-Bus error %s",
						m.error_name);
					return -1;
				}
				return 0;
			}
			/* ignore unrelated traffic during setup */
		}
		if (r < 0) {
			set_err(err, errlen, "malformed D-Bus message");
			return -1;
		}
		if (dw_conn_fill(c, 5000) < 0) {
			set_err(err, errlen, "D-Bus connection closed during setup");
			return -1;
		}
		if (++guard > 100) {
			set_err(err, errlen, "timed out waiting for D-Bus reply");
			return -1;
		}
	}
}

struct dw_conn *dw_conn_open(const char *address, char *err, size_t errlen)
{
	struct dw_conn *c;
	struct sockaddr_un sa;
	socklen_t salen;
	char line[512];
	char uid[32], hexuid[64];
	int fd, serial;

	c = calloc(1, sizeof(*c));
	if (!c) {
		set_err(err, errlen, "out of memory");
		return NULL;
	}
	c->fd = -1;

	if (sockaddr_from_address(address, &sa, &salen) < 0) {
		set_err(err, errlen, "unsupported D-Bus address '%s'",
			address ? address : "");
		goto fail;
	}

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		set_err(err, errlen, "socket: %s", strerror(errno));
		goto fail;
	}
	if (connect(fd, (struct sockaddr *)&sa, salen) < 0) {
		set_err(err, errlen, "connect(%s): %s",
			sa.sun_path[0] ? sa.sun_path : address, strerror(errno));
		close(fd);
		goto fail;
	}
	c->fd = fd;

	/* SASL EXTERNAL: NUL, AUTH EXTERNAL <hex-uid>, BEGIN */
	if (send_all(fd, "\0", 1) < 0) {
		set_err(err, errlen, "auth write failed");
		goto fail;
	}
	snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
	if (hex_encode(uid, hexuid, sizeof(hexuid)) < 0) {
		set_err(err, errlen, "uid encoding failed");
		goto fail;
	}
	snprintf(line, sizeof(line), "AUTH EXTERNAL %s\r\n", hexuid);
	if (send_all(fd, line, strlen(line)) < 0) {
		set_err(err, errlen, "auth write failed");
		goto fail;
	}
	line[0] = '\0';
	if (read_auth_line(fd, line, sizeof(line), 5000) < 0 ||
	    strncmp(line, "OK ", 3) != 0) {
		set_err(err, errlen, "D-Bus EXTERNAL auth rejected: %s", line);
		goto fail;
	}
	if (send_all(fd, "BEGIN\r\n", 7) < 0) {
		set_err(err, errlen, "auth BEGIN failed");
		goto fail;
	}

	serial = dw_send_method(c, "org.freedesktop.DBus",
				"/org/freedesktop/DBus", "org.freedesktop.DBus",
				"Hello", NULL, NULL, NULL, 0);
	if (serial < 0) {
		set_err(err, errlen, "could not send Hello");
		goto fail;
	}

	{
		struct dw_msg m;
		struct dw_reader r;
		const char *name;
		size_t n;

		if (wait_for_reply(c, serial, &m, err, errlen) < 0)
			goto fail;
		r.p = m.raw;
		r.len = m.raw_len;
		r.off = m.body_off;
		name = "";
		n = 0;
		if (dw_r_string(&r, &name, &n) == 0)
			copy_field(c->unique, sizeof(c->unique), name, n);
	}

	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
		set_err(err, errlen, "fcntl: %s", strerror(errno));
		goto fail;
	}
	return c;

fail:
	if (c->fd >= 0)
		close(c->fd);
	free(c->in);
	free(c);
	return NULL;
}

void dw_conn_free(struct dw_conn *c)
{
	if (!c)
		return;
	if (c->fd >= 0)
		close(c->fd);
	free(c->in);
	free(c);
}

int dw_conn_fd(const struct dw_conn *c)
{
	return c->fd;
}

struct request_name_ctx {
	const char *name;
};

static void fill_request_name(struct dw_writer *w, void *ud)
{
	struct request_name_ctx *ctx = ud;

	dw_w_string(w, ctx->name);
	dw_w_uint32(w, 0x1 | 0x4); /* ALLOW_REPLACEMENT | DO_NOT_QUEUE */
}

int dw_request_name(struct dw_conn *c, const char *name, char *err,
		    size_t errlen)
{
	struct request_name_ctx ctx = { .name = name };
	struct dw_msg m;
	struct dw_reader r;
	uint32_t result;
	int serial;

	serial = dw_send_method(c, "org.freedesktop.DBus",
				"/org/freedesktop/DBus", "org.freedesktop.DBus",
				"RequestName", "su", fill_request_name, &ctx, 0);
	if (serial < 0) {
		set_err(err, errlen, "could not send RequestName");
		return -1;
	}
	if (wait_for_reply(c, serial, &m, err, errlen) < 0)
		return -1;
	r.p = m.raw;
	r.len = m.raw_len;
	r.off = m.body_off;
	if (dw_r_uint32(&r, &result) < 0) {
		set_err(err, errlen, "malformed RequestName reply");
		return -1;
	}
	/* 1 = primary owner, 4 = already owner */
	if (result != 1 && result != 4) {
		set_err(err, errlen, "could not own %s (RequestName result %u)",
			name, result);
		return -1;
	}
	return 0;
}

struct add_match_ctx {
	const char *rule;
};

static void fill_add_match(struct dw_writer *w, void *ud)
{
	struct add_match_ctx *ctx = ud;

	dw_w_string(w, ctx->rule);
}

int dw_add_match(struct dw_conn *c, const char *rule)
{
	struct add_match_ctx ctx = { .rule = rule };

	if (dw_send_method(c, "org.freedesktop.DBus",
			   "/org/freedesktop/DBus", "org.freedesktop.DBus",
			   "AddMatch", "s", fill_add_match, &ctx,
			   DW_FLAG_NO_REPLY_EXPECTED) < 0)
		return -1;
	return 0;
}
