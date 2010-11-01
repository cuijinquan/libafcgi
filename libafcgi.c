
#include "libafcgi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

typedef struct fastcgi_queue_link {
	GList queue_link;
	enum { FASTCGI_QUEUE_STRING, FASTCGI_QUEUE_BYTEARRAY } elem_type;
} fastcgi_queue_link;

/* some util functions */
#define GSTR_LEN(x) ((x) ? (x)->str : ""), ((x) ? (x)->len : 0)
#define GBARR_LEN(x) ((x)->data), ((x)->len)
#define UNUSED(x) ((void)(x))
#define ERROR(...) g_printerr("libafcgi.c:" G_STRINGIFY(__LINE__) ": " __VA_ARGS__)

static void fd_init(int fd) {
#ifdef _WIN32
	int i = 1;
#endif
#ifdef FD_CLOEXEC
	/* close fd on exec (cgi) */
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
#ifdef O_NONBLOCK
	fcntl(fd, F_SETFL, O_NONBLOCK | O_RDWR);
#elif defined _WIN32
	ioctlsocket(fd, FIONBIO, &i);
#endif
}

static fastcgi_queue_link* fastcgi_queue_link_new_string(GString *s) {
	fastcgi_queue_link *l = g_slice_new0(fastcgi_queue_link);
	l->queue_link.data = s;
	l->elem_type = FASTCGI_QUEUE_STRING;
	return l;
}

static fastcgi_queue_link* fastcgi_queue_link_new_bytearray(GByteArray *a) {
	fastcgi_queue_link *l = g_slice_new0(fastcgi_queue_link);
	l->queue_link.data = a;
	l->elem_type = FASTCGI_QUEUE_BYTEARRAY ;
	return l;
}

static void fastcgi_queue_link_free(fastcgi_queue *queue, fastcgi_queue_link *l) {
	switch (l->elem_type) {
	case FASTCGI_QUEUE_STRING:
		if (queue) queue->length -= ((GString*)l->queue_link.data)->len;
		g_string_free(l->queue_link.data, TRUE);
		break;
	case FASTCGI_QUEUE_BYTEARRAY:
		if (queue) queue->length -= ((GByteArray*)l->queue_link.data)->len;
		g_byte_array_free(l->queue_link.data, TRUE);
		break;
	}
	g_slice_free(fastcgi_queue_link, l);
}

static fastcgi_queue_link *fastcgi_queue_peek_head(fastcgi_queue *queue) {
	return (fastcgi_queue_link*) g_queue_peek_head_link(&queue->queue);
}

static fastcgi_queue_link *fastcgi_queue_pop_head(fastcgi_queue *queue) {
	return (fastcgi_queue_link*) g_queue_pop_head_link(&queue->queue);
}

void fastcgi_queue_clear(fastcgi_queue *queue) {
	fastcgi_queue_link *l;
	queue->offset = 0;
	while (NULL != (l = fastcgi_queue_pop_head(queue))) {
		fastcgi_queue_link_free(queue, l);
	}
	g_assert(0 == queue->length);
}

void fastcgi_queue_append_string(fastcgi_queue *queue, GString *buf) {
	fastcgi_queue_link *l;
	if (!buf) return;
	if (!buf->len) { g_string_free(buf, TRUE); return; }
	l = fastcgi_queue_link_new_string(buf);
	g_queue_push_tail_link(&queue->queue, (GList*) l);
	queue->length += buf->len;
}

void fastcgi_queue_append_bytearray(fastcgi_queue *queue, GByteArray *buf) {
	fastcgi_queue_link *l;
	if (!buf) return;
	if (!buf->len) { g_byte_array_free(buf, TRUE); return; }
	l = fastcgi_queue_link_new_bytearray(buf);
	g_queue_push_tail_link(&queue->queue, (GList*) l);
	queue->length += buf->len;
}

