#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/eventfd.h>
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
#define MAX_PENDING_RENAMES 256U
#define MAX_PENDING_EVENTS 256U
#define EVENT_DEBOUNCE_MS 100U
#define RENAME_TIMEOUT_SECONDS 2
#define DEFAULT_VERIFY_INTERVAL_SECONDS 3600U
#define MAX_VERIFY_INTERVAL_SECONDS 86400U
#define HASH_WORKER_COUNT 2U
#define MAX_PENDING_HASH_JOBS 1024U

struct file_row {
    char *path;
    char hash[65];
    uint64_t dev, ino, size, mtime_ns;
    uint32_t mode, uid, gid;
};

struct rows { struct file_row *v; size_t n, cap; };
struct watch_row { int wd; char *path; };
struct watches { struct watch_row *v; size_t n, cap; };
struct pending_rename {
    uint32_t cookie;
    char path[PATH_MAX];
    struct timespec observed;
};
struct pending_renames {
    struct pending_rename rows[MAX_PENDING_RENAMES];
    size_t n;
};
struct pending_file_event {
    char path[PATH_MAX];
    uint32_t mask, last_action_mask, count;
    uint64_t first_monotonic_ns, last_monotonic_ns;
    uint64_t first_realtime_ns, last_realtime_ns;
};
struct event_batch {
    struct pending_file_event *rows;
    size_t n;
    unsigned int window_ms;
};
struct hash_job {
    uint64_t sequence;
    struct pending_file_event event;
    char relative[PATH_MAX];
    char coverage[16];
    int scan_rc;
    int error_errno;
    struct file_row current;
    struct hash_job *next;
};
struct hash_pool {
    pthread_t workers[HASH_WORKER_COUNT];
    unsigned int worker_count;
    pthread_mutex_t mutex;
    pthread_cond_t work_ready;
    struct hash_job *work_head, *work_tail, *done_head;
    size_t outstanding;
    uint64_t next_sequence;
    int completion_fd;
    int stopping;
    atomic_int worker_error;
    int consumer_error;
    int mutex_ready;
    int condition_ready;
    int rootfd;
    dev_t root_dev;
};
struct watch_context {
    const char *root;
    int rootfd;
    dev_t root_dev;
    struct rows *snapshot;
    struct hash_pool *hash_pool;
    uint64_t *next_commit_sequence;
};

static volatile sig_atomic_t stopping;

static void on_signal(int sig) { (void)sig; stopping = 1; }

static int emit_event(int fd, const char *kind, const char *path, const char *coverage,
                      uint32_t mask, uint32_t cookie);
static int emit_coverage_event(int fd, const char *path, const char *state,
                               const char *reason, uint32_t mask);
static int rows_differ(const struct file_row *left, const struct file_row *right);

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
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec &&
           a->st_mode == b->st_mode && a->st_uid == b->st_uid && a->st_gid == b->st_gid;
}

static int valid_relative_path(const char *path) {
    if (!path || !path[0] || path[0] == '/') return 0;
    const char *component = path;
    for (const char *cursor = path;; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') continue;
        size_t length = (size_t)(cursor - component);
        if (length == 0U || (length == 1U && component[0] == '.') ||
            (length == 2U && component[0] == '.' && component[1] == '.')) return 0;
        if (*cursor == '\0') break;
        component = cursor + 1;
    }
    return 1;
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
    int saved_error = errno;
    closedir(dir);
    if (result != 0) errno = saved_error ? saved_error : EIO;
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
    int saved_error = rc == 0 ? 0 : errno;
    close(fd);
    if (rc != 0) errno = saved_error ? saved_error : EIO;
    if (rc == 0 && rows->n > 1U) {
        qsort(rows->v, rows->n, sizeof(*rows->v), row_path_compare);
    }
    return rc;
}

/* Returns 0 for a stable regular file, 1 when the path is absent or excluded, -1 on I/O/hash error. */
static int scan_relative_file(int rootfd, dev_t root_dev, const char *relative,
                              struct file_row *row) {
    memset(row, 0, sizeof(*row));
    if (!valid_relative_path(relative)) { errno = EINVAL; return -1; }
    char *path = strdup(relative);
    if (!path) return -1;
    int dirfd = dup(rootfd);
    if (dirfd < 0) { free(path); return -1; }
    char *part = path;
    int result = -1;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash) *slash = '\0';
        if (!part[0] || !strcmp(part, ".") || !strcmp(part, "..")) {
            errno = EINVAL; break;
        }
        if (slash) {
            int nextfd = openat(dirfd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (nextfd < 0) {
                if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) result = 1;
                break;
            }
            close(dirfd); dirfd = nextfd; part = slash + 1;
            continue;
        }
        int fd = openat(dirfd, part, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) result = 1;
            break;
        }
        struct stat before, after;
        if (fstat(fd, &before) != 0) {
            result = -1;
        } else if (!S_ISREG(before.st_mode) || before.st_dev != root_dev) {
            result = 1;
        } else if (before.st_size < 0 || (uint64_t)before.st_size > MAX_FILE_BYTES) {
            errno = EFBIG; result = -1;
        } else {
            char digest[65];
            if (hash_fd(fd, digest) != 0) {
                if (!errno) errno = EIO;
                result = -1;
            } else if (fstat(fd, &after) != 0) {
                result = -1;
            } else if (!same_file_state(&before, &after)) {
                errno = EAGAIN; result = -1;
            } else {
                row->path = strdup(relative);
                if (!row->path) {
                    result = -1;
                } else {
                    row->dev = (uint64_t)before.st_dev; row->ino = (uint64_t)before.st_ino;
                    row->size = (uint64_t)before.st_size; row->mtime_ns = mtime_ns(&before);
                    row->mode = (uint32_t)before.st_mode; row->uid = (uint32_t)before.st_uid;
                    row->gid = (uint32_t)before.st_gid;
                    memcpy(row->hash, digest, sizeof(row->hash));
                    result = 0;
                }
            }
        }
        int saved_error = errno;
        close(fd);
        errno = saved_error;
        break;
    }
    int saved_error = errno;
    close(dirfd); free(path); errno = saved_error;
    if (result != 0) { free(row->path); memset(row, 0, sizeof(*row)); }
    return result;
}

static void free_hash_job(struct hash_job *job) {
    if (!job) return;
    free(job->current.path);
    free(job);
}

static void notify_hash_completion(struct hash_pool *pool) {
    uint64_t one = 1U;
    ssize_t n;
    do { n = write(pool->completion_fd, &one, sizeof(one)); }
    while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(one))
        atomic_store_explicit(&pool->worker_error, n < 0 ? errno : EIO, memory_order_release);
}

