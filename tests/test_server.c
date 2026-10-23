/* Protocol tests (direct) and end-to-end tests over a real TCP socket. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>

#include "server.h"
#include "test_util.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ---------- protocol, no sockets ---------- */

static void exchange(kv_t *db, const char *req, const char *want)
{
    sbuf out = {0};
    CHECK(server_handle_line(db, req, strlen(req), &out) == 0);
    if (out.len != strlen(want) || memcmp(out.data, want, out.len) != 0) {
        fprintf(stderr, "request '%s': got '%.*s', want '%s'\n", req,
                (int)out.len, out.data, want);
        exit(1);
    }
    sbuf_free(&out);
}

static void test_protocol(const char *dir)
{
    kv_t *db;
    CHECK_RC(kv_open(dir, NULL, &db), KV_OK);
    exchange(db, "GET a", "NOT_FOUND\n");
    exchange(db, "SET a 1", "OK\n");
    exchange(db, "GET a", "VALUE 1\n");
    exchange(db, "SET a hello  world ", "OK\n"); /* value keeps its spaces */
    exchange(db, "GET a", "VALUE hello  world \n");
    exchange(db, "set b x\r", "OK\n"); /* lowercase and CRLF */
    exchange(db, "get b", "VALUE x\n");
    exchange(db, "SET e ", "OK\n"); /* empty value */
    exchange(db, "GET e", "VALUE \n");
    exchange(db, "  GET   a  ", "VALUE hello  world \n");
    exchange(db, "DEL a", "OK\n");
    exchange(db, "DEL a", "NOT_FOUND\n");
    exchange(db, "GET a", "NOT_FOUND\n");

    exchange(db, "", "ERR empty command\n");
    exchange(db, "   ", "ERR empty command\n");
    exchange(db, "FOO bar", "ERR unknown command 'FOO'\n");
    exchange(db, "SETX a b", "ERR unknown command 'SETX'\n");
    exchange(db, "SET", "ERR wrong number of arguments for 'SET'\n");
    exchange(db, "SET a", "ERR wrong number of arguments for 'SET'\n");
    exchange(db, "GET", "ERR wrong number of arguments for 'GET'\n");
    exchange(db, "GET a b", "ERR wrong number of arguments for 'GET'\n");
    exchange(db, "DEL", "ERR wrong number of arguments for 'DEL'\n");
    exchange(db, "DEL a b", "ERR wrong number of arguments for 'DEL'\n");

    /* a value stored through the library that the line protocol can't carry */
    CHECK_RC(kv_put(db, "nl", 2, "a\nb", 3), KV_OK);
    exchange(db, "GET nl", "ERR value contains a line break\n");

    /* Carriage returns are refused rather than stored: "GET k\r" would read
     * as "GET k" with a CRLF ending (found by fuzz_protocol). */
    exchange(db, "SET k\r v", "ERR key contains a line break\n");
    exchange(db, "SET k v\rw", "ERR value contains a line break\n");
    exchange(db, "SET k v\r\r", "ERR value contains a line break\n");
    exchange(db, "GET k\rx", "ERR key contains a line break\n");
    exchange(db, "GET k", "NOT_FOUND\n");
    exchange(db, "SET k v\r", "OK\n"); /* one trailing CR is the line end */
    exchange(db, "GET k\r", "VALUE v\n");
    CHECK_RC(kv_close(db), KV_OK);
}

/* ---------- over TCP ---------- */

typedef struct {
    kv_t *db;
    int listen_fd;
    int rc;
} server_ctx;

static void *server_thread(void *arg)
{
    server_ctx *s = arg;
    s->rc = server_serve(s->db, s->listen_fd);
    return NULL;
}

typedef struct {
    int fd;
    char buf[4096];
    size_t len;
} client;

static void client_connect(client *c, int port)
{
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(c->fd >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(c->fd, (struct sockaddr *)&a, sizeof a) == 0);
    c->len = 0;
}

static void client_send(client *c, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = send(c->fd, s, n, MSG_NOSIGNAL);
        CHECK(w > 0);
        s += w;
        n -= (size_t)w;
    }
}

