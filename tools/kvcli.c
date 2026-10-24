/*
 * kvcli: an interactive shell for a meeseeks store.
 *
 *   kvcli DIR [command...]               open the store in DIR directly
 *   kvcli -h HOST -p PORT [command...]   talk to a running kvserver
 *
 * Commands are those of the server protocol (GET, SET, DEL, SCAN, STATS,
 * COMPACT, SYNC, PING), plus KEYS [pattern], BACKUP DIR (local stores
 * only), HELP and QUIT. A local store answers through the same protocol
 * handler as the server, so both modes reply alike. Given a command on the
 * command line, kvcli runs just that one; otherwise it reads commands from
 * standard input a plain line at a time, with a prompt if that is a
 * terminal.
 *
 * Exit status: 0, 1 if any command failed, 2 if the store could not be
 * opened or reached.
 */
#include <errno.h>
#include <fnmatch.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kv.h"
#include "server.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* SIGPIPE is ignored instead */
#endif

typedef struct {
    kv_t *db;   /* local mode */
    int fd;     /* remote mode, else -1 */
    char *rbuf; /* received, not yet consumed */
    size_t rlen, rcap;
    sbuf reply; /* the last reply, without its newline */
} cli;

static int buf_put(sbuf *b, const char *p, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d)
            return -1;
        b->data = d;
        b->cap = cap;
    }
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int has_prefix(const char *p, size_t n, const char *pre)
{
    size_t m = strlen(pre);
    return n >= m && memcmp(p, pre, m) == 0;
}

/* Case-insensitive: is the token p[0..n) the word w? */
static int word_is(const char *p, size_t n, const char *w)
{
    if (n != strlen(w))
        return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = p[i];
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        if (ch != w[i])
            return 0;
    }
    return 1;
}

/* ---------- talking to the store ---------- */

static int read_line(cli *c, sbuf *out)
{
    for (;;) {
        char *nl = c->rlen ? memchr(c->rbuf, '\n', c->rlen) : NULL;
        if (nl) {
            size_t n = (size_t)(nl - c->rbuf);
            out->len = 0;
            if (buf_put(out, c->rbuf, n) != 0)
                return -1;
            memmove(c->rbuf, nl + 1, c->rlen - n - 1);
            c->rlen -= n + 1;
            return 0;
        }
        if (c->rlen == c->rcap) {
            size_t cap = c->rcap ? c->rcap * 2 : 64 * 1024;
            char *t = realloc(c->rbuf, cap);
            if (!t)
                return -1;
            c->rbuf = t;
            c->rcap = cap;
        }
        ssize_t r = recv(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        c->rlen += (size_t)r;
    }
}

static int send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* One request, one reply line in c->reply. -1 if the connection is gone. */
static int roundtrip(cli *c, const char *line, size_t len)
{
    c->reply.len = 0;
    if (c->fd < 0) {
        if (len > SERVER_MAX_LINE) /* as the server would say */
            return buf_put(&c->reply, "ERR line too long", 17);
        if (server_handle_line(c->db, line, len, &c->reply) != 0 ||
            buf_put(&c->reply, "", 0) != 0)
            return -1;
        if (c->reply.len && c->reply.data[c->reply.len - 1] == '\n')
            c->reply.data[--c->reply.len] = '\0';
        return 0;
    }
    if (send_all(c->fd, line, len) != 0 || send_all(c->fd, "\n", 1) != 0)
        return -1;
    return read_line(c, &c->reply);
}

static int dial(const char *host, const char *port)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "kvcli: %s port %s: %s\n", host, port, gai_strerror(rc));
        return -1;
    }
    int fd = -1, err = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        err = errno;
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        fprintf(stderr, "kvcli: cannot connect to %s port %s: %s\n", host,
                port, strerror(err));
    return fd;
}

/* ---------- showing replies ---------- */

static void print_quoted(const char *p, size_t n)
{
    putchar('"');
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)p[i];
        if (ch == '"' || ch == '\\')
            printf("\\%c", ch);
        else if (ch == '\n')
            fputs("\\n", stdout);
        else if (ch == '\t')
            fputs("\\t", stdout);
        else if (ch >= 0x20 && ch < 0x7f)
            putchar(ch);
        else
            printf("\\x%02x", ch);
    }
    putchar('"');
}

/* Split "KEYS <cursor> <omitted> <key>..." into its numbers and the start
 * of the key list. 0, or -1 if the reply is not shaped like that. */
static int parse_keys(const char *r, size_t n, unsigned long long *cursor,
                      unsigned long long *omitted, size_t *keys_at)
{
    if (!has_prefix(r, n, "KEYS "))
        return -1;
    char *end;
    *cursor = strtoull(r + 5, &end, 10);
    if (*end != ' ')
        return -1;
    *omitted = strtoull(end + 1, &end, 10);
    if (*end != ' ' && *end != '\0')
        return -1;
    *keys_at = (size_t)(end - r);
    return 0;
}