/* return values: 0 ok, -1 error, -2 con closed */
gint fastcgi_queue_write(int fd, fastcgi_queue *queue, gsize max_write) {
	gsize rem_write = max_write;
	g_assert(rem_write <= G_MAXSSIZE);
#ifdef TCP_CORK
	int corked = 0;
#endif

#ifdef TCP_CORK
	/* Linux: put a cork into the socket as we want to combine the write() calls
	 * but only if we really have multiple chunks
	 */
	if (queue->queue.length > 1) {
		corked = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	while (rem_write > 0 && queue->length > 0) {
		fastcgi_queue_link *l = fastcgi_queue_peek_head(queue);
		gsize towrite, datalen;
		gssize res;
		gchar *data;
		switch (l->elem_type) {
		case FASTCGI_QUEUE_STRING:
			data = ((GString*) l->queue_link.data)->str;
			datalen = towrite = ((GString*) l->queue_link.data)->len;
			break;
		case FASTCGI_QUEUE_BYTEARRAY:
			data = (gchar*) ((GByteArray*) l->queue_link.data)->data;
			datalen = towrite = ((GByteArray*) l->queue_link.data)->len;
			break;
		default:
			g_error("invalid fastcgi_queue_link type\n");
		}
		towrite -= queue->offset; data += queue->offset;
		if (towrite > rem_write) towrite = rem_write;
		res = write(fd, data, towrite);
		if (-1 == res) {
#ifdef TCP_CORK
			if (corked) {
				corked = 0;
				setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
			}
#endif
			switch (errno) {
			case EINTR:
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				return 0; /* try again later */
			case ECONNRESET:
			case EPIPE:
				return -2;
			default:
				ERROR("write to fd=%d failed, %s\n", fd, g_strerror(errno) );
				return -1;
			}
		} else {
			queue->offset += res;
			rem_write -= res;
			if (queue->offset == datalen) {
				queue->offset = 0;
				fastcgi_queue_link_free(queue, fastcgi_queue_pop_head(queue));
			}
		}
	}

#ifdef TCP_CORK
	if (corked) {
		corked = 0;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	return 0;
}


static void ev_io_add_events(struct ev_loop *loop, ev_io *watcher, int events) {
	if ((watcher->events & events) == events) return;
	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events | events);
	ev_io_start(loop, watcher);
}

static void ev_io_rem_events(struct ev_loop *loop, ev_io *watcher, int events) {
	if (0 == (watcher->events & events)) return;
	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events & ~events);
	ev_io_start(loop, watcher);
}
/* end: some util functions */

static const guint8 __padding[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static void append_padding_str(GString *s, guint8 padlen) {
	g_string_append_len(s, (const gchar*) __padding, padlen);
}

static void append_padding_bytearray(GByteArray *a, guint8 padlen) {
	g_byte_array_append(a, __padding, padlen);
}

/* returns padding length */
static guint8 stream_build_fcgi_record(GByteArray *buf, guint8 type, guint16 requestid, guint16 datalen) {
	guint8 padlen = (8 - (datalen & 0x7)) % 8; /* padding must be < 8 */

	/* alloc enough space */
	g_byte_array_set_size(buf, FCGI_HEADER_LEN);
	buf->len = 0;

	buf->data[buf->len++] = FCGI_VERSION_1;
	buf->data[buf->len++] = type;
	buf->data[buf->len++] = (guint8) (requestid >> 8);
	buf->data[buf->len++] = (guint8) (requestid);
	buf->data[buf->len++] = (guint8) (datalen >> 8);
	buf->data[buf->len++] = (guint8) (datalen);
	buf->data[buf->len++] = padlen;
	buf->data[buf->len++] = 0;
	return padlen;
}

/* returns padding length */
static guint8 stream_send_fcgi_record(fastcgi_queue *out, guint8 type, guint16 requestid, guint16 datalen) {
	GByteArray *record = g_byte_array_sized_new(FCGI_HEADER_LEN);
	guint8 padlen = stream_build_fcgi_record(record, type, requestid, datalen);
	fastcgi_queue_append_bytearray(out, record);
	return padlen;
}

static void stream_send_data(fastcgi_queue *out, guint8 type, guint16 requestid, const guint8 *data, size_t datalen) {
	while (datalen > 0) {
		guint16 tosend = (datalen > G_MAXUINT16) ? G_MAXUINT16 : datalen;
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, tosend);
		GByteArray *buf = g_byte_array_sized_new(tosend + padlen);
		g_byte_array_append(buf, data, tosend);
		append_padding_bytearray(buf, padlen);
		fastcgi_queue_append_bytearray(out, buf);
		data += tosend;
		datalen -= tosend;
	}
}

