#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* not on every platform; SIGPIPE is ignored instead */
#endif

/* ---------- output buffer ---------- */

void sbuf_free(sbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int sb_append(sbuf *b, const char *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n)
            cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d)
            return -1;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static int sb_puts(sbuf *b, const char *s)
{
    return sb_append(b, s, strlen(s));
}

/* ---------- protocol ---------- */

static int is_sep(char c)
{
    return c == ' ' || c == '\t';
}

static int cmd_is(const char *p, size_t n, const char *name)
{
    if (n != strlen(name))
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c != name[i])
            return 0;
    }
    return 1;
}

static int reply_err(sbuf *out, const char *fmt, const char *arg)
{
    char msg[160];
    snprintf(msg, sizeof msg, fmt, arg);
    return sb_puts(out, "ERR ") || sb_puts(out, msg) || sb_puts(out, "\n") ? -1
                                                                          : 0;
}

int server_handle_line(kv_t *db, const char *line, size_t len, sbuf *out)
{
    if (len && line[len - 1] == '\r')
        len--;
    size_t i = 0;
    while (i < len && is_sep(line[i]))
        i++;
    size_t cmd_start = i;
    while (i < len && !is_sep(line[i]))
        i++;
    const char *cmd = line + cmd_start;
    size_t cmd_len = i - cmd_start;
    if (cmd_len == 0)
        return reply_err(out, "%s", "empty command");

    const char *name;
    if (cmd_is(cmd, cmd_len, "SET"))
        name = "SET";
    else if (cmd_is(cmd, cmd_len, "GET"))
        name = "GET";
    else if (cmd_is(cmd, cmd_len, "DEL"))
        name = "DEL";
    else {
        char shown[40];
        snprintf(shown, sizeof shown, "%.*s", cmd_len > 32 ? 32 : (int)cmd_len,
                 cmd);
        return reply_err(out, "unknown command '%s'", shown);
    }

    while (i < len && is_sep(line[i]))
        i++;
    size_t key_start = i;
    while (i < len && !is_sep(line[i]))
        i++;
    const char *key = line + key_start;
    size_t klen = i - key_start;
    if (klen == 0)
        return reply_err(out, "wrong number of arguments for '%s'", name);
    if (klen > KV_MAX_KEY_SIZE)
        return reply_err(out, "%s", "key too long");
    /* A carriage return could never be sent back unambiguously: at the end
     * of a GET line it reads as the CR of a CRLF line ending. So keys and
     * values that contain one are refused, not stored and then lost. */
    if (memchr(key, '\r', klen))
        return reply_err(out, "%s", "key contains a line break");

    int rc;
    if (name[0] == 'S') {
        if (i == len) /* no separator after the key, so no value */
            return reply_err(out, "wrong number of arguments for '%s'", name);
        const char *val = line + i + 1;
        if (memchr(val, '\r', len - i - 1))
            return reply_err(out, "%s", "value contains a line break");
        rc = kv_put(db, key, klen, val, len - i - 1);
        if (rc == KV_OK)
            return sb_puts(out, "OK\n");
        return reply_err(out, "%s", kv_strerror(rc));
    }

    while (i < len && is_sep(line[i]))
        i++;
    if (i != len)
        return reply_err(out, "wrong number of arguments for '%s'", name);

    if (name[0] == 'D') {
        rc = kv_delete(db, key, klen);
        if (rc == KV_OK)
            return sb_puts(out, "OK\n");
        if (rc == KV_NOT_FOUND)
            return sb_puts(out, "NOT_FOUND\n");
        return reply_err(out, "%s", kv_strerror(rc));
    }

    void *val;
    size_t vlen;
    rc = kv_get(db, key, klen, &val, &vlen);
    if (rc == KV_NOT_FOUND)
        return sb_puts(out, "NOT_FOUND\n");
    if (rc != KV_OK)
        return reply_err(out, "%s", kv_strerror(rc));
    int r;
    if (memchr(val, '\n', vlen) || memchr(val, '\r', vlen))
        r = reply_err(out, "%s", "value contains a line break");
    else
        r = sb_puts(out, "VALUE ") || sb_append(out, val, vlen) ||
                    sb_puts(out, "\n")
                ? -1
                : 0;
    free(val);
    return r;
}

