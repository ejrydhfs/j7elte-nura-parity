/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dbus-wire.h - minimal D-Bus binary protocol client/server helpers
 *
 * Implements only what j7elte-sensors needs: SASL EXTERNAL authentication,
 * Hello/RequestName, method calls/replies/errors, and signals with the
 * standard marshalling formats used by net.hadess.SensorProxy.
 *
 * No libdbus/GLib dependency, so the daemon builds against musl and can be
 * statically linked.
 */
#ifndef J7ELTE_DBUS_WIRE_H
#define J7ELTE_DBUS_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define DW_TYPE_METHOD_CALL 1
#define DW_TYPE_METHOD_RETURN 2
#define DW_TYPE_ERROR 3
#define DW_TYPE_SIGNAL 4

#define DW_FLAG_NO_REPLY_EXPECTED 0x1

struct dw_conn;
struct dw_writer;

struct dw_msg {
	uint8_t type;
	uint8_t flags;
	uint32_t serial;
	uint32_t reply_serial;
	int has_reply_serial;
	uint32_t body_len;

	char path[256];
	char iface[128];
	char member[64];
	char sender[128];
	char dest[128];
	char sig[128];
	char error_name[128];

	const uint8_t *raw; /* message start */
	size_t raw_len;
	size_t body_off;    /* offset of body inside raw */
};

struct dw_reader {
	const uint8_t *p;
	size_t len;
	size_t off;
};

/* Connection handling. Returns NULL and fills err on failure. */
struct dw_conn *dw_conn_open(const char *address, char *err, size_t errlen);
void dw_conn_free(struct dw_conn *c);
int dw_conn_fd(const struct dw_conn *c);

/* Non-blocking convenience: poll and read whatever is available.
 * Returns 1 if bytes were read, 0 on timeout/no data, -1 on error. */
int dw_conn_fill(struct dw_conn *c, int timeout_ms);

/* Extract the next complete message. Returns 1 on success, 0 if more data
 * is needed, -1 on protocol error. The returned pointers live in the
 * connection's buffer until the next call to dw_conn_fill(). */
int dw_next_message(struct dw_conn *c, struct dw_msg *m);

/* Send a method call (optionally expecting a reply). */
int dw_send_method(struct dw_conn *c, const char *dest, const char *path,
		   const char *iface, const char *member, const char *sig,
		   void (*fill)(struct dw_writer *, void *), void *ud,
		   uint32_t flags);

/* Reply helpers. */
int dw_send_return(struct dw_conn *c, const struct dw_msg *call,
		   const char *sig,
		   void (*fill)(struct dw_writer *, void *), void *ud);
int dw_send_error(struct dw_conn *c, const struct dw_msg *call,
		  const char *error_name, const char *message);
int dw_send_signal(struct dw_conn *c, const char *path, const char *iface,
		   const char *member, const char *sig,
		   void (*fill)(struct dw_writer *, void *), void *ud);

/* Blocking helpers used during initialisation. */
int dw_request_name(struct dw_conn *c, const char *name, char *err,
		    size_t errlen);
int dw_add_match(struct dw_conn *c, const char *rule);

/* ---- writer API (used from fill callbacks) ------------------------- */
void dw_w_string(struct dw_writer *w, const char *s);
void dw_w_object_path(struct dw_writer *w, const char *s);
void dw_w_signature(struct dw_writer *w, const char *s);
void dw_w_uint32(struct dw_writer *w, uint32_t v);
void dw_w_bool(struct dw_writer *w, int v);
void dw_w_double(struct dw_writer *w, double v);
void dw_w_align(struct dw_writer *w, size_t align);

void dw_w_variant_string(struct dw_writer *w, const char *s);
void dw_w_variant_bool(struct dw_writer *w, int v);
void dw_w_variant_double(struct dw_writer *w, double v);

/* Opens/closes an array (4-byte length prefix followed by alignment
 * padding to the element alignment).  The D-Bus array length excludes that
 * padding.  Elements must be written between the two calls. */
struct dw_array_marker {
	size_t length_offset;
	size_t data_start;
};
struct dw_array_marker dw_w_array_begin(struct dw_writer *w,
					size_t elem_align);
void dw_w_array_end(struct dw_writer *w, struct dw_array_marker m);

/* ---- reader API ---------------------------------------------------- */
int dw_r_string(struct dw_reader *r, const char **s, size_t *n);
int dw_r_signature(struct dw_reader *r, const char **s, size_t *n);
int dw_r_uint32(struct dw_reader *r, uint32_t *v);
int dw_r_bool(struct dw_reader *r, int *v);
/* Reads a variant signature and points value at the contained value. */
int dw_r_variant(struct dw_reader *r, char sig[8], struct dw_reader *value);
void dw_r_skip(struct dw_reader *r, size_t n);

#endif /* J7ELTE_DBUS_WIRE_H */