/* kills string */
static void stream_send_string(fastcgi_queue *out, guint8 type, guint16 requestid, GString *data) {
	if (data->len > G_MAXUINT16) {
		stream_send_data(out, type, requestid, (const guint8*) GSTR_LEN(data));
		g_string_free(data, TRUE);
	} else {
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, data->len);
		append_padding_str(data, padlen);
		fastcgi_queue_append_string(out, data);
	}
}

/* kills bytearray */
static void stream_send_bytearray(fastcgi_queue *out, guint8 type, guint16 requestid, GByteArray *data) {
	if (data->len > G_MAXUINT16) {
		stream_send_data(out, type, requestid, GBARR_LEN(data));
		g_byte_array_free(data, TRUE);
	} else {
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, data->len);
		append_padding_bytearray(data, padlen);
		fastcgi_queue_append_bytearray(out, data);
	}
}

static void stream_send_end_request(fastcgi_queue *out, guint16 requestID, gint32 appStatus, enum FCGI_ProtocolStatus status) {
	GByteArray *record;

	record = g_byte_array_sized_new(16);
	stream_build_fcgi_record(record, FCGI_END_REQUEST, requestID, 8);

	/* alloc enough space */
	g_byte_array_set_size(record, 16);
	record->len = 8;

	appStatus = htonl(appStatus);
	g_byte_array_append(record, (const guchar*) &appStatus, sizeof(appStatus));
	record->data[record->len++] = status;
	g_byte_array_append(record, __padding, 3);
	fastcgi_queue_append_bytearray(out, record);
}


static void write_queue(fastcgi_connection *fcon) {
	if (fcon->closing) return;

	if (fastcgi_queue_write(fcon->fd, &fcon->write_queue, 256*1024) < 0) {
		fastcgi_connection_close(fcon);
		return;
	}

	if (fcon->fsrv->callbacks->cb_wrote_data) {
		fcon->fsrv->callbacks->cb_wrote_data(fcon);
	}

	if (!fcon->closing) {
		if (fcon->write_queue.length > 0) {
			ev_io_add_events(fcon->fsrv->loop, &fcon->fd_watcher, EV_WRITE);
		} else {
			ev_io_rem_events(fcon->fsrv->loop, &fcon->fd_watcher, EV_WRITE);
			if (0 == fcon->requestID) {
				if (!(fcon->flags & FCGI_KEEP_CONN)) {
					fastcgi_connection_close(fcon);
				}
			}
		}
	}
}

static GByteArray* read_chunk(fastcgi_connection *fcon, guint maxlen) {
	gssize res;
	GByteArray *buf;
	int tmp_errno;

	buf = g_byte_array_sized_new(maxlen);
	g_byte_array_set_size(buf, maxlen);
	if (0 == maxlen) return buf;

	res = read(fcon->fd, buf->data, maxlen);
	if (res == -1) {
		tmp_errno = errno;
		g_byte_array_free(buf, TRUE);
		errno = tmp_errno;
		return NULL;
	} else if (res == 0) {
		g_byte_array_free(buf, TRUE);
		errno = ECONNRESET;
		return NULL;
	} else {
		g_byte_array_set_size(buf, res);
		return buf;
	}
}

/* read content + padding, but only returns content data. decrements counters */
static GByteArray *read_content(fastcgi_connection *fcon) {
	GByteArray *buf;

	buf = read_chunk(fcon, fcon->content_remaining + fcon->padding_remaining);
	if (!buf) return NULL;
	if (buf->len > fcon->content_remaining) {
		fcon->padding_remaining -= (buf->len - fcon->content_remaining);
		g_byte_array_set_size(buf, fcon->content_remaining);
		fcon->content_remaining = 0;
	} else {
		fcon->content_remaining -= buf->len;
	}
	return buf;
}

