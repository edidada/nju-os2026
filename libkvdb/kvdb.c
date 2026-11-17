#define _GNU_SOURCE
#include <kvdb.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#define PAGE UINT64_C(4096)
#define COOKIE UINT64_C(0x4b56444232303236)
static const unsigned char record_magic[8] = {'K','V','D','B','v','0','0','1'};
static const unsigned char commit_magic[8] = {'C','O','M','M','I','T','!','!'};
static pthread_mutex_t process_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t fork_once = PTHREAD_ONCE_INIT;
static int fork_error;

static void fork_prepare(void) { pthread_mutex_lock(&process_lock); }
static void fork_release(void) { pthread_mutex_unlock(&process_lock); }
static void register_fork(void) { fork_error = pthread_atfork(fork_prepare, fork_release, fork_release); }
static int enter(void) {
    int error = pthread_once(&fork_once, register_fork);
    if (!error) error = fork_error;
    if (!error) error = pthread_mutex_lock(&process_lock);
    if (error) { errno = error; return -1; }
    return 0;
}
static int leave(int result) {
    int saved = errno;
    pthread_mutex_unlock(&process_lock);
    errno = saved;
    return result;
}
static uint64_t decode(const unsigned char *p) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}
static void encode(unsigned char *p, uint64_t value) {
    for (int i = 0; i < 8; i++) { p[i] = (unsigned char)value; value >>= 8; }
}
static int transfer(int fd, void *data, size_t size, off_t offset, int writing) {
    unsigned char *p = data;
    while (size) {
        size_t count = size > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : size;
        ssize_t n = writing ? pwrite(fd, p, count, offset) : pread(fd, p, count, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += n; size -= (size_t)n; offset += n;
    }
    return 0;
}
static int data_sync(int fd) {
    int result;
    do { result = fdatasync(fd); } while (result < 0 && errno == EINTR);
    return result;
}
static int truncate_to(int fd, off_t size) {
    int result;
    do { result = ftruncate(fd, size); } while (result < 0 && errno == EINTR);
    return result;
}
static int file_lock(int fd, int operation) {
    int result;
    do { result = flock(fd, operation); } while (result < 0 && errno == EINTR);
    return result;
}
static int lock_database(struct kvdb_t *db) {
    if (!db || db->cookie != COOKIE || db->fd < 0) { errno = EBADF; return -1; }
    // flock is attached to an open file description. A dup or a descriptor
    // inherited across fork would share that lock, so reopen the same inode.
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", db->fd);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    if (file_lock(fd, LOCK_EX)) { int saved = errno; close(fd); errno = saved; return -1; }
    return fd;
}
static int unlock_database(int fd, int result) {
    int saved = errno;
    if (file_lock(fd, LOCK_UN) && result >= 0) { saved = errno; result = -1; }
    if (close(fd) && result >= 0) { saved = errno; result = -1; }
    errno = saved;
    return result;
}
static int record_layout(uint64_t offset, uint64_t key, uint64_t value,
                         uint64_t *commit, uint64_t *end) {
    if (key > INT64_MAX || value > INT64_MAX || key > (uint64_t)INT64_MAX - value) return -1;
    uint64_t payload = key + value;
    if (payload > (uint64_t)INT64_MAX - (PAGE - 1)) return -1;
    uint64_t rounded = (payload + PAGE - 1) & ~(PAGE - 1);
    if (rounded > (uint64_t)INT64_MAX - 2 * PAGE ||
        offset > (uint64_t)INT64_MAX - 2 * PAGE - rounded) return -1;
    *commit = offset + PAGE + rounded;
    *end = *commit + PAGE;
    return 0;
}

typedef struct {
    uint64_t end, sequence, value_offset, value_size;
    int found;
} State;

// A valid footer can only be written after the entire payload is synced.
// Recovery synchronizes an observed footer before making its value visible:
// the previous writer may have died between its footer write and final sync.
static int recover(int fd, const char *key, size_t key_size, State *state) {
    memset(state, 0, sizeof(*state));
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0) return -1;
    uint64_t size = (uint64_t)st.st_size;
    while (state->end <= size && size - state->end >= 2 * PAGE) {
        unsigned char header[32], footer[8];
        uint64_t offset = state->end;
        if (transfer(fd, header, sizeof(header), (off_t)offset, 0)) return -1;
        if (memcmp(header, record_magic, 8)) break;
        uint64_t klen = decode(header + 8), vlen = decode(header + 16), sequence = decode(header + 24);
        uint64_t commit, end;
        if (state->sequence == UINT64_MAX || sequence != state->sequence + 1 ||
            record_layout(offset, klen, vlen, &commit, &end) || end > size) break;
        if (transfer(fd, footer, sizeof(footer), (off_t)commit, 0)) return -1;
        if (memcmp(footer, commit_magic, 8)) break;
        if (key && klen == key_size) {
            unsigned char buffer[4096];
            size_t compared = 0;
            int matches = 1;
            while (compared < key_size) {
                size_t n = key_size - compared;
                if (n > sizeof(buffer)) n = sizeof(buffer);
                if (transfer(fd, buffer, n, (off_t)(offset + PAGE + compared), 0)) return -1;
                if (memcmp(buffer, key + compared, n)) { matches = 0; break; }
                compared += n;
            }
            if (matches) {
                state->found = 1;
                state->value_offset = offset + PAGE + klen;
                state->value_size = vlen;
            }
        }
        state->sequence = sequence;
        state->end = end;
    }
    if (state->end != size && truncate_to(fd, (off_t)state->end)) return -1;
    return data_sync(fd);
}

