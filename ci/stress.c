#define _GNU_SOURCE
#include "kvdb.h"
#include <assert.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct kvdb_t shared;
static const char *path;
struct Argument { int id, separate; };

static void *worker(void *pointer) {
    struct Argument *arg = pointer;
    struct kvdb_t own;
    struct kvdb_t *db = &shared;
    if (arg->separate) { assert(kvdb_open(&own, path) == 0); db = &own; }
    for (int i = 0; i < 40; i++) {
        char key[80], value[160], got[160];
        snprintf(key, sizeof(key), "thread-%d-key-%d", arg->id, i);
        snprintf(value, sizeof(value), "value-%d-%d-%d", arg->id, i, arg->id * 1000 + i);
        assert(kvdb_put(db, key, value) == 0);
        assert(kvdb_get(db, key, got, sizeof(got)) == (int)strlen(value));
        assert(strcmp(got, value) == 0);
        assert(kvdb_put(db, "contended", value) == 0);
        int n = kvdb_get(db, "contended", got, sizeof(got));
        int id, round, checksum;
        assert(n > 0 && sscanf(got, "value-%d-%d-%d", &id, &round, &checksum) == 3);
        assert(checksum == id * 1000 + round);
    }
    if (arg->separate) assert(kvdb_close(&own) == 0);
    return NULL;
}

static void threads(int base, int count, int separate) {
    pthread_t ids[8]; struct Argument args[8];
    for (int i = 0; i < count; i++) {
        args[i] = (struct Argument){base + i, separate};
        assert(pthread_create(&ids[i], NULL, worker, &args[i]) == 0);
    }
    for (int i = 0; i < count; i++) assert(pthread_join(ids[i], NULL) == 0);
}

int main(int argc, char **argv) {
    assert(argc == 2); path = argv[1];
    assert(kvdb_open(&shared, path) == 0);
    assert(kvdb_get(&shared, "absent", NULL, 0) == -1);
    assert(kvdb_put(&shared, "", "") == 0);
    char tiny[5] = "xxxx";
    assert(kvdb_get(&shared, "", tiny, sizeof(tiny)) == 0 && tiny[0] == 0);
    assert(kvdb_put(&shared, "a", "first") == 0);
    assert(kvdb_put(&shared, "a", "latest-long") == 0);
    assert(kvdb_get(&shared, "a", NULL, 0) == 0);
    assert(kvdb_get(&shared, "a", tiny, 1) == 0 && tiny[0] == 0);
    assert(kvdb_get(&shared, "a", tiny, sizeof(tiny)) == 4 && strcmp(tiny, "late") == 0);
    char *large = malloc(65538), *got = malloc(65538);
    assert(large && got);
    for (int i = 0; i < 65537; i++) large[i] = (char)('a' + i % 26);
    large[65537] = 0;
    assert(kvdb_put(&shared, "large", large) == 0);
    assert(kvdb_get(&shared, "large", got, 65538) == 65537 && strcmp(got, large) == 0);
    threads(0, 8, 0);
    threads(10, 4, 1);
    // All children inherit the same open handle, but must have independent
    // file-lock ownership. Each child also creates two concurrent threads.
    pid_t children[4];
    for (int i = 0; i < 4; i++) {
        children[i] = fork(); assert(children[i] >= 0);
        if (!children[i]) { threads(100 + i * 10, 2, i % 2); assert(kvdb_close(&shared) == 0); exit(0); }
    }
    threads(200, 2, 0);
    for (int i = 0; i < 4; i++) { int status; assert(waitpid(children[i], &status, 0) == children[i]); assert(WIFEXITED(status) && WEXITSTATUS(status) == 0); }
    // A fresh handle observes each process/thread's completed unique writes.
    struct kvdb_t reader; assert(kvdb_open(&reader, path) == 0);
    int bases[] = {0,10,100,110,120,130,200};
    int counts[] = {8,4,2,2,2,2,2};
    for (int b = 0; b < 7; b++) for (int t = 0; t < counts[b]; t++) {
        char key[80], value[160], expected[160]; int id = bases[b] + t;
        snprintf(key, sizeof(key), "thread-%d-key-39", id);
        snprintf(expected, sizeof(expected), "value-%d-39-%d", id, id * 1000 + 39);
        assert(kvdb_get(&reader, key, value, sizeof(value)) == (int)strlen(expected));
        assert(strcmp(value, expected) == 0);
    }
    assert(kvdb_get(&reader, "large", got, 65538) == 65537 && strcmp(got, large) == 0);
    assert(kvdb_close(&reader) == 0); assert(kvdb_close(&shared) == 0);
    assert(kvdb_close(&shared) == -1);
    free(large); free(got);
    puts("PASS: empty/truncated/65537-byte values; 22 thread workers across parent/four fork children; shared/separate/inherited handles; fresh-handle visibility");
    return 0;
}
