#include <mymalloc.h>

#define PAGE 4096u
#define SHARDS 32u
#define CLASSES 9u
#define LARGE CLASSES
struct page {
    struct page *next, *previous;
    void *free;
    size_t length;
    unsigned owner, class_id, used, capacity;
};
#define HEADER ((sizeof(struct page) + 7u) & ~(size_t)7u)
struct bucket { spinlock_t lock; struct page *available; };
static struct bucket buckets[SHARDS][CLASSES];
struct reserve { spinlock_t lock; void *page; };
static struct reserve reserves[SHARDS];
static void *take_page(unsigned owner) {
    struct reserve *r = &reserves[owner];
    spin_lock(&r->lock);
    void *page = r->page; r->page = 0;
    spin_unlock(&r->lock);
    return page ? page : vmalloc(0, PAGE);
}
static void return_page(unsigned owner, void *page) {
    struct reserve *r = &reserves[owner];
    spin_lock(&r->lock);
    if (!r->page) { r->page = page; page = 0; }
    spin_unlock(&r->lock);
    if (page) vmfree(page, PAGE);
}
/* Optional cache reclamation; active allocations are never touched. */
void mymalloc_trim(void) {
    for (unsigned i = 0; i < SHARDS; ++i) {
        struct reserve *r = &reserves[i];
        spin_lock(&r->lock);
        void *page = r->page; r->page = 0;
        spin_unlock(&r->lock);
        if (page) vmfree(page, PAGE);
    }
}
#ifdef M5_TEST_COUNT
long malloc_count;
#endif
/* No libc/TLS dependency: Linux gettid is explicitly permitted by the task. */
static unsigned shard(void) {
    unsigned long id;
#if defined(__x86_64__)
    __asm__ volatile("syscall" : "=a"(id) : "0"(186UL) : "rcx", "r11", "memory");
#elif defined(__i386__)
    __asm__ volatile("int $0x80" : "=a"(id) : "0"(224UL) : "memory");
#else
#error Unsupported architecture for freestanding gettid
#endif
    return (unsigned)(id % SHARDS);
}
static void insert(struct bucket *b, struct page *p) {
    p->previous = 0; p->next = b->available;
    if (p->next) p->next->previous = p;
    b->available = p;
}
static void detach(struct bucket *b, struct page *p) {
    if (p->previous) p->previous->next = p->next;
    else b->available = p->next;
    if (p->next) p->next->previous = p->previous;
    p->next = p->previous = 0;
}
void *mymalloc(size_t size) {
#ifdef M5_TEST_COUNT
    __atomic_fetch_add(&malloc_count, 1, __ATOMIC_RELAXED);
#endif
    if (!size) return 0;
    if (size > 2048) {
        if (size > (size_t)-1 - HEADER - (PAGE - 1)) return 0;
        size_t length = (size + HEADER + PAGE - 1) & ~(size_t)(PAGE - 1);
        struct page *p = vmalloc(0, length);
        if (!p) return 0;
        p->length = length; p->class_id = LARGE;
        return (char *)p + HEADER;
    }
    unsigned c = 0, owner = shard(); size_t slot = 8;
    while (slot < size) { slot *= 2; ++c; }
    struct bucket *b = &buckets[owner][c];
    spin_lock(&b->lock);
    struct page *p = b->available;
    if (!p) {
        /* Mapping and initialization do not hold a shared allocation lock. */
        spin_unlock(&b->lock);
        p = take_page(owner);
        if (!p) return 0;
        p->length = PAGE; p->owner = owner; p->class_id = c; p->used = 0;
        p->capacity = (unsigned)((PAGE - HEADER) / slot);
        p->free = 0;
        for (unsigned i = p->capacity; i > 0; --i) {
            void *object = (char *)p + HEADER + (i - 1) * slot;
            *(void **)object = p->free; p->free = object;
        }
        spin_lock(&b->lock);
        insert(b, p);
    }
    void *result = p->free;
    p->free = *(void **)result; ++p->used;
    if (!p->free) detach(b, p);
    spin_unlock(&b->lock);
    return result;
}
void myfree(void *ptr) {
    if (!ptr) return;
    struct page *p = (struct page *)((uintptr_t)ptr & ~(uintptr_t)(PAGE - 1));
    if (p->class_id == LARGE) { vmfree(p, p->length); return; }
    struct bucket *b = &buckets[p->owner][p->class_id];
    spin_lock(&b->lock);
    if (p->used == p->capacity) insert(b, p);
    *(void **)ptr = p->free; p->free = ptr; --p->used;
    if (!p->used) {
        detach(b, p);
        spin_unlock(&b->lock);
        /* No live allocation and no bucket reference remains. */
        return_page(p->owner, p);
    } else spin_unlock(&b->lock);
}
