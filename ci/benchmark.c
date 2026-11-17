#define _GNU_SOURCE
#include <mymalloc.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
static atomic_size_t calls,unmaps,live,peak;
void *vmalloc(void *hint,size_t n) {
    assert(n && n%4096==0);
    void *p=mmap(hint,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED)return NULL;
    atomic_fetch_add(&calls,1);
    size_t x=atomic_fetch_add(&live,n)+n,old=atomic_load(&peak);
    while(old<x && !atomic_compare_exchange_weak(&peak,&old,x)){}
    return p;
}
void vmfree(void *p,size_t n) {assert(!munmap(p,n));atomic_fetch_add(&unmaps,1);atomic_fetch_sub(&live,n);}
struct job { unsigned iterations,batch; size_t size; };
static void *worker(void *arg) {
    struct job *j=arg;void *slots[256];assert(j->batch<=256);
    for(unsigned round=0;round<j->iterations;++round) {
        for(unsigned i=0;i<j->batch;++i) {
            slots[i]=mymalloc(j->size);assert(slots[i]);
            ((volatile unsigned char *)slots[i])[0]=(unsigned char)i;
        }
        for(unsigned i=0;i<j->batch;++i) {
            assert(((volatile unsigned char *)slots[i])[0]==(unsigned char)i);myfree(slots[i]);
        }
    }
    return NULL;
}
static double now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv) {
    assert(argc==5);
    unsigned threads=(unsigned)strtoul(argv[1],0,10);
    struct job j={(unsigned)strtoul(argv[4],0,10),(unsigned)strtoul(argv[3],0,10),(size_t)strtoul(argv[2],0,10)};
    assert(threads && threads<=16 && j.size && j.iterations && j.batch);
    pthread_t ids[16];double start=now();
    for(unsigned i=0;i<threads;++i)assert(!pthread_create(&ids[i],NULL,worker,&j));
    for(unsigned i=0;i<threads;++i)assert(!pthread_join(ids[i],NULL));
    double elapsed=now()-start;
    printf("threads,size,batch,iterations,allocations,seconds,maps,unmaps,peak_bytes,final_bytes\n");
    printf("%u,%zu,%u,%u,%zu,%.9f,%zu,%zu,%zu,%zu\n",threads,j.size,j.batch,j.iterations,(size_t)threads*j.batch*j.iterations,elapsed,atomic_load(&calls),atomic_load(&unmaps),atomic_load(&peak),atomic_load(&live));
    assert(!atomic_load(&live));
}
