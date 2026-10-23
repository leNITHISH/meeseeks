/*
 * Load generator for kvserver.
 *
 *   loadgen [-H host] [-p port] [-c connections] [-d depth] [-t seconds]
 *           [-r read%] [-k keys] [-v value bytes]
 *
 * Each connection sends `depth` pipelined requests, reads all the replies,
 * and repeats until the time is up. Keys are preloaded first so every GET
 * hits. Prints requests per second and the latency of a whole round trip
 * of `depth` requests.
 */
#define _POSIX_C_SOURCE 200809L

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char *host = "127.0.0.1";
static const char *port = "7379";
static int conns = 8, depth = 16, read_pct = 90, nkeys = 100000, vlen = 100;
static double secs = 5;
static atomic_int stop;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int dial(void)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

static int send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Read until `lines` newlines arrived; checks each reply's first byte. */
static int read_replies(int fd, char *buf, size_t cap, int lines)
{
    int seen = 0, at_start = 1;
    while (seen < lines) {
        ssize_t r = recv(fd, buf, cap, 0);
        if (r <= 0)
            return -1;
        for (ssize_t i = 0; i < r; i++) {
            if (at_start && buf[i] != 'O' && buf[i] != 'V')
                return -1; /* ERR or NOT_FOUND: something is wrong */
            at_start = buf[i] == '\n';
            seen += at_start;
        }
    }
    return 0;
}

typedef struct {
    int id;
    uint64_t requests;
    uint64_t *lat;
    size_t nlat, cap;
    int failed;
} worker;

static void *run(void *arg)
{
    worker *w = arg;
    int fd = dial();
    if (fd < 0) {
        w->failed = 1;
        return NULL;
    }
    size_t cap = (size_t)depth * ((size_t)vlen + 64);
    char *out = malloc(cap), *in = malloc(1 << 16);
    char *val = malloc((size_t)vlen + 1);
    memset(val, 'v', (size_t)vlen);
    val[vlen] = '\0';
    uint64_t seed = 0x9E3779B97F4A7C15ull * (uint64_t)(w->id + 1);
    while (!atomic_load(&stop)) {
        size_t len = 0;
        for (int i = 0; i < depth; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            int key = (int)(seed % (uint64_t)nkeys);
            if ((int)(seed >> 40) % 100 < read_pct)
                len += (size_t)snprintf(out + len, cap - len, "GET k%d\n", key);
            else
                len += (size_t)snprintf(out + len, cap - len, "SET k%d %s\n",
                                        key, val);
        }
        uint64_t t0 = now_ns();
        if (send_all(fd, out, len) != 0 ||
            read_replies(fd, in, 1 << 16, depth) != 0) {
            w->failed = 1;
            break;
        }
        if (w->nlat == w->cap) {
            w->cap = w->cap ? w->cap * 2 : 1 << 16;
            w->lat = realloc(w->lat, w->cap * sizeof *w->lat);
        }
        w->lat[w->nlat++] = now_ns() - t0;
        w->requests += (uint64_t)depth;
    }
    close(fd);
    free(out);
    free(in);
    free(val);
    return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "H:p:c:d:t:r:k:v:")) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 'p': port = optarg; break;
        case 'c': conns = atoi(optarg); break;
        case 'd': depth = atoi(optarg); break;
        case 't': secs = atof(optarg); break;
        case 'r': read_pct = atoi(optarg); break;
        case 'k': nkeys = atoi(optarg); break;
        case 'v': vlen = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-H host] [-p port] [-c conns] [-d depth] "
                            "[-t secs] [-r read%%] [-k keys] [-v bytes]\n", argv[0]);
            return 2;
        }
    }
    if (conns < 1 || depth < 1 || nkeys < 1 || vlen < 1)
        return 2;

    /* preload every key so reads hit */
    int fd = dial();
    if (fd < 0) {
        fprintf(stderr, "loadgen: cannot connect to %s:%s\n", host, port);
        return 1;
    }
    char *val = malloc((size_t)vlen + 1), line[256], *in = malloc(1 << 16);
    memset(val, 'v', (size_t)vlen);
    val[vlen] = '\0';
    for (int i = 0; i < nkeys; i += 256) {
        int n = 0;
        for (int j = i; j < nkeys && j < i + 256; j++, n++) {
            int l = snprintf(line, sizeof line, "SET k%d ", j);
            if (send_all(fd, line, (size_t)l) || send_all(fd, val, (size_t)vlen) ||
                send_all(fd, "\n", 1))
                return 1;
        }
        if (read_replies(fd, in, 1 << 16, n) != 0) {
            fprintf(stderr, "loadgen: preload failed\n");
            return 1;
        }
    }
    close(fd);

    worker *ws = calloc((size_t)conns, sizeof *ws);
    pthread_t *th = calloc((size_t)conns, sizeof *th);
    uint64_t t0 = now_ns();
    for (int i = 0; i < conns; i++) {
        ws[i].id = i;
        pthread_create(&th[i], NULL, run, &ws[i]);
    }
    struct timespec ts = {(time_t)secs, (long)((secs - (double)(time_t)secs) * 1e9)};
    nanosleep(&ts, NULL);
    atomic_store(&stop, 1);
    uint64_t total = 0;
    size_t nlat = 0;
    for (int i = 0; i < conns; i++) {
        pthread_join(th[i], NULL);
        if (ws[i].failed) {
            fprintf(stderr, "loadgen: connection %d failed\n", i);
            return 1;
        }
        total += ws[i].requests;
        nlat += ws[i].nlat;
    }
    double elapsed = (double)(now_ns() - t0) / 1e9;
    uint64_t *lat = malloc((nlat ? nlat : 1) * sizeof *lat);
    size_t k = 0;
    for (int i = 0; i < conns; i++) {
        memcpy(lat + k, ws[i].lat, ws[i].nlat * sizeof *lat);
        k += ws[i].nlat;
        free(ws[i].lat);
    }
    qsort(lat, nlat, sizeof *lat, cmp_u64);
    size_t i50 = nlat / 2, i99 = nlat * 99 / 100; /* percentile indices */
    printf("%d connections, %d pipelined, %d%% GET, %d B values: %.0f "
           "requests/s; round trip p50 %.0f us, p99 %.0f us\n",
           conns, depth, read_pct, vlen, (double)total / elapsed,
           nlat ? (double)lat[i50] / 1e3 : 0, nlat ? (double)lat[i99] / 1e3 : 0);
    free(lat);
    free(ws);
    free(th);
    free(val);
    free(in);
    return 0;
}
