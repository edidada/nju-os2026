#define _start pool_unused_start
#define main pool_unused_main
#include "../mymalloc/start.c"
#undef _start
#undef main
static void *slots[8192];
static _Noreturn void finish(int code) {
    __asm__ volatile("syscall" : : "a"(60L), "D"((long)code) : "rcx", "r11", "memory");
    __builtin_unreachable();
}
#define CHECK(x) do { if (!(x)) finish(__LINE__); } while (0)
static size_t exhaust(size_t bytes) {
    size_t count=0;
    while(count<8192) {
        unsigned char *p=mymalloc(bytes);
        if(!p)break;
        CHECK(!((uintptr_t)p&7));slots[count++]=p;
        for(size_t j=0;j<bytes;++j)p[j]=(unsigned char)(count%251);
    }
    CHECK(count>0 && count<8192);
    CHECK(!mymalloc(bytes));
    for(size_t i=0;i<count;++i) {
        unsigned char *p=slots[i];
        for(size_t j=0;j<bytes;++j)CHECK(p[j]==(i+1)%251);
    }
    return count;
}
static void release_all(size_t count) {
    for(size_t i=0;i<count;++i)myfree(slots[i]);
    mymalloc_trim();
    for(size_t i=0;i<POOL_PAGES;++i)CHECK(!occupied[i]);
}
__attribute__((used,noinline)) static void run(void) {
    size_t sizes[]={8,64,1024,2048,9000};
    /* 8-byte objects exceed the small static handle array, use 64 onward. */
    for(size_t s=1;s<sizeof sizes/sizeof sizes[0];++s) {
        size_t count=exhaust(sizes[s]);release_all(count);
        size_t again=exhaust(sizes[s]);CHECK(again==count);release_all(again);
    }
    CHECK(!mymalloc(SIZE_MAX));myfree(NULL);
    finish(0);
}
/* Linux ELF entry has no caller return address: realign before entering C. */
__attribute__((naked,noreturn)) void _start(void) {
    __asm__ volatile("andq $-16, %rsp\ncall run\nud2");
}
