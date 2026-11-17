#define _GNU_SOURCE
#include "kvdb.h"
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ssize_t __real_pwrite(int, const void *, size_t, off_t);
int __real_ftruncate(int, off_t);
int __real_fdatasync(int);
static int mode, cut, event_count, stable_fd = -1, current_fd = -1, event_fd = -1;
static int base_fd, write_interrupt, sync_interrupt, truncate_interrupt;
static int error_number, error_count, error_hit;
static char error_kind;
static struct stat identity;

static int target(int fd) {
    if (!mode) return 0;
    struct stat st;
    assert(fstat(fd, &st) == 0);
    return st.st_dev == identity.st_dev && st.st_ino == identity.st_ino;
}
static void snapshot(int source, int destination) {
    struct stat st;
    assert(fstat(source, &st) == 0);
    assert(__real_ftruncate(destination, st.st_size) == 0);
    char buffer[16384];
    for (off_t offset = 0; offset < st.st_size;) {
        ssize_t n = pread(source, buffer, sizeof(buffer), offset);
        assert(n > 0);
        ssize_t written = 0;
        while (written < n) {
            ssize_t k = __real_pwrite(destination, buffer + written, (size_t)(n - written), offset + written);
            assert(k > 0); written += k;
        }
        offset += n;
    }
}
static void event(char kind, int fd) {
    event_count++;
    assert(dprintf(event_fd, "%d %c\n", event_count, kind) > 0);
    if (event_count == cut) {
        snapshot(fd, current_fd);
        kill(getpid(), SIGKILL);
        _exit(99);
    }
}
static int injected(char kind) {
    if (mode != 3 || error_kind != kind) return 0;
    if (++error_count != error_number) return 0;
    error_hit = 1; mode = 0; errno = EIO; return 1;
}
ssize_t __wrap_pwrite(int fd, const void *data, size_t n, off_t offset) {
    int active = target(fd);
    if (active && injected('W')) return -1;
    if (active && mode == 2) {
        if (!write_interrupt++) { errno = EINTR; return -1; }
        if (n > 7) n = 7;
    }
    ssize_t result = __real_pwrite(fd, data, n, offset);
    if (active && mode == 1 && result >= 0) event('W', fd);
    return result;
}
int __wrap_ftruncate(int fd, off_t size) {
    int active = target(fd);
    if (active && injected('T')) return -1;
    if (active && mode == 2 && !truncate_interrupt++) { errno = EINTR; return -1; }
    int result = __real_ftruncate(fd, size);
    if (active && mode == 1 && !result) event('T', fd);
    return result;
}
/* kvdb.c uses _FILE_OFFSET_BITS=64; glibc redirects these symbols even on
 * a 64-bit runner. Route both interfaces through the same fault model. */
_Static_assert(sizeof(off_t) == sizeof(off64_t), "fault harness requires 64-bit off_t");
ssize_t __wrap_pwrite64(int fd, const void *data, size_t n, off64_t offset) {
    return __wrap_pwrite(fd, data, n, (off_t)offset);
}
int __wrap_ftruncate64(int fd, off64_t size) {
    return __wrap_ftruncate(fd, (off_t)size);
}
int __wrap_fdatasync(int fd) {
    int active = target(fd);
    if (active && injected('D')) return -1;
    if (active && mode == 2 && !sync_interrupt++) { errno = EINTR; return -1; }
    int result = __real_fdatasync(fd);
    if (active && mode == 1 && !result) { snapshot(fd, stable_fd); event('D', fd); }
    return result;
}

static void new_value(char value[16385]) {
    for (int i = 0; i < 16384; i++) value[i] = (char)('a' + (i * 7) % 26);
    memcpy(value + 123, "COMMIT!!KVDBv001", 16);
    value[16384] = 0;
}
static int verify(struct kvdb_t *db, int must_new) {
    char value[16385], expected[16385]; new_value(expected);
    assert(kvdb_get(db, "anchor", value, sizeof(value)) == 16);
    assert(strcmp(value, "preserved-anchor") == 0);
    int n = kvdb_get(db, "shared", value, sizeof(value));
    int is_new = n == 16384 && strcmp(value, expected) == 0;
    int is_old = n == 9 && strcmp(value, "old-value") == 0;
    assert(is_new || is_old);
    if (must_new) assert(is_new);
    return is_new;
}

int main(int argc, char **argv) {
    assert(argc >= 3);
    struct kvdb_t db;
    assert(kvdb_open(&db, argv[2]) == 0);
    base_fd = db.fd; assert(fstat(base_fd, &identity) == 0);
    if (!strcmp(argv[1], "seed")) {
        assert(kvdb_put(&db, "anchor", "preserved-anchor") == 0);
        assert(kvdb_put(&db, "shared", "old-value") == 0);
    } else if (!strcmp(argv[1], "write")) {
        assert(argc == 5); cut = atoi(argv[4]);
        char path[4096];
        snprintf(path, sizeof(path), "%s/stable", argv[3]); stable_fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
        snprintf(path, sizeof(path), "%s/current", argv[3]); current_fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
        snprintf(path, sizeof(path), "%s/events", argv[3]); event_fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        assert(stable_fd >= 0 && current_fd >= 0 && event_fd >= 0);
        snapshot(base_fd, stable_fd);
        mode = 1;
        char value[16385]; new_value(value);
        assert(kvdb_put(&db, "shared", value) == 0);
        event('A', base_fd);
        mode = 0;
        snapshot(base_fd, current_fd);
        close(stable_fd); close(current_fd); close(event_fd);
        printf("events=%d\n", event_count);
    } else if (!strcmp(argv[1], "check")) {
        assert(argc == 4);
        puts(verify(&db, atoi(argv[3])) ? "new" : "old");
    } else if (!strcmp(argv[1], "loop")) {
        for (int i = 1; i <= 1000; i++) {
            char value[32]; snprintf(value, sizeof(value), "%d", i);
            assert(kvdb_put(&db, "counter", value) == 0);
            printf("%d\n", i); fflush(stdout);
        }
    } else if (!strcmp(argv[1], "counter")) {
        char value[32];
        assert(kvdb_get(&db, "counter", value, sizeof(value)) > 0);
        printf("%d\n", atoi(value));
    } else if (!strcmp(argv[1], "short")) {
        mode = 2;
        char value[16385]; new_value(value);
        assert(kvdb_put(&db, "shared", value) == 0);
        mode = 0;
        assert(write_interrupt && sync_interrupt && truncate_interrupt);
        assert(verify(&db, 1));
        puts("PASS: EINTR for truncate/write/sync and <=7-byte writes");
    } else if (!strcmp(argv[1], "error")) {
        assert(argc == 5); error_kind = argv[3][0]; error_number = atoi(argv[4]); mode = 3;
        char value[16385]; new_value(value);
        assert(kvdb_put(&db, "shared", value) == -1);
        mode = 0; assert(error_hit);
        verify(&db, 0);
        assert(kvdb_put(&db, "after-error", "recovered") == 0);
        assert(kvdb_get(&db, "after-error", value, sizeof(value)) == 9 && !strcmp(value, "recovered"));
        puts("PASS: injected I/O failure preserves atomic old/new value and subsequent writes");
    } else {
        assert(!"unknown operation");
    }
    assert(kvdb_close(&db) == 0);
    return 0;
}
