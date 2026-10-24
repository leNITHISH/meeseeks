/*
 * kvcli end to end: runs the kvcli binary built next to this test, on a
 * store directly and against a server running in this process. Both must
 * answer the same script with the same output.
 */
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

#include "server.h"
#include "test_util.h"

static char g_kvcli[512];

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    size_t cap = 4096, len = 0;
    char *b = malloc(cap);
    CHECK(b);
    size_t n;
    while ((n = fread(b + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len - 1 == 0) {
            cap *= 2;
            CHECK((b = realloc(b, cap)) != NULL);
        }
    }
    fclose(f);
    b[len] = '\0';
    return b;
}

/* Run kvcli with args (NULL-terminated), input on its stdin. Returns the
 * exit status, with stdout and stderr in *out and *err (caller frees). */
static int run_cli(const char *dir, const char *const *args, const char *input,
                   char **out, char **err)
{
    char in_p[300], out_p[300], err_p[300];
    snprintf(in_p, sizeof in_p, "%s-cli.in", dir);
    snprintf(out_p, sizeof out_p, "%s-cli.out", dir);
    snprintf(err_p, sizeof err_p, "%s-cli.err", dir);
    FILE *f = fopen(in_p, "wb");
    CHECK(f && fputs(input, f) >= 0 && fclose(f) == 0);

    const char *argv[16] = {g_kvcli};
    int argc = 1;
    while (args[argc - 1]) {
        CHECK(argc < 15);
        argv[argc] = args[argc - 1];
        argc++;
    }
    argv[argc] = NULL;
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int in = open(in_p, O_RDONLY);
        int o = open(out_p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int e = open(err_p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (in < 0 || o < 0 || e < 0 || dup2(in, 0) < 0 || dup2(o, 1) < 0 ||
            dup2(e, 2) < 0)
            _exit(126);
        execv(g_kvcli, (char *const *)argv);
        _exit(127);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    *out = slurp(out_p);
    *err = slurp(err_p);
    unlink(in_p);
    unlink(out_p);
    unlink(err_p);
    CHECK(WIFEXITED(st));
    return WEXITSTATUS(st);
}

static void expect_output(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got\n%s\nwant\n%s\n", what, got, want);
        exit(1);
    }
}

/* The same script and the same answers, local or remote. */
static const char script[] = "SET user:1 alice smith\n"
                             "set user:2 bob\n"
                             "GET user:1\n"
                             "GET nope\n"
                             "DEL user:2\n"
                             "DEL user:2\n"
                             "\n"
                             "KEYS\n"
                             "KEYS user:*\n"
                             "KEYS nothing*\n"
                             "SCAN 0\n"
                             "GET quoted\n"
                             "PING\n"
                             "SYNC\n"
                             "COMPACT\n"
                             "FLY me to the moon\n"
                             "GET user:1\n"
                             "QUIT\n"
                             "GET never-run\n";

static const char answers[] = "OK\n"
                              "OK\n"
                              "\"alice smith\"\n"
                              "(not found)\n"
                              "OK\n"
                              "(not found)\n"
                              "1) \"quoted\"\n"
                              "2) \"user:1\"\n"
                              "(2 keys)\n"
                              "1) \"user:1\"\n"
                              "(1 key)\n"
                              "(0 keys)\n"
                              "1) \"quoted\"\n"
                              "2) \"user:1\"\n"
                              "(next cursor 0: done)\n"
                              "\"tab\\tquote\\\"backslash\\\\\\x01\"\n"
                              "PONG\n"
                              "OK\n"
                              "OK\n"
                              "(error) unknown command 'FLY'\n"
                              "\"alice smith\"\n";

static void seed_store(const char *dir)
{
    kv_t *db;
    CHECK_RC(kv_open(dir, NULL, &db), KV_OK);
    const char v[] = "tab\tquote\"backslash\\\x01";
    CHECK_RC(kv_put(db, "quoted", 6, v, sizeof v - 1), KV_OK);
    CHECK_RC(kv_close(db), KV_OK);
}

/* Keys come back in index order, which is not insertion order: sort the
 * numbered lines of a listing so it can be compared. */
static void sort_listing(char *s)
{
    char *lines[64];
    size_t n = 0;
    for (char *p = s; *p && n < 64;) {
        lines[n++] = p;
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        p = nl + 1;
    }
    /* insertion sort of runs of "N) ..." lines, renumbered in place */
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        while (j < n && strstr(lines[j], ") \"") && lines[j][0] >= '1' &&
               lines[j][0] <= '9')
            j++;
        if (j - i < 2)
            continue;
        char buf[512], *items[64];
        size_t m = 0, off = 0;
        for (size_t k = i; k < j; k++) {
            char *q = strstr(lines[k], ") \"") + 2;
            size_t l = (size_t)(strchr(q, '\n') - q);
            items[m] = buf + off;
            memcpy(buf + off, q, l);
            buf[off + l] = '\0';
            off += l + 1;
            m++;
        }
        for (size_t a = 1; a < m; a++)
            for (size_t b = a; b > 0 && strcmp(items[b - 1], items[b]) > 0; b--) {
                char *t = items[b];
                items[b] = items[b - 1];
                items[b - 1] = t;
            }
        /* same lines, same numbers, so the same length: copy it over */
        char sorted[512];
        size_t len = 0;
        for (size_t k = 0; k < m; k++)
            len += (size_t)snprintf(sorted + len, sizeof sorted - len,
                                    "%zu) %s\n", k + 1, items[k]);
        memcpy(lines[i], sorted, len);
        i = j - 1;
    }
}

