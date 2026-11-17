#ifndef KVDB_H
#define KVDB_H
#include <stddef.h>
#include <stdint.h>

struct kvdb_t {
    char *path;
    int fd;
    uint64_t cookie;
};

int kvdb_open(struct kvdb_t *db, const char *path);
int kvdb_put(struct kvdb_t *db, const char *key, const char *value);
int kvdb_get(struct kvdb_t *db, const char *key, char *buf, size_t length);
int kvdb_close(struct kvdb_t *db);
#endif