/* ---------- networking ---------- */

/* Set from server_stop(), which may run in a signal handler or another thread.
 * A lock-free atomic is safe in both places; volatile sig_atomic_t only covers
 * the signal-handler case and is a data race between threads. */
static atomic_int g_stopping;

/*
 * Self-pipe: server_serve() polls it next to the listening socket, and
 * server_stop() writes a byte to it. Shutting the listening socket down
 * instead only wakes accept() on Linux; on macOS and the BSDs it fails with
 * ENOTCONN and accept() keeps waiting. write() is async-signal-safe.
 */
static int g_wake[2] = {-1, -1};

static int set_flags(int fd, int add, int remove)
{
    int fl = fcntl(fd, F_GETFL);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, (fl | add) & ~remove);
}

/* Open client sockets, so shutdown can kick blocked readers. */
static pthread_mutex_t g_conn_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_conn_cv = PTHREAD_COND_INITIALIZER;
static int *g_conns;
static size_t g_nconns, g_conns_cap;
static size_t g_max_conns = SERVER_DEFAULT_MAX_CONNS;
static unsigned g_idle_ms = SERVER_DEFAULT_IDLE_MS;

void server_set_max_conns(size_t n)
{
    pthread_mutex_lock(&g_conn_mu);
    g_max_conns = n ? n : 1;
    pthread_mutex_unlock(&g_conn_mu);
}

void server_set_idle_timeout(unsigned ms)
{
    pthread_mutex_lock(&g_conn_mu);
    g_idle_ms = ms;
    pthread_mutex_unlock(&g_conn_mu);
}

typedef struct {
    kv_t *db;
    int fd;
} conn;

static int send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void conn_unregister(int fd)
{
    pthread_mutex_lock(&g_conn_mu);
    for (size_t i = 0; i < g_nconns; i++)
        if (g_conns[i] == fd) {
            g_conns[i] = g_conns[--g_nconns];
            break;
        }
    close(fd); /* under the lock: the fd number cannot be reused meanwhile */
    pthread_cond_broadcast(&g_conn_cv);
    pthread_mutex_unlock(&g_conn_mu);
}

static void *conn_main(void *arg)
{
    conn c = *(conn *)arg;
    free(arg);
    size_t cap = 64 * 1024, len = 0;
    char *buf = malloc(cap);
    sbuf out = {0};
    int discarding = 0; /* inside an over-long line, skipping to its end */

    while (buf) {
        if (len == cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb)
                break;
            buf = nb;
            cap = ncap;
        }
        ssize_t n = recv(c.fd, buf + len, cap - len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        len += (size_t)n;

        size_t start = 0;
        char *nl;
        while ((nl = memchr(buf + start, '\n', len - start)) != NULL) {
            size_t llen = (size_t)(nl - (buf + start));
            if (discarding) {
                discarding = 0; /* the over-long line ends here */
            } else if (llen > SERVER_MAX_LINE) {
                if (sb_puts(&out, "ERR line too long\n") != 0)
                    goto done;
            } else if (server_handle_line(c.db, buf + start, llen, &out) != 0) {
                goto done;
            }
            start += llen + 1;
            /* Send as we go: a single read can hold thousands of pipelined
             * GETs of a large value, and their replies must not pile up in
             * memory. A client that does not read then stalls on TCP
             * backpressure instead. */
            if (out.len >= SERVER_FLUSH_BYTES) {
                if (send_all(c.fd, out.data, out.len) != 0)
                    goto done;
                out.len = 0;
            }
        }
        memmove(buf, buf + start, len - start);
        len -= start;
        if (len > SERVER_MAX_LINE) {
            if (!discarding && sb_puts(&out, "ERR line too long\n") != 0)
                break;
            discarding = 1;
            len = 0;
        }
        if (out.len) {
            if (send_all(c.fd, out.data, out.len) != 0)
                break;
            out.len = 0;
        }
    }
done:
    free(buf);
    sbuf_free(&out);
    conn_unregister(c.fd);
    return NULL;
}