static gboolean read_append_chunk(fastcgi_connection *fcon, GByteArray *buf) {
	gssize res;
	int tmp_errno;
	guint curlen = buf->len;
	const guint maxlen = fcon->content_remaining + fcon->padding_remaining;
	if (0 == maxlen) return TRUE;

	g_byte_array_set_size(buf, curlen + maxlen);
	res = read(fcon->fd, buf->data + curlen, maxlen);
	if (res == -1) {
		tmp_errno = errno;
		g_byte_array_set_size(buf, curlen);
		errno = tmp_errno;
		return FALSE;
	} else if (res == 0) {
		g_byte_array_set_size(buf, curlen);
		errno = ECONNRESET;
		return FALSE;
	} else {
		/* remove padding data */
		if (res > fcon->content_remaining) {
			fcon->padding_remaining -= res - fcon->content_remaining;
			res = fcon->content_remaining;
		}
		g_byte_array_set_size(buf, curlen + res);
		fcon->content_remaining -= res;
		return TRUE;
	}
}

static gboolean read_key_value(fastcgi_connection *fcon, GByteArray *buf, guint *pos, gchar **key, guint *keylen, gchar **value, guint *valuelen) {
	const unsigned char *data = (const unsigned char*) buf->data;
	guint32 klen, vlen;
	guint p = *pos, len = buf->len;

	if (len - p < 2) return FALSE;

	klen = data[p++];
	if (klen & 0x80) {
		if (len - p < 100) return FALSE;
		klen = ((klen & 0x7f) << 24) | (data[p] << 16) | (data[p+1] << 8) | data[p+2];
		p += 3;
	}
	vlen = data[p++];
	if (vlen & 0x80) {
		if (len - p < 100) return FALSE;
		vlen = ((vlen & 0x7f) << 24) | (data[p] << 16) | (data[p+1] << 8) | data[p+2];
		p += 3;
	}
	if (klen > FASTCGI_MAX_KEYLEN || vlen > FASTCGI_MAX_VALUELEN) {
		fastcgi_connection_close(fcon);
		return FALSE;
	}
	if (len - p < klen + vlen) return FALSE;
	*key = (gchar*) &buf->data[p];
	*keylen = klen;
	p += klen;
	*value = (gchar*) &buf->data[p];
	*valuelen = vlen;
	p += vlen;
	*pos = p;
	return TRUE;
}

static void parse_params(const fastcgi_callbacks *fcbs, fastcgi_connection *fcon) {
	if (!fcon->current_header.contentLength) {
		fcbs->cb_new_request(fcon);
		g_byte_array_set_size(fcon->parambuf, 0);
	} else {
		guint pos = 0, keylen = 0, valuelen = 0;
		gchar *key = NULL, *value = NULL;
		while (read_key_value(fcon, fcon->parambuf, &pos, &key, &keylen, &value, &valuelen)) {
			GString *gkey = g_string_new_len(key, keylen);
			GString *gvalue = g_string_new_len(value, valuelen);
			g_hash_table_insert(fcon->environ, gkey, gvalue);
		}
		if (!fcon->closing)
			g_byte_array_remove_range(fcon->parambuf, 0, pos);
	}
}

static void parse_get_values(fastcgi_connection *fcon) {
	/* just send the request back and don't insert results */
	GByteArray *tmp = g_byte_array_sized_new(0);
	stream_send_bytearray(&fcon->write_queue, FCGI_GET_VALUES_RESULT, 0, fcon->buffer);
	*fcon->buffer = *tmp;
	/* TODO: provide get-values result */
}

