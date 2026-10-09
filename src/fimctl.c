#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_FILE_BYTES (512ULL * 1024ULL * 1024ULL)
#define EVENT_MAX_BYTES (1024U * 1024U)
#define MAX_DEPTH 128
#define EVENT_BUFFER 65536

struct file_row {
    char *path;
    char hash[65];
    uint64_t dev, ino, size, mtime_ns;
    uint32_t mode, uid, gid;
};

struct rows { struct file_row *v; size_t n, cap; };
struct watch_row { int wd; char *path; };
struct watches { struct watch_row *v; size_t n, cap; };

static volatile sig_atomic_t stopping;

static void on_signal(int sig) { (void)sig; stopping = 1; }

static int fail(const char *message) {
    fprintf(stderr, "fimctl: %s: %s\n", message, strerror(errno));
    return 1;
}

static int canonical_root(const char *input, char out[PATH_MAX]) {
    if (!input || input[0] != '/') return -1;
    struct stat input_st;
    if (lstat(input, &input_st) != 0 || !S_ISDIR(input_st.st_mode) || S_ISLNK(input_st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if (!realpath(input, out)) return -1;
    if (!strcmp(out, "/") || !strcmp(out, "/proc") || !strcmp(out, "/sys") ||
        !strcmp(out, "/dev") || !strcmp(out, "/etc") || !strcmp(out, "/usr") ||
        !strcmp(out, "/home")) {
        errno = EPERM;
        return -1;
    }
    struct stat st;
    if (lstat(out, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static uint64_t mtime_ns(const struct stat *st) {
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ULL +
           (uint64_t)st->st_mtim.tv_nsec;
}

static int same_file_state(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

static int hash_fd(int fd, char out[65]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    unsigned char buf[65536], digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    while (ok == 1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;
        ok = EVP_DigestUpdate(ctx, buf, (size_t)n);
    }
    if (ok == 1) ok = EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);
    if (ok != 1 || digest_len != 32) return -1;
    for (unsigned int i = 0; i < digest_len; ++i)
        (void)snprintf(out + i * 2U, 3U, "%02x", digest[i]);
    out[64] = '\0';
    return 0;
}

static int push_row(struct rows *rows, struct file_row *row) {
    if (rows->n == rows->cap) {
        size_t next = rows->cap ? rows->cap * 2U : 128U;
        if (next > 1000000U) { errno = EOVERFLOW; return -1; }
        struct file_row *v = realloc(rows->v, next * sizeof(*v));
        if (!v) return -1;
        rows->v = v; rows->cap = next;
    }
    rows->v[rows->n++] = *row;
    return 0;
}

static void free_rows(struct rows *rows) {
    for (size_t i = 0; i < rows->n; ++i) free(rows->v[i].path);
    free(rows->v); memset(rows, 0, sizeof(*rows));
}

static int scan_dir(int dirfd, const char *relative, dev_t root_dev,
                    struct rows *rows, unsigned int depth) {
    if (depth > MAX_DEPTH) { errno = ELOOP; return -1; }
    int copyfd = dup(dirfd);
    if (copyfd < 0) return -1;
    DIR *dir = fdopendir(copyfd);
    if (!dir) { close(copyfd); return -1; }
    errno = 0;
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            result = -1; break;
        }
        if (st.st_dev != root_dev || S_ISLNK(st.st_mode)) continue;
        size_t plen = strlen(relative), nlen = strlen(entry->d_name);
        char *path = malloc(plen + nlen + 2U);
        if (!path) { result = -1; break; }
        if (plen) (void)snprintf(path, plen + nlen + 2U, "%s/%s", relative, entry->d_name);
        else (void)snprintf(path, plen + nlen + 2U, "%s", entry->d_name);
        if (S_ISDIR(st.st_mode)) {
            int child = openat(dirfd, entry->d_name,
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) { free(path); result = -1; break; }
            if (scan_dir(child, path, root_dev, rows, depth + 1U) != 0) result = -1;
            close(child); free(path);
            if (result) break;
        } else if (S_ISREG(st.st_mode) && st.st_size >= 0 &&
                   (uint64_t)st.st_size <= MAX_FILE_BYTES) {
            int fd = openat(dirfd, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) { free(path); result = -1; break; }
            struct stat before, after;
            char digest[65];
            int stable = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) &&
                         hash_fd(fd, digest) == 0 && fstat(fd, &after) == 0 &&
                         same_file_state(&before, &after);
            close(fd);
            if (!stable) { free(path); errno = EAGAIN; result = -1; break; }
            struct file_row row = {
                .path = path, .dev = (uint64_t)before.st_dev, .ino = (uint64_t)before.st_ino,
                .size = (uint64_t)before.st_size, .mtime_ns = mtime_ns(&before),
                .mode = (uint32_t)before.st_mode, .uid = (uint32_t)before.st_uid,
                .gid = (uint32_t)before.st_gid
            };
            memcpy(row.hash, digest, sizeof(row.hash));
            if (push_row(rows, &row) != 0) { free(path); result = -1; break; }
        } else {
            free(path);
        }
        errno = 0;
    }
    if (!result && errno != 0) result = -1;
    closedir(dir);
    return result;
}

static int row_path_compare(const void *a, const void *b) {
    const struct file_row *left = a, *right = b;
    return strcmp(left->path, right->path);
}

static int scan_root(const char *root, struct rows *rows) {
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    int rc = fstat(fd, &st);
    if (rc == 0) rc = scan_dir(fd, "", st.st_dev, rows, 0);
    close(fd);
    if (rc == 0) {
        qsort(rows->v, rows->n, sizeof(*rows->v), row_path_compare);
    }
    return rc;
}

static int db_open(const char *path, sqlite3 **db) {
    int rc = sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "fimctl: database: %s\n", *db ? sqlite3_errmsg(*db) : "open failed");
        if (*db) sqlite3_close(*db);
        return -1;
    }
    (void)sqlite3_busy_timeout(*db, 5000);
    const char *sql =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS schema_meta(version INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS roots(id INTEGER PRIMARY KEY CHECK(id=1), path TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS files(relative_path TEXT PRIMARY KEY, device INTEGER NOT NULL,"
        " inode INTEGER NOT NULL, mode INTEGER NOT NULL, uid INTEGER NOT NULL, gid INTEGER NOT NULL,"
        " size INTEGER NOT NULL, mtime_ns INTEGER NOT NULL, sha256 TEXT NOT NULL);";
    char *error = NULL;
    rc = sqlite3_exec(*db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "fimctl: database schema: %s\n", error ? error : sqlite3_errmsg(*db));
        sqlite3_free(error); sqlite3_close(*db); *db = NULL; return -1;
    }
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(*db, "SELECT version FROM schema_meta LIMIT 1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        if (sqlite3_exec(*db, "INSERT INTO schema_meta(version) VALUES(1)", NULL, NULL, NULL) != SQLITE_OK)
            return -1;
    } else {
        int version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (version != 1) { fprintf(stderr, "fimctl: unsupported database schema version\n"); return -1; }
    }
    return 0;
}

static int store_rows(sqlite3 *db, const char *root, const struct rows *rows, int create) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
    sqlite3_stmt *stmt = NULL;
    const char *root_sql = create ? "INSERT INTO roots(id,path) VALUES(1,?1)"
                                  : "UPDATE roots SET path=?1 WHERE id=1";
    int rc = sqlite3_prepare_v2(db, root_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) { sqlite3_bind_text(stmt, 1, root, -1, SQLITE_TRANSIENT); rc = sqlite3_step(stmt); }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) goto rollback;
    if (sqlite3_exec(db, "DELETE FROM files", NULL, NULL, NULL) != SQLITE_OK) goto rollback;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO files(relative_path,device,inode,mode,uid,gid,size,mtime_ns,sha256)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto rollback;
    for (size_t i = 0; i < rows->n; ++i) {
        const struct file_row *r = &rows->v[i];
        sqlite3_bind_text(stmt, 1, r->path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)r->dev);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)r->ino);
        sqlite3_bind_int64(stmt, 4, r->mode); sqlite3_bind_int64(stmt, 5, r->uid);
        sqlite3_bind_int64(stmt, 6, r->gid); sqlite3_bind_int64(stmt, 7, (sqlite3_int64)r->size);
        sqlite3_bind_int64(stmt, 8, (sqlite3_int64)r->mtime_ns);
        sqlite3_bind_text(stmt, 9, r->hash, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); goto rollback; }
        sqlite3_reset(stmt); sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);
    return sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