/* Print a reply for a person. Returns 1 for an error reply, else 0. */
static int show_reply(const char *r, size_t n)
{
    if (has_prefix(r, n, "VALUE ")) {
        print_quoted(r + 6, n - 6);
        putchar('\n');
    } else if (n == 9 && memcmp(r, "NOT_FOUND", 9) == 0) {
        puts("(not found)");
    } else if (has_prefix(r, n, "ERR ")) {
        printf("(error) %.*s\n", (int)(n - 4), r + 4);
        return 1;
    } else if (has_prefix(r, n, "STATS ")) {
        for (size_t i = 6; i < n;) { /* "name=value" pairs, one per line */
            size_t j = i;
            while (j < n && r[j] != ' ')
                j++;
            const char *eq = memchr(r + i, '=', j - i);
            if (eq)
                printf("%-12.*s %.*s\n", (int)(eq - (r + i)), r + i,
                       (int)(r + j - eq - 1), eq + 1);
            i = j + 1;
        }
    } else if (has_prefix(r, n, "KEYS ")) {
        unsigned long long cursor, omitted;
        size_t at, k = 0;
        if (parse_keys(r, n, &cursor, &omitted, &at) != 0) {
            printf("%.*s\n", (int)n, r);
            return 0;
        }
        for (size_t i = at + 1; i < n;) {
            size_t j = i;
            while (j < n && r[j] != ' ')
                j++;
            printf("%zu) ", ++k);
            print_quoted(r + i, j - i);
            putchar('\n');
            i = j + 1;
        }
        if (omitted)
            printf("(%llu more cannot be shown over the line protocol)\n",
                   omitted);
        printf("(next cursor %llu%s)\n", cursor, cursor ? "" : ": done");
    } else {
        printf("%.*s\n", (int)n, r); /* OK, PONG */
    }
    return 0;
}

/* ---------- shell commands ---------- */

static int key_matches(const char *pat, const char *k, size_t n)
{
    if (!pat)
        return 1;
    if (memchr(k, '\0', n))
        return 0;
    char *s = strndup(k, n);
    int m = s && fnmatch(pat, s, 0) == 0;
    free(s);
    return m;
}

/* KEYS [pattern]: SCAN through the whole store. -1 if the connection is
 * gone, else what show_reply returns. */
static int cmd_keys(cli *c, const char *pat)
{
    unsigned long long cursor = 0, omitted = 0, shown = 0;
    do {
        char req[48];
        snprintf(req, sizeof req, "SCAN %llu 1000", cursor);
        if (roundtrip(c, req, strlen(req)) != 0)
            return -1;
        const char *r = c->reply.data;
        size_t n = c->reply.len, at;
        unsigned long long om;
        if (parse_keys(r, n, &cursor, &om, &at) != 0)
            return show_reply(r, n);
        omitted += om;
        for (size_t i = at + 1; i < n;) {
            size_t j = i;
            while (j < n && r[j] != ' ')
                j++;
            if (key_matches(pat, r + i, j - i)) {
                printf("%llu) ", ++shown);
                print_quoted(r + i, j - i);
                putchar('\n');
            }
            i = j + 1;
        }
    } while (cursor != 0);
    printf("(%llu key%s", shown, shown == 1 ? "" : "s");
    if (omitted)
        printf("; %llu more cannot be shown over the line protocol", omitted);
    printf(")\n");
    return 0;
}

static void help(void)
{
    puts("GET key              print the value of key\n"
         "SET key value        store value (the rest of the line) under key\n"
         "DEL key              delete key\n"
         "KEYS [pattern]       list keys, optionally matching a glob pattern\n"
         "SCAN cursor [count]  list a batch of keys, starting at cursor (0)\n"
         "STATS                counters and sizes\n"
         "COMPACT              reclaim the space of old and deleted records\n"
         "SYNC                 make every write so far durable\n"
         "BACKUP dir           copy the store to a new directory (local only)\n"
         "PING                 check that the store answers\n"
         "QUIT                 leave");
}

/*
 * Run one input line. Returns 1 if it failed, 0 if not, -1 if the
 * connection is gone, and 2 to quit.
 */