static void test_local(const char *dir)
{
    seed_store(dir);
    char *out, *err;
    const char *args[] = {dir, NULL};
    CHECK(run_cli(dir, args, script, &out, &err) == 1); /* FLY failed */
    sort_listing(out);
    expect_output("local", out, answers);
    CHECK(err[0] == '\0');
    free(out);
    free(err);

    /* a command on the command line; SET's value may look like an option */
    const char *set[] = {dir, "SET", "n", "-1", "and", "more", NULL};
    CHECK(run_cli(dir, set, "", &out, &err) == 0);
    expect_output("set", out, "OK\n");
    free(out);
    free(err);
    const char *get[] = {dir, "get", "n", NULL};
    CHECK(run_cli(dir, get, "", &out, &err) == 0);
    expect_output("get", out, "\"-1 and more\"\n");
    free(out);
    free(err);
    const char *bad[] = {dir, "DEL", NULL};
    CHECK(run_cli(dir, bad, "", &out, &err) == 1);
    expect_output("bad", out, "(error) wrong number of arguments for 'DEL'\n");
    free(out);
    free(err);

    /* STATS, one name and value per line */
    CHECK(run_cli(dir, args, "STATS\n", &out, &err) == 0);
    const char stats[] = "keys         3\nsegments     ";
    CHECK(strncmp(out, stats, sizeof stats - 1) == 0);
    CHECK(strstr(out, "\nindex_bytes  "));
    free(out);
    free(err);

    /* BACKUP, and the copy opens like any store */
    char bk[300], cmd[700];
    snprintf(bk, sizeof bk, "%s-bk", dir);
    snprintf(cmd, sizeof cmd, "BACKUP %s\nBACKUP %s\n", bk, bk);
    CHECK(run_cli(dir, args, cmd, &out, &err) == 1);
    char want[700];
    snprintf(want, sizeof want, "OK\n(error) %s already exists\n", bk);
    expect_output("backup", out, want);
    free(out);
    free(err);
    const char *from_bk[] = {bk, "GET", "user:1", NULL};
    CHECK(run_cli(dir, from_bk, "", &out, &err) == 0);
    expect_output("backup get", out, "\"alice smith\"\n");
    free(out);
    free(err);
    tu_rmrf(bk);
}