static void read_queue(fastcgi_connection *fcon) {
	gssize res;
	GByteArray *buf;
	const fastcgi_callbacks *fcbs = fcon->fsrv->callbacks;

	for (;;) {
		if (fcon->closing || fcon->read_suspended) return;

		if (fcon->headerbuf_used < 8) {
			const unsigned char *data = fcon->headerbuf;
			res = read(fcon->fd, fcon->headerbuf + fcon->headerbuf_used, 8 - fcon->headerbuf_used);
			if (0 == res) { errno = ECONNRESET; goto handle_error; }
			if (-1 == res) goto handle_error;
			fcon->headerbuf_used += res;
			if (fcon->headerbuf_used < 8) return; /* need more data */

			fcon->current_header.version = data[0];
			fcon->current_header.type = data[1];
			fcon->current_header.requestID = (data[2] << 8) | (data[3]);
			fcon->current_header.contentLength = (data[4] << 8) | (data[5]);
			fcon->current_header.paddingLength = data[6];
			fcon->content_remaining = fcon->current_header.contentLength;
			fcon->padding_remaining = fcon->current_header.paddingLength;
			fcon->first = TRUE;
			g_byte_array_set_size(fcon->buffer, 0);

			if (fcon->current_header.version != FCGI_VERSION_1) {
				fastcgi_connection_close(fcon);
				return;
			}
		}

		if (fcon->current_header.type != FCGI_BEGIN_REQUEST &&
		    (0 != fcon->current_header.requestID) && fcon->current_header.requestID != fcon->requestID) {
		    /* ignore packet data */
			if (0 != fcon->content_remaining + fcon->padding_remaining) {
				if (NULL == (buf = read_content(fcon))) goto handle_error;
				g_byte_array_free(buf, TRUE);
			}
			if (0 == fcon->content_remaining + fcon->padding_remaining) {
				fcon->headerbuf_used = 0;
			}
			continue;
		}

		if (fcon->first || fcon->content_remaining) {
			fcon->first = FALSE;
			switch (fcon->current_header.type) {
			case FCGI_BEGIN_REQUEST:
				if (8 != fcon->current_header.contentLength || 0 == fcon->current_header.requestID) goto error;
				if (!read_append_chunk(fcon, fcon->buffer)) goto handle_error;
				if (0 == fcon->content_remaining) {
					if (fcon->requestID) {
						stream_send_end_request(&fcon->write_queue, fcon->current_header.requestID, 0, FCGI_CANT_MPX_CONN);
					} else {
						unsigned char *data = (unsigned char*) fcon->buffer->data;
						fcon->requestID = fcon->current_header.requestID;
						fcon->role = (data[0] << 8) | (data[1]);
						fcon->flags = data[2];
						g_byte_array_set_size(fcon->parambuf, 0);
					}
				}
				break;
			case FCGI_ABORT_REQUEST:
				if (0 != fcon->current_header.contentLength || 0 == fcon->current_header.requestID) goto error;
				fcbs->cb_request_aborted(fcon);
				break;
			case FCGI_END_REQUEST:
				goto error; /* invalid type */
			case FCGI_PARAMS:
				if (0 == fcon->current_header.requestID) goto error;
				if (!read_append_chunk(fcon, fcon->parambuf)) goto handle_error;
				parse_params(fcbs, fcon);
				break;
			case FCGI_STDIN:
				if (0 == fcon->current_header.requestID) goto error;
				buf = NULL;
				if (0 != fcon->content_remaining &&
				    NULL == (buf = read_content(fcon))) goto handle_error;
				if (fcbs->cb_received_stdin) {
					fcbs->cb_received_stdin(fcon, buf);
				} else {
					g_byte_array_free(buf, TRUE);
				}
				break;
			case FCGI_STDOUT:
				goto error; /* invalid type */
			case FCGI_STDERR:
				goto error; /* invalid type */
			case FCGI_DATA:
				if (0 == fcon->current_header.requestID) goto error;
				buf = NULL;
				if (0 != fcon->content_remaining &&
				    NULL == (buf = read_content(fcon))) goto handle_error;
				if (fcbs->cb_received_data) {
					fcbs->cb_received_data(fcon, buf);
				} else {
					g_byte_array_free(buf, TRUE);
				}
				break;
			case FCGI_GET_VALUES:
				if (0 != fcon->current_header.requestID) goto error;
				if (!read_append_chunk(fcon, fcon->buffer)) goto handle_error;
				if (0 == fcon->content_remaining)
					parse_get_values(fcon);
				break;
			case FCGI_GET_VALUES_RESULT:
				goto error; /* invalid type */
				break;
			case FCGI_UNKNOWN_TYPE:
				/* we didn't send anything fancy, so this is not expected */
				goto error; /* invalid type */
			default:
				break;
			}
		}

		if (0 == fcon->content_remaining) {
			if (0 == fcon->padding_remaining) {
				fcon->headerbuf_used = 0;
			} else {
				if (NULL == (buf = read_chunk(fcon, fcon->padding_remaining))) goto handle_error;
				fcon->padding_remaining -= buf->len;
				if (0 == fcon->padding_remaining) {
					fcon->headerbuf_used = 0;
				}
				g_byte_array_free(buf, TRUE);
			}
		}

	}

	return;

handle_error:
	switch (errno) {
	case EINTR:
	case EAGAIN:
#if EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
		return; /* try again later */
	case ECONNRESET:
		break;
	default:
		ERROR("read from fd=%d failed, %s\n", fcon->fd, g_strerror(errno) );
		break;
	}

error:
	if (0 != fcon->requestID)
		fcbs->cb_request_aborted(fcon);
	fastcgi_connection_close(fcon);
}