rollback:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

static int get_root(sqlite3 *db, char root[PATH_MAX]) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT path FROM roots WHERE id=1", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(stmt, 0);
        if (s) (void)snprintf(root, PATH_MAX, "%s", (const char *)s);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 0 : -1;
}

static void json_escape(const char *in, char *out, size_t cap);

static void print_json_path(const char *kind, const char *path) {
    char escaped[PATH_MAX * 6U + 1U];
    json_escape(path, escaped, sizeof(escaped));
    printf("{\"kind\":\"%s\",\"path\":\"%s\"}\n", kind, escaped);
}

static int compare_rows(sqlite3 *db, const struct rows *rows, int json) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT relative_path,device,inode,mode,uid,gid,size,mtime_ns,sha256 "
                      "FROM files ORDER BY relative_path";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    size_t i = 0; int changed = 0, rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *oldpath = (const char *)sqlite3_column_text(stmt, 0);
        while (i < rows->n && strcmp(rows->v[i].path, oldpath) < 0) {
            if (json) print_json_path("created", rows->v[i].path);
            else printf("created %s\n", rows->v[i].path);
            ++i; ++changed;
        }
        if (i == rows->n || strcmp(rows->v[i].path, oldpath) != 0) {
            if (json) print_json_path("deleted", oldpath);
            else printf("deleted %s\n", oldpath);
            ++changed; continue;
        }
        const struct file_row *cur = &rows->v[i++];
        const char *hash = (const char *)sqlite3_column_text(stmt, 8);
        uint64_t size = (uint64_t)sqlite3_column_int64(stmt, 6);
        uint64_t mtime = (uint64_t)sqlite3_column_int64(stmt, 7);
        uint32_t mode = (uint32_t)sqlite3_column_int64(stmt, 3);
        if (!hash || strcmp(cur->hash, hash) != 0 || cur->size != size) {
            if (json) print_json_path("modified", cur->path);
            else printf("modified %s\n", cur->path);
            ++changed;
        } else if (cur->mtime_ns != mtime || cur->mode != mode ||
                   cur->uid != (uint32_t)sqlite3_column_int64(stmt, 4) ||
                   cur->gid != (uint32_t)sqlite3_column_int64(stmt, 5)) {
            if (json) print_json_path("metadata_changed", cur->path);
            else printf("metadata_changed %s\n", cur->path);
            ++changed;
        }
    }
    if (rc != SQLITE_DONE) { sqlite3_finalize(stmt); return -1; }
    while (i < rows->n) {
        if (json) print_json_path("created", rows->v[i].path);
        else printf("created %s\n", rows->v[i].path);
        ++changed; ++i;
    }
    sqlite3_finalize(stmt);
    if (!changed) puts(json ? "{\"kind\":\"summary\",\"changed\":0}" : "no changes");
    return changed;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 1U < cap; ++p) {
        if (*p == '"' || *p == '\\') {
            if (n + 2U >= cap) break;
            out[n++] = '\\'; out[n++] = (char)*p;
        } else if (*p < 0x20U) {
            if (n + 6U >= cap) break;
            out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
            out[n++] = hex[*p >> 4U]; out[n++] = hex[*p & 0x0fU];
        } else out[n++] = (char)*p;
    }
    out[n] = '\0';
}

