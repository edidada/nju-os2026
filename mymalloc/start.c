#include <mymalloc.h>

#ifdef FREESTANDING

// This checks whether your code works for
// freestanding environments.

#define POOL_PAGES 128
static _Alignas(4096) unsigned char pool[POOL_PAGES * 4096];
static unsigned char occupied[POOL_PAGES];
static spinlock_t pool_lock;
void *vmalloc(void *addr, size_t length) {
    (void)addr;
    if (!length || length % 4096 || length / 4096 > POOL_PAGES) return NULL;
    size_t pages = length / 4096;
    spin_lock(&pool_lock);
    for (size_t i = 0; i + pages <= POOL_PAGES; ++i) {
        size_t j = 0;
        while (j < pages && !occupied[i + j]) ++j;
        if (j == pages) {
            for (j = 0; j < pages; ++j) occupied[i + j] = 1;
            spin_unlock(&pool_lock);
            return pool + i * 4096;
        }
        i += j;
    }
    spin_unlock(&pool_lock);
    return NULL;
}
void vmfree(void *addr, size_t length) {
    size_t first = ((uintptr_t)addr - (uintptr_t)pool) / 4096;
    spin_lock(&pool_lock);
    for (size_t i = 0; i < length / 4096; ++i) occupied[first + i] = 0;
    spin_unlock(&pool_lock);
}
void _start() {
}

int main() {
    return 0;
}

#else

#include <sys/mman.h>

void *vmalloc(void *addr, size_t length) {
    // length must be aligned to page size (4096).
    void *result = mmap(addr, length, PROT_READ | PROT_WRITE, 
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        return NULL;
    }
    return result;
}

void vmfree(void *addr, size_t length) {
    munmap(addr, length);
}

#endif
