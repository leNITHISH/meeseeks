#ifndef KV_SERVER_H
#define KV_SERVER_H

#include <stddef.h>

#include "kv.h"

/*
 * Line-based text protocol, one request per line ("\n" or "\r\n") and one
 * response line per request:
 *
 *   SET <key> <value>     -> OK
 *   GET <key>             -> VALUE <value>  |  NOT_FOUND
 *   DEL <key>             -> OK             |  NOT_FOUND
 *   SCAN <cursor> [count] -> KEYS <next-cursor> <omitted> <key>...
 *   STATS                 -> STATS keys=<n> segments=<n> ...
 *   COMPACT, SYNC         -> OK
 *   PING                  -> PONG
 *   anything malformed    -> ERR <reason>
 *
 * Keys are a single token without spaces or tabs. The value is the rest of
 * the line after the single space that follows the key, so it may contain
 * spaces and may be empty. Neither may contain a carriage return, which
 * would be indistinguishable from a CRLF line ending. Commands are
 * case-insensitive. Requests may be pipelined; responses come back in
 * order.
 *
 * SCAN lists up to count keys (default SERVER_SCAN_COUNT, fewer once the
 * reply passes SERVER_SCAN_BYTES), starting at cursor, 0 for the first call.
 * Pass the next cursor to the next SCAN; it is 0 once every key has been
 * listed. Keys that exist for the whole scan are listed exactly once.
 * <omitted> counts keys in this batch that cannot be written as a token
 * (empty, or holding whitespace or a line break), which only the library
 * can store.
 */

#define SERVER_MAX_LINE (1u << 20)
#define SERVER_DEFAULT_MAX_CONNS 1024
#define SERVER_FLUSH_BYTES (1u << 20) /* most reply bytes held per client */
#define SERVER_DEFAULT_IDLE_MS 300000u
#define SERVER_SCAN_COUNT 100
#define SERVER_SCAN_BYTES (64u * 1024u) /* a SCAN reply stops growing here */

typedef struct {
    char *data;
    size_t len, cap;
} sbuf;

void sbuf_free(sbuf *b);

/* Execute one request line (without its newline) and append the response,
 * newline included, to out. Returns 0, or -1 if out could not grow. */
int server_handle_line(kv_t *db, const char *line, size_t len, sbuf *out);

/* Most clients served at once (each gets a thread); further connections
 * are answered "ERR too many connections" and closed. */
void server_set_max_conns(size_t n);

/* Close a client after it has sent nothing, or not taken its replies, for
 * this long. 0 disables the timeout. */
void server_set_idle_timeout(unsigned ms);

/* Bind and listen on host:port (port 0 picks a free port). */
int server_listen(const char *host, int port, int *fd_out, int *port_out);

/* Accept clients until server_stop() is called on listen_fd, one thread per
 * connection. Returns after every connection thread has exited, so the
 * caller may close the store afterwards. */
int server_serve(kv_t *db, int listen_fd);

/* Make server_serve() return. Async-signal-safe, and callable from any
 * thread. */
void server_stop(int listen_fd);

#endif