static int watch_add(struct watches *ws, int inofd, const char *path) {
    int wd = inotify_add_watch(inofd, path, IN_CREATE | IN_DELETE | IN_CLOSE_WRITE |
                               IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | IN_IGNORED |
                               IN_ONLYDIR | IN_DONT_FOLLOW);
    if (wd < 0) return -1;
    for (size_t i = 0; i < ws->n; ++i) if (ws->v[i].wd == wd) return 0;
    if (ws->n == ws->cap) {
        size_t cap = ws->cap ? ws->cap * 2U : 64U;
        struct watch_row *v = realloc(ws->v, cap * sizeof(*v));
        if (!v) return -1;
        ws->v = v; ws->cap = cap;
    }
    ws->v[ws->n++] = (struct watch_row){ .wd = wd, .path = strdup(path) };
    return ws->v[ws->n - 1U].path ? 0 : -1;
}

static const char *watch_path(const struct watches *ws, int wd) {
    for (size_t i = 0; i < ws->n; ++i) if (ws->v[i].wd == wd) return ws->v[i].path;
    return NULL;
}

static int add_tree_watches(int inofd, struct watches *ws, const char *path,
                            dev_t root_dev, unsigned depth) {
    if (depth > MAX_DEPTH) { errno = ELOOP; return -1; }
    struct stat self;
    if (lstat(path, &self) != 0) return -1;
    if (!S_ISDIR(self.st_mode) || S_ISLNK(self.st_mode) || self.st_dev != root_dev) return 0;
    if (watch_add(ws, inofd, path) != 0) return -1;
    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(dir)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { rc = -1; errno = ENAMETOOLONG; break; }
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) &&
            st.st_dev == root_dev) {
            if (add_tree_watches(inofd, ws, child, root_dev, depth + 1U) != 0) { rc = -1; break; }
        }
    }
    closedir(dir);
    return rc;
}