static void *hash_worker_main(void *opaque) {
    struct hash_pool *pool = opaque;
    for (;;) {
        if (pthread_mutex_lock(&pool->mutex) != 0) return NULL;
        while (!pool->work_head && !pool->stopping) {
            int rc = pthread_cond_wait(&pool->work_ready, &pool->mutex);
            if (rc != 0) {
                pool->worker_error = rc;
                pool->stopping = 1;
                (void)pthread_cond_broadcast(&pool->work_ready);
                (void)pthread_mutex_unlock(&pool->mutex);
                notify_hash_completion(pool);
                return NULL;
            }
        }
        if (!pool->work_head && pool->stopping) {
            (void)pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        struct hash_job *job = pool->work_head;
        pool->work_head = job->next;
        if (!pool->work_head) pool->work_tail = NULL;
        (void)pthread_mutex_unlock(&pool->mutex);

        job->current = (struct file_row){0};
        job->scan_rc = -1;
        job->error_errno = EIO;
        for (unsigned int attempt = 0U; attempt < 3U; ++attempt) {
            errno = 0;
            job->scan_rc = scan_relative_file(pool->rootfd, pool->root_dev,
                                              job->relative, &job->current);
            job->error_errno = errno ? errno : EIO;
            if (job->scan_rc >= 0 || job->error_errno != EAGAIN) break;
            const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
            (void)nanosleep(&delay, NULL);
        }

        if (pthread_mutex_lock(&pool->mutex) != 0) {
            atomic_store_explicit(&pool->worker_error, EDEADLK, memory_order_release);
            free_hash_job(job);
            notify_hash_completion(pool);
            return NULL;
        }
        struct hash_job **slot = &pool->done_head;
        while (*slot && (*slot)->sequence < job->sequence) slot = &(*slot)->next;
        job->next = *slot;
        *slot = job;
        (void)pthread_mutex_unlock(&pool->mutex);
        notify_hash_completion(pool);
    }
}

static void hash_pool_stop(struct hash_pool *pool) {
    if (pool->mutex_ready) {
        (void)pthread_mutex_lock(&pool->mutex);
        pool->stopping = 1;
        if (pool->condition_ready) (void)pthread_cond_broadcast(&pool->work_ready);
        (void)pthread_mutex_unlock(&pool->mutex);
    }
    for (unsigned int i = 0U; i < pool->worker_count; ++i)
        (void)pthread_join(pool->workers[i], NULL);
    struct hash_job *job = pool->work_head;
    while (job) { struct hash_job *next = job->next; free_hash_job(job); job = next; }
    job = pool->done_head;
    while (job) { struct hash_job *next = job->next; free_hash_job(job); job = next; }
    if (pool->condition_ready) (void)pthread_cond_destroy(&pool->work_ready);
    if (pool->mutex_ready) (void)pthread_mutex_destroy(&pool->mutex);
    if (pool->completion_fd >= 0) close(pool->completion_fd);
    memset(pool, 0, sizeof(*pool));
    pool->completion_fd = -1;
}

static int hash_pool_start(struct hash_pool *pool, int rootfd, dev_t root_dev) {
    memset(pool, 0, sizeof(*pool));
    pool->completion_fd = -1;
    pool->rootfd = rootfd;
    pool->root_dev = root_dev;
    pool->next_sequence = 1U;
    atomic_init(&pool->worker_error, 0);
    pool->completion_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (pool->completion_fd < 0) return -1;
    int rc = pthread_mutex_init(&pool->mutex, NULL);
    if (rc != 0) { errno = rc; hash_pool_stop(pool); return -1; }
    pool->mutex_ready = 1;
    rc = pthread_cond_init(&pool->work_ready, NULL);
    if (rc != 0) { errno = rc; hash_pool_stop(pool); return -1; }
    pool->condition_ready = 1;
    for (unsigned int i = 0U; i < HASH_WORKER_COUNT; ++i) {
        rc = pthread_create(&pool->workers[i], NULL, hash_worker_main, pool);
        if (rc != 0) { errno = rc; hash_pool_stop(pool); return -1; }
        ++pool->worker_count;
    }
    return 0;
}

static size_t row_index(const struct rows *rows, const char *path, int *found) {
    if (!found) { errno = EINVAL; return 0U; }
    *found = 0;
    if (!rows || !path) { errno = EINVAL; return 0U; }
    if (rows->n == 0U) return 0U;
    if (!rows->v) { errno = EIO; return 0U; }
    size_t low = 0U, high = rows->n;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        int cmp = strcmp(rows->v[middle].path, path);
        if (cmp < 0) low = middle + 1U;
        else high = middle;
    }
    *found = low < rows->n && strcmp(rows->v[low].path, path) == 0;
    return low;
}

static int rows_upsert(struct rows *rows, struct file_row *row) {
    if (!rows || !row || !row->path || (rows->n > 0U && !rows->v)) {
        errno = EINVAL; return -1;
    }
    int found = 0;
    size_t index = row_index(rows, row->path, &found);
    if (found && (!rows->v || index >= rows->n)) { errno = EIO; return -1; }
    if (found) {
        free(rows->v[index].path);
        rows->v[index] = *row;
        row->path = NULL;
        return 0;
    }
    if (rows->n >= 1000000U) { errno = EOVERFLOW; return -1; }
    if (rows->n == rows->cap) {
        size_t next = rows->cap ? rows->cap * 2U : 128U;
        if (next > 1000000U) next = 1000000U;
        struct file_row *v = realloc(rows->v, next * sizeof(*v));
        if (!v) return -1;
        rows->v = v; rows->cap = next;
    }
    memmove(&rows->v[index + 1U], &rows->v[index], (rows->n - index) * sizeof(rows->v[0]));
    rows->v[index] = *row; row->path = NULL; ++rows->n;
    return 0;
}

static int rows_remove(struct rows *rows, size_t index) {
    if (!rows || !rows->v || index >= rows->n) { errno = EINVAL; return -1; }
    free(rows->v[index].path);
    if (index + 1U < rows->n)
        memmove(&rows->v[index], &rows->v[index + 1U], (rows->n - index - 1U) * sizeof(rows->v[0]));
    --rows->n;
    return 0;
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
        int length = sqlite3_column_bytes(stmt, 0);
        if (!s || length <= 0 || length >= PATH_MAX) {
            rc = SQLITE_ERROR;
        } else {
            memcpy(root, s, (size_t)length);
            root[length] = '\0';
        }
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 0 : -1;
}

static int db_open_readonly(const char *path, sqlite3 **db) {
    int rc = sqlite3_open_v2(path, db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "fimctl: baseline database: %s\n", *db ? sqlite3_errmsg(*db) : "open failed");
        if (*db) sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    (void)sqlite3_busy_timeout(*db, 5000);
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(*db, "SELECT version FROM schema_meta", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW || sqlite3_column_int(stmt, 0) != 1) {
        fprintf(stderr, "fimctl: missing, corrupt, or unsupported baseline schema\n");
        sqlite3_finalize(stmt); sqlite3_close(*db); *db = NULL; errno = EINVAL; return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "fimctl: baseline schema version is not unique\n");
        sqlite3_close(*db); *db = NULL; errno = EINVAL; return -1;
    }
    return 0;
}