int server_listen(const char *host, int port, int *fd_out, int *port_out)
{
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 128) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;
    if (port_out) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) {
            close(fd);
            return -1;
        }
        *port_out = ss.ss_family == AF_INET6
                        ? ntohs(((struct sockaddr_in6 *)&ss)->sin6_port)
                        : ntohs(((struct sockaddr_in *)&ss)->sin_port);
    }
    if (g_wake[0] < 0) {
        if (pipe(g_wake) != 0) {
            close(fd);
            return -1;
        }
        for (int i = 0; i < 2; i++) {
            set_flags(g_wake[i], O_NONBLOCK, 0);
            fcntl(g_wake[i], F_SETFD, FD_CLOEXEC);
        }
    }
    char junk[64]; /* forget wake-ups from an earlier server_stop() */
    while (read(g_wake[0], junk, sizeof junk) > 0)
        ;
    /* Non-blocking, so a connection that vanishes between poll() and
     * accept() cannot leave us stuck in accept(). */
    set_flags(fd, O_NONBLOCK, 0);
    atomic_store(&g_stopping, 0);
    *fd_out = fd;
    return 0;
}

void server_stop(int listen_fd)
{
    (void)listen_fd;
    atomic_store(&g_stopping, 1);
    if (g_wake[1] >= 0) {
        char c = 0;
        ssize_t unused = write(g_wake[1], &c, 1);
        (void)unused;
    }
}

int server_serve(kv_t *db, int listen_fd)
{
    int rc = 0;
    while (!atomic_load(&g_stopping)) {
        struct pollfd pfd[2] = {{.fd = listen_fd, .events = POLLIN},
                                {.fd = g_wake[0], .events = POLLIN}};
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            rc = -1;
            break;
        }
        if (pfd[1].revents || atomic_load(&g_stopping))
            break;
        if (!pfd[0].revents)
            continue;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN ||
                errno == EWOULDBLOCK)
                continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
                errno == ENOMEM) {
                struct timespec ts = {0, 10 * 1000 * 1000};
                nanosleep(&ts, NULL); /* back off until fds free up */
                continue;
            }
            rc = -1;
            break;
        }
        /* macOS and the BSDs copy O_NONBLOCK from the listening socket */
        set_flags(fd, 0, O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        /* A client that sends nothing, or never reads its replies, would
         * hold its slot and thread forever: time out both directions.
         * recv/send then fail with EAGAIN and the connection is closed. */
        pthread_mutex_lock(&g_conn_mu);
        unsigned idle = g_idle_ms;
        pthread_mutex_unlock(&g_conn_mu);
        if (idle) {
            struct timeval tv = {(time_t)(idle / 1000),
                                 (suseconds_t)(idle % 1000) * 1000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }

        conn *c = malloc(sizeof *c);
        pthread_mutex_lock(&g_conn_mu);
        int ok = c != NULL;
        int full = g_nconns >= g_max_conns; /* one thread per client: cap it */
        if (full)
            ok = 0;
        if (ok && g_nconns == g_conns_cap) {
            size_t cap = g_conns_cap ? g_conns_cap * 2 : 16;
            int *t = realloc(g_conns, cap * sizeof *t);
            if (t) {
                g_conns = t;
                g_conns_cap = cap;
            } else {
                ok = 0;
            }
        }
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (ok) {
            c->db = db;
            c->fd = fd;
            g_conns[g_nconns++] = fd;
            if (pthread_create(&th, &attr, conn_main, c) != 0) {
                g_nconns--;
                ok = 0;
            }
        }
        pthread_attr_destroy(&attr);
        pthread_mutex_unlock(&g_conn_mu);
        if (!ok) {
            static const char busy[] = "ERR too many connections\n";
            if (full) /* best effort: the socket is fresh, this fits */
                send_all(fd, busy, sizeof busy - 1);
            free(c);
            close(fd);
        }
    }

    /* Kick every client and wait for its thread, so the store can close. */
    pthread_mutex_lock(&g_conn_mu);
    for (size_t i = 0; i < g_nconns; i++)
        shutdown(g_conns[i], SHUT_RDWR);
    while (g_nconns > 0)
        pthread_cond_wait(&g_conn_cv, &g_conn_mu);
    free(g_conns);
    g_conns = NULL;
    g_conns_cap = 0;
    pthread_mutex_unlock(&g_conn_mu);
    return rc;
}