static void fastcgi_connection_fd_cb(struct ev_loop *loop, ev_io *w, int revents) {
	fastcgi_connection *fcon = (fastcgi_connection*) w->data;
	UNUSED(loop);

	if (revents & EV_READ) {
		read_queue(fcon);
	}

	if (revents & EV_WRITE) {
		write_queue(fcon);
	}
}

static void _g_string_destroy(gpointer data) {
	g_string_free(data, TRUE);
}

static fastcgi_connection *fastcgi_connecion_create(fastcgi_server *fsrv, gint fd, guint id) {
	fastcgi_connection *fcon = g_slice_new0(fastcgi_connection);

	fcon->fsrv = fsrv;
	fcon->fcon_id = id;

	fcon->buffer = g_byte_array_sized_new(0);
	fcon->parambuf = g_byte_array_sized_new(0);
	fcon->environ = g_hash_table_new_full((GHashFunc) g_string_hash, (GEqualFunc) g_string_equal, _g_string_destroy, _g_string_destroy);

	fcon->fd = fd;
	fd_init(fcon->fd);
	ev_io_init(&fcon->fd_watcher, fastcgi_connection_fd_cb, fcon->fd, EV_READ);
	fcon->fd_watcher.data = fcon;
	ev_io_start(fcon->fsrv->loop, &fcon->fd_watcher);

	return fcon;
}

static void fastcgi_connection_free(fastcgi_connection *fcon) {
	fcon->fsrv->callbacks->cb_reset_connection(fcon);

	if (fcon->fd != -1) {
		ev_io_stop(fcon->fsrv->loop, &fcon->fd_watcher);
		close(fcon->fd);
		fcon->fd = -1;
	}

	fastcgi_queue_clear(&fcon->write_queue);
	g_hash_table_destroy(fcon->environ);
	g_byte_array_free(fcon->buffer, TRUE);
	g_byte_array_free(fcon->parambuf, TRUE);

	g_slice_free(fastcgi_connection, fcon);
}

void fastcgi_connection_close(fastcgi_connection *fcon) {
	fcon->closing = TRUE;
	if (fcon->fd != -1) {
		ev_io_stop(fcon->fsrv->loop, &fcon->fd_watcher);
		close(fcon->fd);
		fcon->fd = -1;
	}

	fastcgi_queue_clear(&fcon->write_queue);

	g_byte_array_set_size(fcon->buffer, 0);
	g_byte_array_set_size(fcon->parambuf, 0);
	g_hash_table_remove_all(fcon->environ);

	ev_prepare_start(fcon->fsrv->loop, &fcon->fsrv->closing_watcher);
}