static int run_line(cli *c, char *line, size_t len)
{
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    size_t cs = i;
    while (i < len && line[i] != ' ' && line[i] != '\t')
        i++;
    const char *cmd = line + cs;
    size_t clen = i - cs;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    const char *arg = line + i; /* the rest of the line, NUL-terminated */

    if (clen == 0)
        return 0;
    if (word_is(cmd, clen, "QUIT") || word_is(cmd, clen, "EXIT"))
        return 2;
    if (word_is(cmd, clen, "HELP")) {
        help();
        return 0;
    }
    if (word_is(cmd, clen, "KEYS")) {
        if (strpbrk(arg, " \t")) {
            puts("(error) KEYS takes at most one pattern");
            return 1;
        }
        return cmd_keys(c, *arg ? arg : NULL);
    }
    if (word_is(cmd, clen, "BACKUP")) {
        if (c->fd >= 0) {
            puts("(error) BACKUP needs the store itself: run kvcli DIR on "
                 "the server's host");
            return 1;
        }
        if (!*arg || strpbrk(arg, " \t")) {
            puts("(error) usage: BACKUP dir");
            return 1;
        }
        int rc = kv_backup(c->db, arg);
        if (rc == KV_OK) {
            puts("OK");
            return 0;
        }
        if (rc == KV_ERR_INVALID)
            printf("(error) %s already exists\n", arg);
        else
            printf("(error) %s\n", kv_strerror(rc));
        return 1;
    }
    if (roundtrip(c, line, len) != 0)
        return -1;
    return show_reply(c->reply.data, c->reply.len);
}

static void usage(void)
{
    fprintf(stderr, "usage: kvcli DIR [command...]\n"
                    "       kvcli [-h host] [-p port] [command...]\n"
                    "Type HELP at the prompt for the commands.\n");
}

int main(int argc, char **argv)
{
    const char *dir = NULL, *host = NULL, *port = NULL;
    int i = 1;
    /* by hand, not getopt: "SET k -1" must not be read as an option */
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = argv[++i];
        else {
            usage();
            return 2;
        }
        i++;
    }
    if (!host && !port) {
        if (i >= argc) {
            usage();
            return 2;
        }
        dir = argv[i++];
    }

    cli c;
    memset(&c, 0, sizeof c);
    c.fd = -1;
    signal(SIGPIPE, SIG_IGN);
    char where[300];
    if (dir) {
        int rc = kv_open(dir, NULL, &c.db);
        if (rc == KV_ERR_LOCKED) {
            fprintf(stderr,
                    "kvcli: %s is already open in another process, perhaps "
                    "a kvserver.\nConnect to that instead: kvcli -h HOST -p "
                    "PORT\n",
                    dir);
            return 2;
        }
        if (rc != KV_OK) {
            fprintf(stderr, "kvcli: cannot open %s: %s\n", dir, kv_strerror(rc));
            return 2;
        }
        snprintf(where, sizeof where, "%s", dir);
    } else {
        host = host ? host : "127.0.0.1";
        port = port ? port : "7379";
        if ((c.fd = dial(host, port)) < 0)
            return 2;
        /* a server at its connection limit answers with an error and hangs up */
        if (roundtrip(&c, "PING", 4) != 0 || strcmp(c.reply.data, "PONG") != 0) {
            fprintf(stderr, "kvcli: %s port %s did not answer like a kvserver%s%s\n",
                    host, port, c.reply.len ? ": " : "",
                    c.reply.len ? c.reply.data : "");
            close(c.fd);
            return 2;
        }
        snprintf(where, sizeof where, "%s:%s", host, port);
    }

    int status = 0, rc = 0;
    if (i < argc) { /* one command from the command line */
        sbuf line = {0};
        for (; i < argc && rc == 0; i++)
            if ((line.len && buf_put(&line, " ", 1) != 0) ||
                buf_put(&line, argv[i], strlen(argv[i])) != 0)
                rc = -1;
        if (rc == 0)
            rc = run_line(&c, line.data, line.len);
        status = rc == 1;
        sbuf_free(&line);
    } else {
        int tty = isatty(STDIN_FILENO);
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        for (;;) {
            if (tty) {
                printf("%s> ", where);
                fflush(stdout);
            }
            if ((n = getline(&line, &cap, stdin)) < 0)
                break;
            rc = run_line(&c, line, (size_t)n);
            fflush(stdout);
            if (rc == 1)
                status = 1;
            if (rc == -1 || rc == 2)
                break;
        }
        if (tty && n < 0)
            putchar('\n');
        free(line);
    }
    if (rc == -1) {
        fprintf(stderr, "kvcli: lost the connection to %s\n", where);
        status = 2;
    }
    if (c.db) {
        int crc = kv_close(c.db);
        if (crc != KV_OK) {
            fprintf(stderr, "kvcli: closing %s: %s\n", where, kv_strerror(crc));
            status = 2;
        }
    }
    if (c.fd >= 0)
        close(c.fd);
    free(c.rbuf);
    sbuf_free(&c.reply);
    return status;
}