static int load_rows(sqlite3 *db, struct rows *rows) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT relative_path,device,inode,mode,uid,gid,size,mtime_ns,sha256 "
                      "FROM files ORDER BY relative_path";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *path_data = sqlite3_column_text(stmt, 0);
        const unsigned char *hash_data = sqlite3_column_text(stmt, 8);
        int path_len = sqlite3_column_bytes(stmt, 0);
        int hash_len = sqlite3_column_bytes(stmt, 8);
        if (!path_data || path_len <= 0 || path_len >= PATH_MAX ||
            (int)strlen((const char *)path_data) != path_len || !hash_data || hash_len != 64) {
            rc = SQLITE_CORRUPT; break;
        }
        char *path_copy = strndup((const char *)path_data, (size_t)path_len);
        if (!path_copy) { rc = SQLITE_NOMEM; break; }
        if (!valid_relative_path(path_copy)) {
            free(path_copy); rc = SQLITE_CORRUPT; break;
        }
        int valid_hash = 1;
        for (int i = 0; i < hash_len; ++i) {
            unsigned char c = hash_data[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { valid_hash = 0; break; }
        }
        sqlite3_int64 device = sqlite3_column_int64(stmt, 1);
        sqlite3_int64 inode = sqlite3_column_int64(stmt, 2);
        sqlite3_int64 mode = sqlite3_column_int64(stmt, 3);
        sqlite3_int64 uid = sqlite3_column_int64(stmt, 4);
        sqlite3_int64 gid = sqlite3_column_int64(stmt, 5);
        sqlite3_int64 size = sqlite3_column_int64(stmt, 6);
        sqlite3_int64 mtime = sqlite3_column_int64(stmt, 7);
        if (!valid_hash || device < 0 || inode < 0 || mode < 0 || mode > UINT32_MAX ||
            uid < 0 || uid > UINT32_MAX || gid < 0 || gid > UINT32_MAX || size < 0 || mtime < 0) {
            free(path_copy); rc = SQLITE_CORRUPT; break;
        }
        struct file_row row = {
            .path = path_copy,
            .dev = (uint64_t)device, .ino = (uint64_t)inode,
            .size = (uint64_t)size, .mtime_ns = (uint64_t)mtime,
            .mode = (uint32_t)mode, .uid = (uint32_t)uid, .gid = (uint32_t)gid
        };
        memcpy(row.hash, hash_data, 64U); row.hash[64] = '\0';
        if (push_row(rows, &row) != 0) { free(row.path); rc = SQLITE_NOMEM; break; }
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { free_rows(rows); errno = EINVAL; return -1; }
    return 0;
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
        uint64_t device = (uint64_t)sqlite3_column_int64(stmt, 1);
        uint64_t inode = (uint64_t)sqlite3_column_int64(stmt, 2);
        uint64_t size = (uint64_t)sqlite3_column_int64(stmt, 6);
        uint64_t mtime = (uint64_t)sqlite3_column_int64(stmt, 7);
        uint32_t mode = (uint32_t)sqlite3_column_int64(stmt, 3);
        if (!hash || strcmp(cur->hash, hash) != 0 || cur->size != size) {
            if (json) print_json_path("modified", cur->path);
            else printf("modified %s\n", cur->path);
            ++changed;
        } else if (cur->dev != device || cur->ino != inode ||
                   cur->mtime_ns != mtime || cur->mode != mode ||
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
    if (!in) { if (cap) out[0] = '\0'; return; }
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
                               IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT |
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

static int remember_rename(struct pending_renames *pending, uint32_t cookie,
                           const char *path) {
    if (cookie == 0U || pending->n == MAX_PENDING_RENAMES) { errno = ENOSPC; return -1; }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    struct pending_rename *row = &pending->rows[pending->n];
    int n = snprintf(row->path, sizeof(row->path), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(row->path)) { errno = ENAMETOOLONG; return -1; }
    row->cookie = cookie;
    row->observed = now;
    ++pending->n;
    return 0;
}

static int take_rename(struct pending_renames *pending, uint32_t cookie) {
    for (size_t i = 0; i < pending->n; ++i) {
        if (pending->rows[i].cookie != cookie) continue;
        if (i + 1U < pending->n)
            memmove(&pending->rows[i], &pending->rows[i + 1U],
                    (pending->n - i - 1U) * sizeof(pending->rows[0]));
        --pending->n;
        return 1;
    }
    return 0;
}

static int expire_renames(struct pending_renames *pending, int logfd, const char *coverage) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    for (size_t i = 0; i < pending->n;) {
        time_t age = now.tv_sec - pending->rows[i].observed.tv_sec;
        if (now.tv_nsec < pending->rows[i].observed.tv_nsec) --age;
        if (age < RENAME_TIMEOUT_SECONDS) { ++i; continue; }
        if (emit_event(logfd, "file.rename_timeout", pending->rows[i].path,
                       coverage, IN_MOVED_FROM, pending->rows[i].cookie) != 0) return -1;
        if (i + 1U < pending->n)
            memmove(&pending->rows[i], &pending->rows[i + 1U],
                    (pending->n - i - 1U) * sizeof(pending->rows[0]));
        --pending->n;
    }
    return 0;
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
        if (lstat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_dev == root_dev &&
                add_tree_watches(inofd, ws, child, root_dev, depth + 1U) != 0) {
                rc = -1; break;
            }
        } else if (errno != ENOENT) {
            rc = -1; break;
        }
    }
    int saved_error = rc == 0 ? 0 : errno;
    closedir(dir);
    if (rc != 0) errno = saved_error ? saved_error : EIO;
    return rc;
}

