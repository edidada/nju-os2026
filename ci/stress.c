#define _GNU_SOURCE
#include <mymalloc.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <stdlib.h>
static atomic_size_t mapped, peak;
static atomic_int fail_next;
void *vmalloc(void *hint,size_t n) {
    assert(n && n%4096==0);
    if (atomic_exchange(&fail_next,0)) return NULL;
    void *p=mmap(hint,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED)return NULL;
    size_t current=atomic_fetch_add(&mapped,n)+n,old=atomic_load(&peak);
    while(old<current && !atomic_compare_exchange_weak(&peak,&old,current)){}
    return p;
}
void vmfree(void *p,size_t n) {assert(!munmap(p,n));atomic_fetch_sub(&mapped,n);}
static void *remote[8][512];
static pthread_barrier_t barrier;
static void *worker(void *argument) {
    size_t id=(size_t)argument;
    for(unsigned round=0;round<20;++round) {
        for(unsigned i=0;i<512;++i) {
            size_t n=1+((i*131+round*53)%9000);
            unsigned char *p=mymalloc(n);assert(p && !((uintptr_t)p&7));
            memset(p,(int)(id+1),n);remote[id][i]=p;
        }
        pthread_barrier_wait(&barrier);
        size_t other=(id+1)%8;
        for(unsigned i=0;i<512;++i) {
            size_t n=1+((i*131+round*53)%9000);
            unsigned char *p=remote[other][i];
            for(size_t j=0;j<n;++j)assert(p[j]==other+1);
            myfree(p);
        }
        pthread_barrier_wait(&barrier);
    }
    return NULL;
}
int main(void) {
    assert(!mymalloc(0));myfree(NULL);assert(!mymalloc(SIZE_MAX));
    atomic_store(&fail_next,1);assert(!mymalloc(12));
    atomic_store(&fail_next,1);assert(!mymalloc(9000));
    for(size_t n=1;n<=8193;++n) {
        unsigned char *p=mymalloc(n);assert(p && !((uintptr_t)p&7));
        memset(p,0xa5,n);for(size_t j=0;j<n;++j)assert(p[j]==0xa5);
        myfree(p);assert(!atomic_load(&mapped));
    }
    void *small[10000];
    for(int i=0;i<10000;++i){small[i]=mymalloc(8);assert(small[i]);}
    assert(atomic_load(&mapped)<4*10000*8);
    for(int i=0;i<10000;++i)myfree(small[i]);
    assert(!atomic_load(&mapped));
    pthread_t threads[8];assert(!pthread_barrier_init(&barrier,NULL,8));
    for(size_t i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
    for(int i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    assert(!atomic_load(&mapped));pthread_barrier_destroy(&barrier);
    printf("PASS sizes=8193 small=10000 cross_thread_allocations=81920 peak=%zu final=0\n",atomic_load(&peak));
}