/* ---------- kvserver binary ---------- */

#ifdef KV_SERVER_MAIN

static int g_listen_fd = -1;

static void on_signal(int sig)
{
    (void)sig;
    if (g_listen_fd >= 0)
        server_stop(g_listen_fd);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-d dir] [-H host] [-p port] [-s always|batch|none] "
            "[-n N] [-c ratio] [-m N] [-t secs]\n"
            "  -d  data directory (default ./data)\n"
            "  -H  listen address (default 127.0.0.1)\n"
            "  -p  port (default 7379)\n"
            "  -s  fsync policy (default batch)\n"
            "  -n  writes per fsync for -s batch (default 100)\n"
            "  -c  compact in the background once this fraction of the log\n"
            "      is garbage (default 0.5; 0 turns it off)\n"
            "  -m  most clients served at once (default %d)\n"
            "  -t  close a client after this many idle seconds (default %d;\n"
            "      0 never)\n",
            prog, SERVER_DEFAULT_MAX_CONNS, SERVER_DEFAULT_IDLE_MS / 1000);
}

int main(int argc, char **argv)
{
    const char *dir = "./data", *host = "127.0.0.1";
    int port = 7379, opt;
    kv_options o;
    kv_default_options(&o);
    o.auto_compact_ratio = 0.5; /* a long-running server must reclaim space */
    while ((opt = getopt(argc, argv, "d:H:p:s:n:c:m:t:h")) != -1) {
        switch (opt) {
        case 'd':
            dir = optarg;
            break;
        case 'H':
            host = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        case 's':
            if (strcmp(optarg, "always") == 0)
                o.sync_policy = KV_SYNC_ALWAYS;
            else if (strcmp(optarg, "batch") == 0)
                o.sync_policy = KV_SYNC_EVERY_N;
            else if (strcmp(optarg, "none") == 0)
                o.sync_policy = KV_SYNC_NONE;
            else {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'n':
            o.sync_every_n = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'c':
            o.auto_compact_ratio = strtod(optarg, NULL);
            break;
        case 'm':
            server_set_max_conns((size_t)strtoul(optarg, NULL, 10));
            break;
        case 't':
            server_set_idle_timeout((unsigned)strtoul(optarg, NULL, 10) * 1000);
            break;
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    kv_t *db;
    int rc = kv_open(dir, &o, &db);
    if (rc != KV_OK) {
        fprintf(stderr, "kvserver: cannot open %s: %s\n", dir, kv_strerror(rc));
        return 1;
    }
    int bound;
    if (server_listen(host, port, &g_listen_fd, &bound) != 0) {
        fprintf(stderr, "kvserver: cannot listen on %s:%d: %s\n", host, port,
                strerror(errno));
        kv_close(db);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: interrupt accept() */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    printf("kvserver: %zu keys in %s, listening on %s:%d\n", kv_count(db), dir,
           host, bound);
    fflush(stdout);
    rc = server_serve(db, g_listen_fd);
    close(g_listen_fd);
    kv_stats st;
    kv_get_stats(db, &st);
    printf("kvserver: %llu puts, %llu deletes, %llu compactions; %llu keys, "
           "%.1f MiB on disk\n",
           (unsigned long long)st.puts, (unsigned long long)st.deletes,
           (unsigned long long)st.compactions, (unsigned long long)st.keys,
           (double)st.disk_bytes / (1024.0 * 1024.0));
    int crc = kv_close(db);
    printf("kvserver: shut down cleanly\n");
    return rc == 0 && crc == KV_OK ? 0 : 1;
}

#endif