static int emit_event_full(int fd, const char *kind, const char *path, const char *coverage,
                           uint32_t mask, uint32_t cookie, const char *reason,
                           int reconciliation, uint32_t observation_count,
                           uint64_t first_observed_ns, uint64_t last_observed_ns,
                           const char *before_sha256, const char *after_sha256,
                           int error_errno) {
    char escaped[PATH_MAX * 6U + 1U]; json_escape(path ? path : "", escaped, sizeof(escaped));
    char escaped_reason[256], reason_json[260];
    if (reason) {
        json_escape(reason, escaped_reason, sizeof(escaped_reason));
        (void)snprintf(reason_json, sizeof(reason_json), "\"%s\"", escaped_reason);
    } else {
        (void)snprintf(reason_json, sizeof(reason_json), "null");
    }
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
    char first_observed_json[32], last_observed_json[32];
    if (first_observed_ns) (void)snprintf(first_observed_json, sizeof(first_observed_json),
                                          "%llu", (unsigned long long)first_observed_ns);
    else (void)snprintf(first_observed_json, sizeof(first_observed_json), "null");
    if (last_observed_ns) (void)snprintf(last_observed_json, sizeof(last_observed_json),
                                         "%llu", (unsigned long long)last_observed_ns);
    else (void)snprintf(last_observed_json, sizeof(last_observed_json), "null");
    char before_hash_json[68], after_hash_json[68];
    if (before_sha256) (void)snprintf(before_hash_json, sizeof(before_hash_json), "\"%s\"", before_sha256);
    else (void)snprintf(before_hash_json, sizeof(before_hash_json), "null");
    if (after_sha256) (void)snprintf(after_hash_json, sizeof(after_hash_json), "\"%s\"", after_sha256);
    else (void)snprintf(after_hash_json, sizeof(after_hash_json), "null");
    char error_errno_json[32];
    if (error_errno > 0) (void)snprintf(error_errno_json, sizeof(error_errno_json), "%d", error_errno);
    else (void)snprintf(error_errno_json, sizeof(error_errno_json), "null");
    int n = snprintf(line, sizeof(line),
        "{\"schema_version\":\"event.v1\",\"event_id\":\"%s\","
        "\"source\":\"fim\",\"host_id\":\"local\",\"observed_at\":\"%s\","
        "\"kind\":\"%s\",\"severity\":\"info\",\"target\":{\"path\":\"%s\"},"
        "\"actor\":{\"pid\":null,\"start_time_ticks\":null,\"attribution\":\"unknown\"},"
        "\"evidence\":{\"event_mask\":%u,\"rename_cookie\":%u,"
        "\"reconciliation\":%s,\"observation_count\":%u,"
        "\"first_observed_unix_ns\":%s,\"last_observed_unix_ns\":%s,"
        "\"before_sha256\":%s,\"after_sha256\":%s,\"error_errno\":%s},"
        "\"coverage\":{\"state\":\"%s\",\"reason\":%s}}\n",
        event_id, observed, kind, escaped,
        mask, cookie, reconciliation ? "true" : "false", observation_count,
        first_observed_json, last_observed_json, before_hash_json, after_hash_json,
        error_errno_json, coverage, reason_json);
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

static int emit_event(int fd, const char *kind, const char *path, const char *coverage,
                      uint32_t mask, uint32_t cookie) {
    return emit_event_full(fd, kind, path, coverage, mask, cookie, NULL, 0, 1U,
                           0U, 0U, NULL, NULL, 0);
}

static int emit_coverage_event(int fd, const char *path, const char *state,
                               const char *reason, uint32_t mask) {
    char kind[64];
    int n = snprintf(kind, sizeof(kind), "coverage.%s", state);
    if (n < 0 || (size_t)n >= sizeof(kind)) { errno = EOVERFLOW; return -1; }
    return emit_event_full(fd, kind, path, state, mask, 0U, reason, 0, 0U,
                           0U, 0U, NULL, NULL, 0);
}

static int commit_hash_job(struct hash_job *job, int logfd,
                           const struct watch_context *ctx) {
    if (!ctx->snapshot || (ctx->snapshot->n > 0U && !ctx->snapshot->v)) {
        errno = EIO; return -1;
    }
    int found = 0;
    size_t old_index = row_index(ctx->snapshot, job->relative, &found);
    if (found && (!ctx->snapshot->v || old_index >= ctx->snapshot->n)) {
        errno = EIO; return -1;
    }
    char before_hash[65];
    const char *before_sha256 = NULL;
    if (found) {
        memcpy(before_hash, ctx->snapshot->v[old_index].hash, sizeof(before_hash));
        before_sha256 = before_hash;
    }
    if (job->scan_rc < 0) {
        const char *reason = job->error_errno == EAGAIN
            ? "unstable_file_retry_exhausted" : "file_hash_failed";
        return emit_event_full(logfd, "file.unknown", job->event.path, "degraded",
                               job->event.mask, 0U, reason, 0, job->event.count,
                               job->event.first_realtime_ns, job->event.last_realtime_ns,
                               before_sha256, NULL, job->error_errno);
    }

    const char *kind = NULL;
    const char *after_sha256 = job->scan_rc == 0 ? job->current.hash : NULL;
    if (job->scan_rc == 1) {
        if (found) {
            if (rows_remove(ctx->snapshot, old_index) != 0) return -1;
            kind = "file.deleted";
        }
        else kind = "file.unchanged";
    } else if (!found) {
        kind = "file.created";
        if (rows_upsert(ctx->snapshot, &job->current) != 0) return -1;
    } else {
        const struct file_row *old = &ctx->snapshot->v[old_index];
        if (strcmp(old->hash, job->current.hash) != 0 || old->size != job->current.size)
            kind = "file.modified";
        else if (rows_differ(old, &job->current))
            kind = "file.metadata_changed";
        else
            kind = "file.unchanged";
        if (rows_upsert(ctx->snapshot, &job->current) != 0) return -1;
    }
    return emit_event_full(logfd, kind, job->event.path, job->coverage,
                           job->event.mask, 0U, NULL, 0, job->event.count,
                           job->event.first_realtime_ns, job->event.last_realtime_ns,
                           before_sha256, after_sha256, 0);
}

static int hash_pool_reap_ready(struct hash_pool *pool, uint64_t *next_commit_sequence,
                                int logfd, const struct watch_context *ctx,
                                size_t *reaped) {
    *reaped = 0U;
    for (;;) {
        int rc = pthread_mutex_lock(&pool->mutex);
        if (rc != 0) { errno = rc; return -1; }
        int worker_error = atomic_load_explicit(&pool->worker_error, memory_order_acquire);
        if (worker_error) {
            (void)pthread_mutex_unlock(&pool->mutex);
            errno = worker_error;
            return -1;
        }
        struct hash_job *job = pool->done_head;
        if (!job || job->sequence != *next_commit_sequence) {
            (void)pthread_mutex_unlock(&pool->mutex);
            return 0;
        }
        pool->done_head = job->next;
        job->next = NULL;
        (void)pthread_mutex_unlock(&pool->mutex);

        int commit_rc = commit_hash_job(job, logfd, ctx);
        free_hash_job(job);
        if (pool->outstanding > 0U) --pool->outstanding;
        if (commit_rc != 0) { pool->consumer_error = 1; return -1; }
        if (*next_commit_sequence == UINT64_MAX) {
            pool->consumer_error = 1; errno = EOVERFLOW; return -1;
        }
        ++*next_commit_sequence;
        ++*reaped;
    }
}

static int hash_pool_consume_notification(struct hash_pool *pool) {
    uint64_t completions;
    for (;;) {
        ssize_t n = read(pool->completion_fd, &completions, sizeof(completions));
        if (n == (ssize_t)sizeof(completions)) return 0;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return 0;
        if (n >= 0) errno = EIO;
        return -1;
    }
}

static int hash_pool_wait_for_completion(struct hash_pool *pool) {
    struct pollfd pfd = { .fd = pool->completion_fd, .events = POLLIN };
    for (;;) {
        int rc = poll(&pfd, 1, 1000);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        if (rc == 0) return 0;
        if ((pfd.revents & POLLIN) && hash_pool_consume_notification(pool) == 0) return 0;
        errno = EIO;
        return -1;
    }
}

static int hash_pool_submit(struct hash_pool *pool, struct hash_job *job, int logfd,
                            const struct watch_context *ctx) {
    if (pool->consumer_error) { errno = EIO; return -1; }
    if (pool->next_sequence == UINT64_MAX) { errno = EOVERFLOW; return -1; }
    while (pool->outstanding >= MAX_PENDING_HASH_JOBS) {
        size_t reaped = 0U;
        if (hash_pool_reap_ready(pool, ctx->next_commit_sequence, logfd, ctx, &reaped) != 0)
            return -1;
        if (pool->outstanding < MAX_PENDING_HASH_JOBS) break;
        if (hash_pool_wait_for_completion(pool) != 0) return -1;
    }
    int rc = pthread_mutex_lock(&pool->mutex);
    if (rc != 0) { errno = rc; return -1; }
    int worker_error = atomic_load_explicit(&pool->worker_error, memory_order_acquire);
    if (pool->stopping || worker_error) {
        int error = worker_error ? worker_error : ESHUTDOWN;
        (void)pthread_mutex_unlock(&pool->mutex);
        errno = error;
        return -1;
    }
    job->sequence = pool->next_sequence++;
    job->next = NULL;
    if (pool->work_tail) pool->work_tail->next = job;
    else pool->work_head = job;
    pool->work_tail = job;
    ++pool->outstanding;
    rc = pthread_cond_signal(&pool->work_ready);
    (void)pthread_mutex_unlock(&pool->mutex);
    if (rc != 0) {
        atomic_store_explicit(&pool->worker_error, rc, memory_order_release);
        notify_hash_completion(pool);
    }
    return 0;
}

static int hash_pool_drain(struct hash_pool *pool, int logfd,
                           const struct watch_context *ctx) {
    if (pool->consumer_error) { errno = EIO; return -1; }
    while (pool->outstanding > 0U) {
        size_t reaped = 0U;
        if (hash_pool_reap_ready(pool, ctx->next_commit_sequence, logfd, ctx, &reaped) != 0)
            return -1;
        if (pool->outstanding > 0U && reaped == 0U &&
            hash_pool_wait_for_completion(pool) != 0) return -1;
    }
    return 0;
}

static uint64_t clock_ns(clockid_t clock_id) {
    struct timespec now;
    if (clock_gettime(clock_id, &now) != 0 || now.tv_sec < 0) return 0U;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void remove_pending_event(struct event_batch *batch, size_t index) {
    if (index + 1U < batch->n)
        memmove(&batch->rows[index], &batch->rows[index + 1U],
                (batch->n - index - 1U) * sizeof(batch->rows[0]));
    --batch->n;
}

static int flush_pending_event(struct event_batch *batch, size_t index, int logfd,
                               const char *coverage, const struct watch_context *ctx) {
    const struct pending_file_event event = batch->rows[index];
    size_t root_len = strlen(ctx->root);
    if (strncmp(event.path, ctx->root, root_len) != 0 || event.path[root_len] != '/') {
        errno = EPERM; return -1;
    }
    const char *relative = event.path + root_len + 1U;
    if (!valid_relative_path(relative)) { errno = EINVAL; return -1; }
    struct hash_job *job = calloc(1U, sizeof(*job));
    if (!job) return -1;
    job->event = event;
    int n = snprintf(job->relative, sizeof(job->relative), "%s", relative);
    if (n < 0 || (size_t)n >= sizeof(job->relative)) {
        free_hash_job(job); errno = ENAMETOOLONG; return -1;
    }
    n = snprintf(job->coverage, sizeof(job->coverage), "%s", coverage);
    if (n < 0 || (size_t)n >= sizeof(job->coverage)) {
        free_hash_job(job); errno = EOVERFLOW; return -1;
    }
    if (hash_pool_submit(ctx->hash_pool, job, logfd, ctx) != 0) {
        free_hash_job(job); return -1;
    }
    remove_pending_event(batch, index);
    return 0;
}

static int flush_pending_events(struct event_batch *batch, int logfd, const char *coverage,
                                const struct watch_context *ctx) {
    while (batch->n) if (flush_pending_event(batch, 0U, logfd, coverage, ctx) != 0) return -1;
    return 0;
}

static int flush_expired_events(struct event_batch *batch, int logfd, const char *coverage,
                                const struct watch_context *ctx) {
    uint64_t now = clock_ns(CLOCK_MONOTONIC);
    if (!now) return -1;
    uint64_t window = (uint64_t)batch->window_ms * 1000000ULL;
    for (size_t i = 0; i < batch->n;) {
        uint64_t last = batch->rows[i].last_monotonic_ns;
        if (now >= last && now - last >= window) {
            if (flush_pending_event(batch, i, logfd, coverage, ctx) != 0) return -1;
        } else {
            ++i;
        }
    }
    return 0;
}

static int pending_event_add(struct event_batch *batch, const char *path, uint32_t mask,
                             int logfd, const char *coverage, const struct watch_context *ctx) {
    uint64_t mono = clock_ns(CLOCK_MONOTONIC), realtime = clock_ns(CLOCK_REALTIME);
    if (!mono || !realtime) return -1;
    uint64_t window = (uint64_t)batch->window_ms * 1000000ULL;
    for (size_t i = 0; i < batch->n; ++i) {
        struct pending_file_event *event = &batch->rows[i];
        if (strcmp(event->path, path) != 0) continue;
        if (mono >= event->last_monotonic_ns && mono - event->last_monotonic_ns < window) {
            event->mask |= mask;
            uint32_t action = mask & (IN_CREATE | IN_DELETE);
            if (action) event->last_action_mask = action;
            if (event->count < UINT32_MAX) ++event->count;
            event->last_monotonic_ns = mono;
            event->last_realtime_ns = realtime;
            return 0;
        }
        if (flush_pending_event(batch, i, logfd, coverage, ctx) != 0) return -1;
        break;
    }
    if (batch->n == MAX_PENDING_EVENTS) {
        size_t oldest = 0U;
        for (size_t i = 1U; i < batch->n; ++i)
            if (batch->rows[i].last_monotonic_ns < batch->rows[oldest].last_monotonic_ns)
                oldest = i;
        if (flush_pending_event(batch, oldest, logfd, coverage, ctx) != 0) return -1;
    }
    struct pending_file_event *event = &batch->rows[batch->n];
    int n = snprintf(event->path, sizeof(event->path), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(event->path)) { errno = ENAMETOOLONG; return -1; }
    event->mask = mask;
    event->last_action_mask = mask & (IN_CREATE | IN_DELETE);
    event->count = 1U;
    event->first_monotonic_ns = mono;
    event->last_monotonic_ns = mono;
    event->first_realtime_ns = realtime;
    event->last_realtime_ns = realtime;
    ++batch->n;
    return 0;
}

static int pending_poll_timeout(const struct event_batch *batch, int maximum_ms) {
    if (batch->n == 0U) return maximum_ms;
    uint64_t now = clock_ns(CLOCK_MONOTONIC);
    if (!now) return maximum_ms;
    uint64_t window = (uint64_t)batch->window_ms * 1000000ULL;
    uint64_t remaining = UINT64_MAX;
    for (size_t i = 0; i < batch->n; ++i) {
        uint64_t last = batch->rows[i].last_monotonic_ns;
        uint64_t deadline = last > UINT64_MAX - window ? UINT64_MAX : last + window;
        uint64_t left = deadline > now ? deadline - now : 0U;
        if (left < remaining) remaining = left;
    }
    uint64_t ms = (remaining + 999999ULL) / 1000000ULL;
    if (ms > (uint64_t)maximum_ms) return maximum_ms;
    return (int)ms;
}

static int rows_differ(const struct file_row *left, const struct file_row *right) {
    return left->dev != right->dev || left->ino != right->ino ||
           strcmp(left->hash, right->hash) != 0 || left->size != right->size ||
           left->mtime_ns != right->mtime_ns || left->mode != right->mode ||
           left->uid != right->uid || left->gid != right->gid;
}

static int emit_reconciliation_diff(int logfd, const char *root,
                                    const struct rows *before, const struct rows *after,
                                    const char *reason) {
    size_t i = 0, j = 0;
    while (i < before->n || j < after->n) {
        int order = i == before->n ? 1 : j == after->n ? -1 :
                    strcmp(before->v[i].path, after->v[j].path);
        const char *kind = NULL, *path = NULL;
        const char *before_sha256 = NULL, *after_sha256 = NULL;
        if (order < 0) {
            kind = "file.deleted"; path = before->v[i].path;
            before_sha256 = before->v[i++].hash;
        } else if (order > 0) {
            kind = "file.created"; path = after->v[j].path;
            after_sha256 = after->v[j++].hash;
        } else {
            if (strcmp(before->v[i].hash, after->v[j].hash) != 0 ||
                before->v[i].size != after->v[j].size) {
                kind = "file.modified";
                before_sha256 = before->v[i].hash; after_sha256 = after->v[j].hash;
            } else if (rows_differ(&before->v[i], &after->v[j])) {
                kind = "file.metadata_changed";
                before_sha256 = before->v[i].hash; after_sha256 = after->v[j].hash;
            }
            path = after->v[j].path; ++i; ++j;
        }
        if (kind) {
            char full[PATH_MAX];
            int n = snprintf(full, sizeof(full), "%s/%s", root, path);
            if (n < 0 || (size_t)n >= sizeof(full)) { errno = ENAMETOOLONG; return -1; }
            if (emit_event_full(logfd, kind, full, "recovering", 0U, 0U,
                                reason, 1, 0U, 0U, 0U,
                                before_sha256, after_sha256, 0) != 0) return -1;
        }
    }
    return 0;
}

static void free_watches(struct watches *ws) {
    for (size_t i = 0; i < ws->n; ++i) free(ws->v[i].path);
    free(ws->v);
    memset(ws, 0, sizeof(*ws));
}

/* Returns 0 for complete watch+scan coverage, -1 for a visible degraded state,
 * and -2 when the event journal itself cannot record the transition. */
static int rebuild_and_reconcile(int *inofd, struct watches *ws, const char *root,
                                 dev_t root_dev, struct rows *snapshot, int *have_snapshot,
                                 struct event_batch *batch, int logfd,
                                 const struct watch_context *ctx, const char *reason) {
    if (strcmp(reason, "watch_startup") != 0 &&
        emit_coverage_event(logfd, root, "degraded", reason, 0U) != 0) return -2;
    if (emit_coverage_event(logfd, root, "recovering", reason, 0U) != 0) return -2;
    if (flush_pending_events(batch, logfd, "recovering", ctx) != 0 ||
        hash_pool_drain(ctx->hash_pool, logfd, ctx) != 0) return -2;
    if (*inofd >= 0) close(*inofd);
    *inofd = -1;
    free_watches(ws);
    errno = 0;
    *inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int watch_error = 0;
    int watches_ok = 0;
    if (*inofd >= 0) {
        errno = EIO;
        watches_ok = add_tree_watches(*inofd, ws, root, root_dev, 0U) == 0;
        if (!watches_ok) watch_error = errno;
    } else {
        watch_error = errno ? errno : EIO;
    }

    struct rows current = {0};
    errno = EIO;
    int scan_ok = scan_root(root, &current) == 0;
    int scan_error = scan_ok ? 0 : errno;
    if (scan_ok) {
        if (*have_snapshot && emit_reconciliation_diff(logfd, root, snapshot, &current,
                                                        "full_reconciliation") != 0) {
            free_rows(&current);
            return -2;
        }
        free_rows(snapshot);
        *snapshot = current;
        *have_snapshot = 1;
    } else {
        free_rows(&current);
    }

    if (watches_ok && scan_ok) {
        if (emit_coverage_event(logfd, root, "healthy", "reconciliation_complete", 0U) != 0)
            return -2;
        return 0;
    }
    const char *failure = !watches_ok && !scan_ok ? "watch_and_scan_failed" :
                          !watches_ok ? "watch_registration_failed" : "full_scan_failed";
    fprintf(stderr, "coverage degraded: %s (watch errno=%d, scan errno=%d)\n",
            failure, watch_error, scan_error);
    if (emit_coverage_event(logfd, root, "degraded", failure, 0U) != 0) return -2;
    return -1;
}

/* Refreshes the current-state snapshot without replacing the inotify instance or DB baseline. */
static int verify_current_snapshot(const char *root, struct rows *snapshot,
                                   int *have_snapshot, struct event_batch *batch, int logfd,
                                   int *coverage_healthy, const struct watch_context *ctx,
                                   const char *reason, const char *success_reason) {
    if (emit_coverage_event(logfd, root, "recovering", reason, 0U) != 0) return -1;
    if (flush_pending_events(batch, logfd, *coverage_healthy ? "healthy" : "degraded", ctx) != 0 ||
        hash_pool_drain(ctx->hash_pool, logfd, ctx) != 0)
        return -1;
    struct rows current = {0};
    errno = EIO;
    if (scan_root(root, &current) != 0) {
        int saved_error = errno ? errno : EIO;
        fprintf(stderr, "fimctl: full verification failed (errno=%d)\n", saved_error);
        *coverage_healthy = 0;
        if (emit_coverage_event(logfd, root, "degraded", "full_verification_failed", 0U) != 0)
            return -1;
        return 0;
    }
    if (*have_snapshot && emit_reconciliation_diff(logfd, root, snapshot, &current, reason) != 0) {
        free_rows(&current); return -1;
    }
    free_rows(snapshot); *snapshot = current; *have_snapshot = 1;
    if (emit_coverage_event(logfd, root, "healthy", success_reason, 0U) != 0) return -1;
    *coverage_healthy = 1;
    return 0;
}

static int cmd_watch(const char *root_arg, const char *events, const char *pidfile,
                     const char *db_path, unsigned int verify_interval_seconds,
                     int inject_overflow_once) {
    char root[PATH_MAX];
    if (canonical_root(root_arg, root) != 0) return fail("invalid or unsafe root");
    sqlite3 *db = NULL;
    if (db_open_readonly(db_path, &db) != 0) return 1;
    char baseline_root[PATH_MAX];
    if (get_root(db, baseline_root) != 0 || strcmp(root, baseline_root) != 0) {
        sqlite3_close(db); errno = EINVAL; return fail("--root does not match the baseline database");
    }
    struct rows snapshot = {0};
    if (load_rows(db, &snapshot) != 0) {
        sqlite3_close(db); return fail("load baseline rows");
    }
    sqlite3_close(db);
    int have_snapshot = 1;
    int inofd = -1;
    struct watches ws = {0};
    struct pending_renames renames = {0};
    int rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat rootst;
    if (rootfd < 0 || fstat(rootfd, &rootst) != 0) {
        if (rootfd >= 0) close(rootfd);
        free_rows(&snapshot);
        return fail("open watch root");
    }
    int logfd = open(events, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (logfd < 0) { close(rootfd); free_rows(&snapshot); return fail("open event log"); }
    struct event_batch batch = { .window_ms = EVENT_DEBOUNCE_MS };
    batch.rows = calloc(MAX_PENDING_EVENTS, sizeof(*batch.rows));
    if (!batch.rows) { close(rootfd); close(logfd); free_rows(&snapshot); return fail("allocate event coalescing table"); }
    int pfd = open(pidfile, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (pfd < 0) { free(batch.rows); close(rootfd); close(logfd); free_rows(&snapshot); return fail("pidfile exists or inaccessible"); }
    dprintf(pfd, "%ld\n", (long)getpid()); close(pfd);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    struct hash_pool hash_pool;
    if (hash_pool_start(&hash_pool, rootfd, rootst.st_dev) != 0) {
        int saved_error = errno;
        unlink(pidfile); free(batch.rows); close(rootfd); close(logfd); free_rows(&snapshot);
        errno = saved_error; return fail("start hash worker pool");
    }
    int exit_status = 0;
    uint64_t next_commit_sequence = 1U;
    struct watch_context ctx = { .root = root, .rootfd = rootfd,
                                 .root_dev = rootst.st_dev, .snapshot = &snapshot,
                                 .hash_pool = &hash_pool,
                                 .next_commit_sequence = &next_commit_sequence };
    int coverage_healthy = 0;
    int recovery_rc = rebuild_and_reconcile(&inofd, &ws, root, rootst.st_dev,
                                            &snapshot, &have_snapshot, &batch, logfd,
                                            &ctx, "watch_startup");
    if (recovery_rc == -2) { exit_status = 1; stopping = 1; }
    coverage_healthy = recovery_rc == 0;
    uint64_t last_verify_ns = clock_ns(CLOCK_MONOTONIC);
    if (!last_verify_ns) { exit_status = 1; stopping = 1; }
    const uint64_t verify_interval_ns = (uint64_t)verify_interval_seconds * 1000000000ULL;
    unsigned int degraded_idle = 0;
    int injected = 0;
    int inject_ready = 0;
    unsigned char buf[EVENT_BUFFER];
    puts("watching; coverage is recorded in the event journal; Ctrl-C or fimctl stop will stop the monitor");
    while (!stopping) {
        struct pollfd p[2] = {
            { .fd = inofd, .events = POLLIN },
            { .fd = hash_pool.completion_fd, .events = POLLIN }
        };
        int had_pending_events = batch.n != 0U;
        int ready = poll(p, 2, pending_poll_timeout(&batch, 1000));
        if (ready < 0 && errno == EINTR) { if (stopping) break; else continue; }
        int need_reconcile = 0;
        const char *reconcile_reason = NULL;
        int need_snapshot_verify = 0;
        const char *snapshot_verify_reason = NULL;
        if (ready < 0) {
            need_reconcile = 1;
            reconcile_reason = "poll_failed";
        }
        if (ready > 0 && (p[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            errno = EIO; exit_status = 1; stopping = 1; break;
        }
        if (ready > 0 && (p[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            need_reconcile = 1;
            reconcile_reason = "poll_failed";
        }
        if (ready > 0 && (p[1].revents & POLLIN)) {
            if (hash_pool_consume_notification(&hash_pool) != 0) {
                exit_status = 1; stopping = 1; break;
            }
        }
        size_t reaped = 0U;
        if (hash_pool_reap_ready(&hash_pool, &next_commit_sequence, logfd, &ctx, &reaped) != 0) {
            exit_status = 1; stopping = 1; break;
        }
        if (ready == 0) {
            const char *coverage = coverage_healthy ? "healthy" : "degraded";
            if (flush_expired_events(&batch, logfd, coverage, &ctx) != 0 ||
                expire_renames(&renames, logfd, coverage) != 0) {
                exit_status = 1; stopping = 1; break;
            }
            if (inject_overflow_once && inject_ready && !injected && !had_pending_events) {
                need_reconcile = 1;
                reconcile_reason = "test_injected_queue_overflow";
                injected = 1;
            } else if (!coverage_healthy && !had_pending_events && ++degraded_idle >= 5U) {
                need_reconcile = 1;
                reconcile_reason = "watch_recovery_retry";
                degraded_idle = 0;
            }
        }
        ssize_t n = 0;
        if (ready > 0 && (p[0].revents & POLLIN)) {
            n = inofd >= 0 ? read(inofd, buf, sizeof(buf)) : -1;
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n < 0) {
                need_reconcile = 1;
                reconcile_reason = "inotify_read_failed";
                n = 0;
            }
        }
        for (size_t off = 0; off < (size_t)n && !need_reconcile;) {
            struct inotify_event *ev = (struct inotify_event *)(void *)(buf + off);
            const char *base = watch_path(&ws, ev->wd);
            if (ev->mask & IN_Q_OVERFLOW) {
                need_reconcile = 1;
                reconcile_reason = "inotify_queue_overflow";
                break;
            }
            if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) {
                need_reconcile = 1;
                reconcile_reason = "watch_invalidated";
                break;
            }
            if (!base) {
                need_reconcile = 1;
                reconcile_reason = "unknown_watch_descriptor";
                break;
            } else {
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
                    const char *coverage = coverage_healthy ? "healthy" : "degraded";
                    int direct_event = (ev->mask & (IN_MOVED_FROM | IN_MOVED_TO | IN_ISDIR)) != 0U;
                    if ((direct_event && emit_event(logfd, kind, full, coverage,
                                                    ev->mask, ev->cookie) != 0) ||
                        (!direct_event && pending_event_add(&batch, full, ev->mask,
                                                           logfd, coverage, &ctx) != 0)) {
                        exit_status = 1; stopping = 1; break;
                    }
                    inject_ready = 1;
                    if (ev->mask & IN_MOVED_FROM) {
                        if (remember_rename(&renames, ev->cookie, full) != 0) {
                            if (emit_event(logfd, "file.rename_match_failed", full, "degraded",
                                           ev->mask, ev->cookie) != 0) {
                                exit_status = 1; stopping = 1; break;
                            }
                            need_reconcile = 1;
                            reconcile_reason = "rename_correlation_capacity";
                        }
                    } else if (ev->mask & IN_MOVED_TO) {
                        if (take_rename(&renames, ev->cookie)) {
                            if (emit_event(logfd, "path.renamed", full,
                                           coverage_healthy ? "healthy" : "degraded",
                                           ev->mask, ev->cookie) != 0) {
                                exit_status = 1; stopping = 1; break;
                            }
                        } else if (emit_event(logfd, "file.rename_match_failed", full,
                                              "degraded", ev->mask, ev->cookie) != 0) {
                            exit_status = 1; stopping = 1; break;
                        }
                    }
                    if ((ev->mask & (IN_MOVED_FROM | IN_MOVED_TO)) && (ev->mask & IN_ISDIR) == 0U) {
                        need_snapshot_verify = 1;
                        snapshot_verify_reason = "file_move_snapshot_refresh";
                    }
                    if ((ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR)) {
                        need_reconcile = 1;
                        reconcile_reason = "new_directory_discovered";
                    }
                    if ((ev->mask & (IN_MOVED_FROM | IN_MOVED_TO)) && (ev->mask & IN_ISDIR)) {
                        need_reconcile = 1;
                        reconcile_reason = "directory_tree_moved";
                    }
                }
            }
            off += sizeof(*ev) + ev->len;
        }
        if (!stopping && !need_reconcile &&
            (flush_expired_events(&batch, logfd,
                                  coverage_healthy ? "healthy" : "degraded", &ctx) != 0 ||
             expire_renames(&renames, logfd,
                            coverage_healthy ? "healthy" : "degraded") != 0)) {
            exit_status = 1; stopping = 1; break;
        }
        if (!stopping && need_reconcile) {
            recovery_rc = rebuild_and_reconcile(&inofd, &ws, root, rootst.st_dev,
                                                &snapshot, &have_snapshot, &batch, logfd,
                                                &ctx,
                                                reconcile_reason ? reconcile_reason : "unknown_failure");
            if (recovery_rc == -2) { exit_status = 1; stopping = 1; }
            coverage_healthy = recovery_rc == 0;
            degraded_idle = 0;
            last_verify_ns = clock_ns(CLOCK_MONOTONIC);
        } else if (!stopping && need_snapshot_verify) {
            if (verify_current_snapshot(root, &snapshot, &have_snapshot,
                                        &batch, logfd, &coverage_healthy, &ctx,
                                        snapshot_verify_reason, "snapshot_refresh_complete") != 0) {
                exit_status = 1; stopping = 1;
            }
            last_verify_ns = clock_ns(CLOCK_MONOTONIC);
        } else if (!stopping) {
            uint64_t now = clock_ns(CLOCK_MONOTONIC);
            if (!now) {
                exit_status = 1; stopping = 1;
            } else if (now >= last_verify_ns && now - last_verify_ns >= verify_interval_ns) {
                if (verify_current_snapshot(root, &snapshot, &have_snapshot,
                                            &batch, logfd, &coverage_healthy, &ctx,
                                            "periodic_verification",
                                            "periodic_verification_complete") != 0) {
                    exit_status = 1; stopping = 1;
                }
                last_verify_ns = clock_ns(CLOCK_MONOTONIC);
            }
        }
    }
    if (flush_pending_events(&batch, logfd, coverage_healthy ? "healthy" : "degraded", &ctx) != 0 ||
        hash_pool_drain(&hash_pool, logfd, &ctx) != 0) {
        fprintf(stderr, "fimctl: could not flush pending hash work: %s\n", strerror(errno));
        exit_status = 1;
    }
    hash_pool_stop(&hash_pool);
    unlink(pidfile);
    free(batch.rows); free_watches(&ws); free_rows(&snapshot);
    close(logfd);
    close(rootfd);
    if (inofd >= 0) close(inofd);
    return exit_status;
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
         "fimctl watch --root DIR --db FILE --events FILE [--pidfile FILE] "
         "[--verify-interval-seconds N] [--inject-overflow-once]\n"
         "fimctl stop [--pidfile FILE]");
}

static const char *option(int argc, char **argv, const char *name, const char *fallback) {
    for (int i = 2; i + 1 < argc; ++i) if (!strcmp(argv[i], name)) return argv[i + 1];
    return fallback;
}

static int has_flag(int argc, char **argv, const char *name) {
    for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], name)) return 1;
    return 0;
}

static int parse_verify_interval(int argc, char **argv, unsigned int *seconds) {
    const char *value = NULL;
    unsigned int count = 0U;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--verify-interval-seconds") != 0) continue;
        ++count;
        if (i + 1 >= argc) { errno = EINVAL; return -1; }
        value = argv[++i];
    }
    if (count == 0U) { *seconds = DEFAULT_VERIFY_INTERVAL_SECONDS; return 0; }
    if (count != 1U || !value || value[0] < '0' || value[0] > '9') { errno = EINVAL; return -1; }
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || !end || *end || parsed == 0UL ||
        parsed > MAX_VERIFY_INTERVAL_SECONDS) { errno = EINVAL; return -1; }
    *seconds = (unsigned int)parsed;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help")) { usage(); return argc < 2 ? 2 : 0; }
    if (!strcmp(argv[1], "watch")) {
        const char *root = option(argc, argv, "--root", NULL);
        const char *dbpath = option(argc, argv, "--db", NULL);
        const char *events = option(argc, argv, "--events", NULL);
        const char *pidfile = option(argc, argv, "--pidfile", "./fimd.pid");
        unsigned int verify_interval_seconds = 0U;
        if (!root || !dbpath || !events ||
            parse_verify_interval(argc, argv, &verify_interval_seconds) != 0) {
            usage(); return 2;
        }
        return cmd_watch(root, events, pidfile, dbpath, verify_interval_seconds,
                         has_flag(argc, argv, "--inject-overflow-once"));
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