static void fastcgi_server_fd_cb(struct ev_loop *loop, ev_io *w, int revents) {
	fastcgi_server *fsrv = (fastcgi_server*) w->data;
	fastcgi_connection *fcon;
	void (*cb_new_connection)(fastcgi_connection *fcon) = fsrv->callbacks->cb_new_connection;

	g_assert(revents & EV_READ);

	for (;;) {
		gint fd = accept(fsrv->fd, NULL, NULL);
		if (-1 == fd) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
			case EINTR:
				/* we were stopped _before_ we had a connection */
			case ECONNABORTED: /* this is a FreeBSD thingy */
				/* we were stopped _after_ we had a connection */
				return;
			case EMFILE:
				if (0 == fsrv->max_connections) {
					fsrv->max_connections = fsrv->connections->len / 2;
				} else {
					fsrv->max_connections = fsrv->max_connections / 2;
				}
				ERROR("dropped connection limit to %u as we got EMFILE\n", fsrv->max_connections);
				ev_io_rem_events(loop, w, EV_READ);
				return;
			default:
				ERROR("accept failed on fd=%d with error: %s\nshutting down\n", fsrv->fd, g_strerror(errno));
				fastcgi_server_stop(fsrv);
				return;
			}
		}

		fcon = fastcgi_connecion_create(fsrv, fd, fsrv->connections->len);
		g_ptr_array_add(fsrv->connections, fcon);
		if (cb_new_connection) {
			cb_new_connection(fcon);
		}

		if (fsrv->connections->len >= fsrv->max_connections) {
			ev_io_rem_events(loop, w, EV_READ);
			return;
		}

		if (fsrv->do_shutdown) return;
	}
}

static void fastcgi_cleanup_connections(fastcgi_server *fsrv) {
	guint i;

	for (i = 0; i < fsrv->connections->len; ) {
		fastcgi_connection *fcon = g_ptr_array_index(fsrv->connections, i);
		if (fcon->closing) {
			fastcgi_connection *t_fcon;
			guint l = fsrv->connections->len-1;
			t_fcon = g_ptr_array_index(fsrv->connections, i) = g_ptr_array_index(fsrv->connections, l);
			g_ptr_array_set_size(fsrv->connections, l);
			t_fcon->fcon_id = i;
			fastcgi_connection_free(fcon);
		} else {
			i++;
		}
	}
}

static void fastcgi_closing_cb(struct ev_loop *loop, ev_prepare *w, int revents) {
	UNUSED(revents);
	ev_prepare_stop(loop, w);
	fastcgi_cleanup_connections((fastcgi_server*) w->data);
}

fastcgi_server *fastcgi_server_create(struct ev_loop *loop, gint socketfd, const fastcgi_callbacks *callbacks, guint max_connections) {
	fastcgi_server *fsrv = g_slice_new0(fastcgi_server);

	fsrv->callbacks = callbacks;

	fsrv->max_connections = max_connections;

	fsrv->connections = g_ptr_array_sized_new(fsrv->max_connections);

	fsrv->loop = loop;
	fsrv->fd = socketfd;
	fd_init(fsrv->fd);
	ev_io_init(&fsrv->fd_watcher, fastcgi_server_fd_cb, fsrv->fd, EV_READ);
	fsrv->fd_watcher.data = fsrv;
	ev_io_start(fsrv->loop, &fsrv->fd_watcher);

	ev_prepare_init(&fsrv->closing_watcher, fastcgi_closing_cb);
	fsrv->closing_watcher.data = fsrv;

	return fsrv;
}

void fastcgi_server_stop(fastcgi_server *fsrv) {
	if (fsrv->do_shutdown) return;
	fsrv->do_shutdown = TRUE;

	ev_io_stop(fsrv->loop, &fsrv->fd_watcher);
	close(fsrv->fd);
	fsrv->fd = -1;
}

void fastcgi_server_free(fastcgi_server *fsrv) {
	guint i;
	void (*cb_request_aborted)(fastcgi_connection *fcon) = fsrv->callbacks->cb_request_aborted;
	if (!fsrv->do_shutdown) fastcgi_server_stop(fsrv);
	ev_prepare_stop(fsrv->loop, &fsrv->closing_watcher);

	for (i = 0; i < fsrv->connections->len; i++) {
		fastcgi_connection *fcon = g_ptr_array_index(fsrv->connections, i);
		cb_request_aborted(fcon);
		fcon->closing = TRUE;
	}
	fastcgi_cleanup_connections(fsrv);
	g_ptr_array_free(fsrv->connections, TRUE);

	g_slice_free(fastcgi_server, fsrv);
}