/* KEYS over more keys than one SCAN batch, and globbing. */
static void test_many_keys(const char *dir)
{
    kv_t *db;
    CHECK_RC(kv_open(dir, NULL, &db), KV_OK);
    char k[32];
    for (int i = 0; i < 2500; i++) {
        snprintf(k, sizeof k, "%s:%d", i % 2 ? "odd" : "even", i);
        CHECK_RC(kv_put(db, k, strlen(k), "v", 1), KV_OK);
    }
    CHECK_RC(kv_put(db, "has space", 9, "v", 1), KV_OK);
    CHECK_RC(kv_close(db), KV_OK);
    char *out, *err;
    const char *all[] = {dir, "KEYS", NULL};
    CHECK(run_cli(dir, all, "", &out, &err) == 0);
    CHECK(strstr(out, "\n2500) \"") &&
          strstr(out, "\n(2500 keys; 1 more cannot be shown over the line "
                      "protocol)\n"));
    free(out);
    free(err);
    const char *odd[] = {dir, "KEYS", "odd:*", NULL};
    CHECK(run_cli(dir, odd, "", &out, &err) == 0);
    CHECK(strstr(out, "\n1250) \"odd:") && strstr(out, "\n(1250 keys; 1 more"));
    CHECK(!strstr(out, "even"));
    free(out);
    free(err);
}

/* A directory some other process has open gets a plain explanation. */
static void test_locked(const char *dir)
{
    kv_t *db;
    CHECK_RC(kv_open(dir, NULL, &db), KV_OK);
    char *out, *err;
    const char *args[] = {dir, "GET", "x", NULL};
    CHECK(run_cli(dir, args, "", &out, &err) == 2);
    CHECK(strstr(err, "is already open in another process") &&
          strstr(err, "kvcli -h HOST -p PORT"));
    free(out);
    free(err);
    CHECK_RC(kv_close(db), KV_OK);
}

typedef struct {
    kv_t *db;
    int fd;
} srv;

static void *serve(void *arg)
{
    srv *s = arg;
    server_serve(s->db, s->fd);
    return NULL;
}

static void test_remote(const char *dir)
{
    seed_store(dir);
    srv s;
    CHECK_RC(kv_open(dir, NULL, &s.db), KV_OK);
    int port;
    CHECK(server_listen("127.0.0.1", 0, &s.fd, &port) == 0);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, serve, &s) == 0);
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);

    char *out, *err;
    const char *args[] = {"-h", "127.0.0.1", "-p", ps, NULL};
    CHECK(run_cli(dir, args, script, &out, &err) == 1);
    sort_listing(out);
    expect_output("remote", out, answers);
    CHECK(err[0] == '\0');
    free(out);
    free(err);

    const char *bk[] = {"-p", ps, "BACKUP", "/tmp/x", NULL}; /* -h defaults */
    CHECK(run_cli(dir, bk, "", &out, &err) == 1);
    const char refused[] = "(error) BACKUP needs the store itself";
    CHECK(strncmp(out, refused, sizeof refused - 1) == 0);
    free(out);
    free(err);

    server_stop(s.fd);
    pthread_join(th, NULL);
    close(s.fd);
    CHECK_RC(kv_close(s.db), KV_OK);

    /* nobody listening any more */
    CHECK(run_cli(dir, args, "PING\n", &out, &err) == 2);
    CHECK(strstr(err, "cannot connect to 127.0.0.1 port"));
    free(out);
    free(err);
}

int main(int argc, char **argv)
{
    (void)argc;
    /* kvcli lives next to this test */
    snprintf(g_kvcli, sizeof g_kvcli, "%s", argv[0]);
    char *slash = strrchr(g_kvcli, '/');
    snprintf(slash ? slash + 1 : g_kvcli,
             sizeof g_kvcli - (size_t)(slash ? slash + 1 - g_kvcli : 0), "kvcli");
    CHECK(access(g_kvcli, X_OK) == 0);
    signal(SIGPIPE, SIG_IGN);
    static const test_case tests[] = {
        {"local store", test_local},
        {"KEYS over many batches", test_many_keys},
        {"directory open elsewhere", test_locked},
        {"remote server, same answers", test_remote},
    };
    return tu_run("test_cli", tests, sizeof tests / sizeof *tests);
}