static int emit_event(int fd, const char *kind, const char *path, const char *coverage,
                      uint32_t mask, uint32_t cookie) {
    char escaped[PATH_MAX * 6U + 1U]; json_escape(path ? path : "", escaped, sizeof(escaped));
    unsigned char random_id[16];
    if (RAND_bytes(random_id, sizeof(random_id)) != 1) { errno = EIO; return -1; }
    random_id[6] = (unsigned char)((random_id[6] & 0x0fU) | 0x40U);
    random_id[8] = (unsigned char)((random_id[8] & 0x3fU) | 0x80U);
    char event_id[37];
    (void)snprintf(event_id, sizeof(event_id),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        random_id[0], random_id[1], random_id[2], random_id[3], random_id[4], random_id[5],
        random_id[6], random_id[7], random_id[8], random_id[9], random_id[10], random_id[11],
        random_id[12], random_id[13], random_id[14], random_id[15]);
    time_t now = time(NULL);
    struct tm utc;
    char observed[32];
    if (!gmtime_r(&now, &utc) || strftime(observed, sizeof(observed), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        errno = EIO; return -1;
    }
    char line[EVENT_MAX_BYTES];
    int n = snprintf(line, sizeof(line),
        "{\"schema_version\":\"event.v1\",\"event_id\":\"%s\","
        "\"source\":\"fim\",\"host_id\":\"local\",\"observed_at\":\"%s\","
        "\"kind\":\"%s\",\"severity\":\"info\",\"target\":{\"path\":\"%s\"},"
        "\"actor\":{\"pid\":null,\"start_time_ticks\":null,\"attribution\":\"unknown\"},"
        "\"evidence\":{\"event_mask\":%u,\"rename_cookie\":%u},"
        "\"coverage\":{\"state\":\"%s\",\"reason\":null}}\n",
        event_id, observed, kind, escaped,
        mask, cookie, coverage);
    if (n < 0 || (size_t)n >= sizeof(line)) { errno = EOVERFLOW; return -1; }
    size_t off = 0;
    while (off < (size_t)n) {
        ssize_t w = write(fd, line + off, (size_t)n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int cmd_watch(const char *root_arg, const char *events, const char *pidfile) {
    char root[PATH_MAX];
    if (canonical_root(root_arg, root) != 0) return fail("invalid or unsafe root");
    int inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inofd < 0) return fail("inotify_init1");
    struct watches ws = {0};
    int rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat rootst;
    if (rootfd < 0 || fstat(rootfd, &rootst) != 0) {
        if (rootfd >= 0) close(rootfd);
        close(inofd); return fail("open watch root");
    }
    close(rootfd);
    if (add_tree_watches(inofd, &ws, root, rootst.st_dev, 0) != 0) {
        close(inofd); return fail("watch registration failed");
    }
    int logfd = open(events, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (logfd < 0) { close(inofd); return fail("open event log"); }
    int pfd = open(pidfile, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (pfd < 0) { close(logfd); close(inofd); return fail("pidfile exists or inaccessible"); }
    dprintf(pfd, "%ld\n", (long)getpid()); close(pfd);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    unsigned char buf[EVENT_BUFFER];
    puts("watching; Ctrl-C or fimctl stop will stop the monitor");
    while (!stopping) {
        struct pollfd p = { .fd = inofd, .events = POLLIN };
        int ready = poll(&p, 1, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (ready == 0) continue;
        ssize_t n = read(inofd, buf, sizeof(buf));
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n < 0) break;
        for (size_t off = 0; off < (size_t)n;) {
            struct inotify_event *ev = (struct inotify_event *)(void *)(buf + off);
            const char *base = watch_path(&ws, ev->wd);
            if (ev->mask & IN_Q_OVERFLOW) {
                emit_event(logfd, "coverage.degraded", root, "degraded", ev->mask, 0);
                fputs("coverage degraded: inotify queue overflow; run fimctl verify --full\n", stderr);
            } else if (base) {
                char full[PATH_MAX];
                int k = ev->len && ev->name[0]
                    ? snprintf(full, sizeof(full), "%s/%s", base, ev->name)
                    : snprintf(full, sizeof(full), "%s", base);
                if (k > 0 && (size_t)k < sizeof(full)) {
                    const char *kind = (ev->mask & IN_DELETE) ? "file.deleted" :
                        ((ev->mask & IN_MOVED_FROM) ? "file.moved_from" :
                        ((ev->mask & IN_MOVED_TO) ? "file.moved_to" :
                        ((ev->mask & IN_CREATE) ? "file.created" :
                        ((ev->mask & IN_ATTRIB) ? "file.metadata_changed" : "file.modified"))));
                    if (emit_event(logfd, kind, full, "healthy", ev->mask, ev->cookie) != 0) {
                        stopping = 1; break;
                    }
                    if ((ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR))
                        (void)add_tree_watches(inofd, &ws, full, rootst.st_dev, 0);
                }
                if (ev->mask & IN_IGNORED) {
                    for (size_t i = 0; i < ws.n; ++i) if (ws.v[i].wd == ev->wd) ws.v[i].wd = -1;
                }
            }
            off += sizeof(*ev) + ev->len;
        }
    }
    unlink(pidfile);
    for (size_t i = 0; i < ws.n; ++i) free(ws.v[i].path);
    free(ws.v); close(logfd); close(inofd);
    return 0;
}

static int cmd_stop(const char *pidfile) {
    int fd = open(pidfile, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return fail("open pidfile");
    struct stat st;
    char text[64];
    ssize_t n = read(fd, text, sizeof(text) - 1U);
    int valid_file = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                     st.st_uid == geteuid() && (st.st_mode & 0077) == 0 && n > 0;
    close(fd);
    if (!valid_file) { errno = EPERM; return fail("unsafe pidfile"); }
    text[n] = '\0';
    char *end = NULL;
    errno = 0;
    long pid = strtol(text, &end, 10);
    if (errno != 0 || pid <= 1 || pid == (long)getpid() || !end ||
        (*end != '\n' && *end != '\0') || (end[0] == '\n' && end[1] != '\0')) {
        errno = EINVAL; return fail("invalid pidfile");
    }
#ifdef SYS_pidfd_open
    int pidfd = (int)syscall(SYS_pidfd_open, (pid_t)pid, 0U);
    if (pidfd < 0) return fail("open monitored process handle");
#else
    errno = ENOSYS; return fail("safe process handles are unavailable");
#endif
    char proc_path[64], exe_path[PATH_MAX];
    int pn = snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", pid);
    ssize_t exe_n = pn > 0 && (size_t)pn < sizeof(proc_path)
        ? readlink(proc_path, exe_path, sizeof(exe_path) - 1U) : -1;
    if (exe_n < 0) { close(pidfd); return fail("inspect monitored process"); }
    exe_path[exe_n] = '\0';
    const char *base = strrchr(exe_path, '/');
    if (!base || strcmp(base + 1, "fimctl") != 0) {
        close(pidfd); errno = EPERM; return fail("pidfile does not name fimctl");
    }
    int cmdfd = snprintf(proc_path, sizeof(proc_path), "/proc/%ld/cmdline", pid) > 0
        ? open(proc_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
    char cmdline[4096];
    ssize_t cmd_n = cmdfd >= 0 ? read(cmdfd, cmdline, sizeof(cmdline)) : -1;
    if (cmdfd >= 0) close(cmdfd);
    if (cmd_n < 0 || cmd_n >= (ssize_t)sizeof(cmdline)) {
        close(pidfd); errno = EINVAL; return fail("inspect monitored command");
    }
    size_t first_len = strnlen(cmdline, (size_t)cmd_n);
    size_t second_start = first_len + 1U;
    size_t second_len = second_start < (size_t)cmd_n
        ? strnlen(cmdline + second_start, (size_t)cmd_n - second_start) : 0;
    if (second_len != 5U || memcmp(cmdline + second_start, "watch", 5U) != 0) {
        close(pidfd); errno = EPERM; return fail("pidfile process is not fimctl watch");
    }
#ifdef SYS_pidfd_send_signal
    int rc = (int)syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, NULL, 0U);
    close(pidfd);
    if (rc != 0) return fail("send SIGTERM");
    return 0;
#else
    close(pidfd); errno = ENOSYS; return fail("safe process signaling is unavailable");
#endif
}

static void usage(void) {
    puts("fimctl init --root DIR --db FILE\n"
         "fimctl scan --db FILE             (refresh baseline for stored root)\n"
         "fimctl verify --db FILE [--full] [--format json]\n"
         "fimctl status --db FILE\n"
         "fimctl export --db FILE --out FILE\n"
         "fimctl watch --root DIR --events FILE [--pidfile FILE]\n"
         "fimctl stop [--pidfile FILE]");
}

static const char *option(int argc, char **argv, const char *name, const char *fallback) {
    for (int i = 2; i + 1 < argc; ++i) if (!strcmp(argv[i], name)) return argv[i + 1];
    return fallback;
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help")) { usage(); return argc < 2 ? 2 : 0; }
    if (!strcmp(argv[1], "watch")) {
        const char *root = option(argc, argv, "--root", NULL);
        const char *events = option(argc, argv, "--events", NULL);
        const char *pidfile = option(argc, argv, "--pidfile", "./fimd.pid");
        if (!root || !events) { usage(); return 2; }
        return cmd_watch(root, events, pidfile);
    }
    if (!strcmp(argv[1], "stop")) return cmd_stop(option(argc, argv, "--pidfile", "./fimd.pid"));
    const char *dbpath = option(argc, argv, "--db", NULL);
    if (!dbpath) { usage(); return 2; }
    if (!strcmp(argv[1], "init")) {
        const char *root_arg = option(argc, argv, "--root", NULL);
        if (!root_arg) {
            fprintf(stderr, "fimctl: init requires a new database path and --root\n"); return 2;
        }
        char root[PATH_MAX];
        if (canonical_root(root_arg, root) != 0) return fail("invalid or unsafe root");
        struct rows rows = {0};
        if (scan_root(root, &rows) != 0) { free_rows(&rows); return fail("baseline scan failed"); }
        int dbfd = open(dbpath, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (dbfd < 0) { free_rows(&rows); return fail("create new database"); }
        close(dbfd);
        sqlite3 *db = NULL;
        if (db_open(dbpath, &db) != 0) { unlink(dbpath); free_rows(&rows); return 1; }
        int rc = store_rows(db, root, &rows, 1);
        sqlite3_close(db); free_rows(&rows);
        if (rc != 0) { errno = EIO; return fail("commit baseline"); }
        puts("baseline created");
        return 0;
    }
    sqlite3 *db = NULL;
    if (db_open(dbpath, &db) != 0) return 1;
    char root[PATH_MAX];
    if (get_root(db, root) != 0) { fprintf(stderr, "fimctl: no baseline root in database\n"); sqlite3_close(db); return 1; }
    if (!strcmp(argv[1], "status")) {
        sqlite3_stmt *stmt = NULL;
        sqlite3_prepare_v2(db, "SELECT count(*) FROM files", -1, &stmt, NULL);
        sqlite3_step(stmt);
        char escaped[PATH_MAX * 6U + 1U]; json_escape(root, escaped, sizeof(escaped));
        printf("{\"schema_version\":1,\"root\":\"%s\",\"files\":%d,\"coverage\":\"unknown\"}\n",
               escaped, sqlite3_column_int(stmt, 0));
        sqlite3_finalize(stmt); sqlite3_close(db); return 0;
    }
    if (!strcmp(argv[1], "scan") || !strcmp(argv[1], "verify") || !strcmp(argv[1], "export")) {
        if (!strcmp(argv[1], "export")) {
            const char *out = option(argc, argv, "--out", NULL);
            if (!out) { sqlite3_close(db); usage(); return 2; }
            FILE *f = fopen(out, "w");
            if (!f) { sqlite3_close(db); return fail("open export"); }
            sqlite3_stmt *s = NULL;
            sqlite3_prepare_v2(db, "SELECT relative_path,sha256,size,mode,uid,gid,mtime_ns FROM files ORDER BY relative_path", -1, &s, NULL);
            fputs("{\"schema_version\":1,\"files\":[", f);
            int comma = 0;
            while (sqlite3_step(s) == SQLITE_ROW) {
                if (comma++) fputc(',', f);
                char escaped[PATH_MAX * 6U + 1U];
                json_escape((const char *)sqlite3_column_text(s,0), escaped, sizeof(escaped));
                fprintf(f, "{\"path\":\"%s\",\"sha256\":\"%s\",\"size\":%lld,\"mode\":%d,\"uid\":%d,\"gid\":%d,\"mtime_ns\":%lld}",
                    escaped, sqlite3_column_text(s,1),
                    (long long)sqlite3_column_int64(s,2), sqlite3_column_int(s,3),
                    sqlite3_column_int(s,4), sqlite3_column_int(s,5),
                    (long long)sqlite3_column_int64(s,6));
            }
            fputs("]}\n", f); sqlite3_finalize(s); fclose(f); sqlite3_close(db); return 0;
        }
        struct rows rows = {0};
        int rc = scan_root(root, &rows);
        if (rc != 0) { free_rows(&rows); sqlite3_close(db); return fail("scan failed"); }
        if (!strcmp(argv[1], "scan")) {
            rc = store_rows(db, root, &rows, 0);
            puts(rc == 0 ? "baseline refreshed" : "baseline refresh failed");
            free_rows(&rows); sqlite3_close(db); return rc == 0 ? 0 : 1;
        }
        int changed = compare_rows(db, &rows, !strcmp(option(argc, argv, "--format", ""), "json"));
        free_rows(&rows); sqlite3_close(db);
        return changed < 0 ? 1 : changed ? 2 : 0;
    }
    sqlite3_close(db); usage(); return 2;
}