static int sync_parent(const char *path) {
    char *copy = strdup(path);
    if (!copy) return -1;
    char *slash = strrchr(copy, '/');
    const char *parent = ".";
    if (slash) { if (slash == copy) slash[1] = 0; else *slash = 0; parent = copy; }
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(copy);
    if (fd < 0) return -1;
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    int saved = errno;
    if (close(fd) && !result) { saved = errno; result = -1; }
    errno = saved;
    return result;
}

int kvdb_open(struct kvdb_t *db, const char *path) {
    if (!db || !path || !*path) { errno = EINVAL; return -1; }
    if (enter()) return -1;
    db->fd = -1; db->path = NULL; db->cookie = 0;
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NONBLOCK, 0666);
    if (fd < 0) return leave(-1);
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd); errno = EINVAL; return leave(-1);
    }
    db->path = strdup(path);
    if (!db->path) { close(fd); return leave(-1); }
    db->fd = fd; db->cookie = COOKIE;
    int operation = lock_database(db);
    State state;
    int result = operation < 0 ? -1 : recover(operation, NULL, 0, &state);
    if (!result) result = sync_parent(path);
    if (operation >= 0) result = unlock_database(operation, result);
    if (result) {
        int saved = errno;
        close(db->fd); free(db->path); db->fd = -1; db->path = NULL; db->cookie = 0;
        errno = saved;
    }
    return leave(result);
}

int kvdb_put(struct kvdb_t *db, const char *key, const char *value) {
    if (!key || !value) { errno = EINVAL; return -1; }
    if (enter()) return -1;
    int fd = lock_database(db);
    if (fd < 0) return leave(-1);
    State state;
    int result = recover(fd, NULL, 0, &state);
    uint64_t commit = 0, end = 0;
    size_t key_size = strlen(key), value_size = strlen(value);
    if (!result && (state.sequence == UINT64_MAX || record_layout(state.end, key_size, value_size, &commit, &end))) {
        errno = EOVERFLOW; result = -1;
    }
    unsigned char fields[24];
    encode(fields, key_size); encode(fields + 8, value_size); encode(fields + 16, state.sequence + 1);
    // Stage 1: new extent is zero, and immutable length/sequence fields are
    // durable BEFORE the record magic could make them parseable.
    if (!result && (truncate_to(fd, (off_t)end) ||
        transfer(fd, fields, sizeof(fields), (off_t)(state.end + 8), 1) || data_sync(fd))) result = -1;
    // Stage 2: publish magic and complete data, then sync. The commit page is
    // still entirely zero, so a torn/short payload cannot become committed.
    if (!result && (transfer(fd, (void *)record_magic, 8, (off_t)state.end, 1) ||
        transfer(fd, (void *)key, key_size, (off_t)(state.end + PAGE), 1) ||
        transfer(fd, (void *)value, value_size, (off_t)(state.end + PAGE + key_size), 1) || data_sync(fd))) result = -1;
    // Stage 3: only now append the nonzero commit marker and sync before ACK.
    if (!result && (transfer(fd, (void *)commit_magic, 8, (off_t)commit, 1) || data_sync(fd))) result = -1;
    return leave(unlock_database(fd, result));
}

int kvdb_get(struct kvdb_t *db, const char *key, char *buf, size_t length) {
    if (!key || (length && !buf)) { errno = EINVAL; return -1; }
    if (enter()) return -1;
    int fd = lock_database(db);
    if (fd < 0) return leave(-1);
    State state;
    int result = recover(fd, key, strlen(key), &state);
    if (!result && !state.found) { errno = ENOENT; result = -1; }
    if (!result) {
        uint64_t copied = length ? length - 1 : 0;
        if (copied > state.value_size) copied = state.value_size;
        if (copied > INT_MAX) { errno = EOVERFLOW; result = -1; }
        else {
            if (length) buf[0] = 0;
            if (copied && transfer(fd, buf, (size_t)copied, (off_t)state.value_offset, 0)) result = -1;
            else { if (length) buf[copied] = 0; result = (int)copied; }
        }
    }
    return leave(unlock_database(fd, result));
}

int kvdb_close(struct kvdb_t *db) {
    if (enter()) return -1;
    if (!db || db->cookie != COOKIE || db->fd < 0) { errno = EBADF; return leave(-1); }
    int result = close(db->fd);
    free(db->path); db->path = NULL; db->fd = -1; db->cookie = 0;
    return leave(result);
}