void fastcgi_end_request(fastcgi_connection *fcon, gint32 appStatus, enum FCGI_ProtocolStatus status) {
	gboolean had_data = (fcon->write_queue.length > 0);

	if (0 == fcon->requestID) return;
	stream_send_end_request(&fcon->write_queue, fcon->requestID, appStatus, status);
	fcon->requestID = 0;
	if (!had_data) write_queue(fcon);
}

void fastcgi_suspend_read(fastcgi_connection *fcon) {
	fcon->read_suspended = TRUE;
	ev_io_rem_events(fcon->fsrv->loop, &fcon->fd_watcher, EV_READ);
}

void fastcgi_resume_read(fastcgi_connection *fcon) {
	fcon->read_suspended = FALSE;
	ev_io_add_events(fcon->fsrv->loop, &fcon->fd_watcher, EV_READ);
}

void fastcgi_send_out(fastcgi_connection *fcon, GString *data) {
	gboolean had_data = (fcon->write_queue.length > 0);
	if (!data) {
		stream_send_fcgi_record(&fcon->write_queue, FCGI_STDOUT, fcon->requestID, 0);
	} else {
		stream_send_string(&fcon->write_queue, FCGI_STDOUT, fcon->requestID, data);
	}
	if (!had_data) write_queue(fcon);
}

void fastcgi_send_err(fastcgi_connection *fcon, GString *data) {
	gboolean had_data = (fcon->write_queue.length > 0);
	if (!data) {
		stream_send_fcgi_record(&fcon->write_queue, FCGI_STDERR, fcon->requestID, 0);
	} else {
		stream_send_string(&fcon->write_queue, FCGI_STDERR, fcon->requestID, data);
	}
	if (!had_data) write_queue(fcon);
}

void fastcgi_send_out_bytearray(fastcgi_connection *fcon, GByteArray *data) {
	gboolean had_data = (fcon->write_queue.length > 0);
	if (!data) {
		stream_send_fcgi_record(&fcon->write_queue, FCGI_STDOUT, fcon->requestID, 0);
	} else {
		stream_send_bytearray(&fcon->write_queue, FCGI_STDOUT, fcon->requestID, data);
	}
	if (!had_data) write_queue(fcon);
}

void fastcgi_send_err_bytearray(fastcgi_connection *fcon, GByteArray *data) {
	gboolean had_data = (fcon->write_queue.length > 0);
	if (!data) {
		stream_send_fcgi_record(&fcon->write_queue, FCGI_STDERR, fcon->requestID, 0);
	} else {
		stream_send_bytearray(&fcon->write_queue, FCGI_STDERR, fcon->requestID, data);
	}
	if (!had_data) write_queue(fcon);
}

char** fastcgi_build_env(fastcgi_connection *con) {
	GPtrArray *env = g_ptr_array_new();
	GHashTableIter iter;
	gpointer pkey, pvalue;

	g_hash_table_iter_init(&iter, con->environ);
	while (g_hash_table_iter_next(&iter, &pkey, &pvalue)) {
		GString *key = pkey, *value = pvalue;
		char *s = g_malloc(key->len + value->len + 2);
		memcpy(s, key->str, key->len);
		memcpy(s + key->len + 1, value->str, value->len);
		s[key->len] = '=';
		s[key->len + value->len + 1] = '\0';
		g_ptr_array_add(env, s);
	}
	g_ptr_array_add(env, NULL);

	return (char**) g_ptr_array_free(env, FALSE);
}

const gchar* fastcgi_connection_environ_lookup(fastcgi_connection *fcon, const gchar* key, gsize keylen) {
	GString s = { (gchar*) key, keylen, 0 };
	GString *value = g_hash_table_lookup(fcon->environ, &s);
	return (NULL != value) ? value->str : NULL;
}