/* Read one response line (without the newline) into out. */
static void client_line(client *c, char *out, size_t cap)
{
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->len);
        if (nl) {
            size_t n = (size_t)(nl - c->buf);
            CHECK(n < cap);
            memcpy(out, c->buf, n);
            out[n] = '\0';
            memmove(c->buf, nl + 1, c->len - n - 1);
            c->len -= n + 1;
            return;
        }
        CHECK(c->len < sizeof c->buf);
        ssize_t r = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len, 0);
        CHECK(r > 0);
        c->len += (size_t)r;
    }
}

static void expect_line(client *c, const char *want)
{
    char got[4096];
    client_line(c, got, sizeof got);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "got '%s', want '%s'\n", got, want);
        exit(1);
    }
}

typedef struct {
    int port, id;
} worker_arg;

static void *client_worker(void *arg)
{
    worker_arg *w = arg;
    client c;
    client_connect(&c, w->port);
    char req[128], want[128];
    for (int i = 0; i < 300; i++) {
        snprintf(req, sizeof req, "SET w%d-k%d value %d from %d\n", w->id,
                 i % 50, i, w->id);
        client_send(&c, req, strlen(req));
        expect_line(&c, "OK");
        snprintf(req, sizeof req, "GET w%d-k%d\n", w->id, i % 50);
        client_send(&c, req, strlen(req));
        snprintf(want, sizeof want, "VALUE value %d from %d", i, w->id);
        expect_line(&c, want);
    }
    close(c.fd);
    return NULL;
}

static void test_tcp(const char *dir)
{
    server_ctx s;
    int port;
    CHECK_RC(kv_open(dir, NULL, &s.db), KV_OK);
    CHECK(server_listen("127.0.0.1", 0, &s.listen_fd, &port) == 0);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, server_thread, &s) == 0);

    client c;
    client_connect(&c, port);

    /* pipelined requests in one packet, answered in order */
    const char *batch = "SET greeting hello world\r\nGET greeting\nDEL greeting\n"
                        "GET greeting\nBOGUS\n";
    client_send(&c, batch, strlen(batch));
    expect_line(&c, "OK");
    expect_line(&c, "VALUE hello world");
    expect_line(&c, "OK");
    expect_line(&c, "NOT_FOUND");
    expect_line(&c, "ERR unknown command 'BOGUS'");

    /* one request split across many tiny writes */
    const char *slow = "SET slow drip\n";
    for (size_t i = 0; i < strlen(slow); i++) {
        client_send(&c, slow + i, 1);
        if (i % 4 == 0)
            tu_sleep_us(1000);
    }
    expect_line(&c, "OK");

    /* an over-long line is rejected, and the connection keeps working */
    size_t big = SERVER_MAX_LINE + 4096;
    char *junk = malloc(big);
    memset(junk, 'x', big);
    memcpy(junk, "SET huge ", 9);
    client_send(&c, junk, big);
    free(junk);
    client_send(&c, "\nGET slow\n", 10);
    expect_line(&c, "ERR line too long");
    expect_line(&c, "VALUE drip");

    /* many clients at once */
    enum { NW = 8 };
    pthread_t ws[NW];
    worker_arg wa[NW];
    for (int i = 0; i < NW; i++) {
        wa[i] = (worker_arg){.port = port, .id = i};
        CHECK(pthread_create(&ws[i], NULL, client_worker, &wa[i]) == 0);
    }
    for (int i = 0; i < NW; i++)
        pthread_join(ws[i], NULL);

    /* stopping with a client still connected must not hang */
    server_stop(s.listen_fd);
    pthread_join(th, NULL);
    CHECK(s.rc == 0);
    close(s.listen_fd);
    close(c.fd);

    void *v;
    size_t n;
    CHECK_RC(kv_get(s.db, "w3-k49", 6, &v, &n), KV_OK);
    CHECK(n == strlen("value 299 from 3") && memcmp(v, "value 299 from 3", n) == 0);
    free(v);
    CHECK_RC(kv_close(s.db), KV_OK);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN); /* where MSG_NOSIGNAL is missing */
    static const test_case tests[] = {
        {"protocol", test_protocol},
        {"tcp end-to-end", test_tcp},
    };
    return tu_run("test_server", tests, sizeof tests / sizeof *tests);
}
