#define _GNU_SOURCE
#include <mymalloc.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdbool.h>
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
struct interval { uintptr_t start; size_t size; };
static struct interval active[8*512];
static int compare_range(const void *a,const void *b) {
    uintptr_t x=((const struct interval *)a)->start,y=((const struct interval *)b)->start;
    return (x>y)-(x<y);
}
static void *worker(void *argument) {
    size_t id=(size_t)argument;
    for(unsigned round=0;round<20;++round) {
        for(unsigned i=0;i<512;++i) {
            size_t n=1+((i*131+round*53)%9000);
            unsigned char *p=mymalloc(n);assert(p && !((uintptr_t)p&7));
            memset(p,(int)(id+1),n);remote[id][i]=p;
        }
        pthread_barrier_wait(&barrier);
        if(id==0) {
            for(size_t t=0;t<8;++t)for(unsigned i=0;i<512;++i)
                active[t*512+i]=(struct interval){(uintptr_t)remote[t][i],1+((i*131+round*53)%9000)};
            qsort(active,8*512,sizeof active[0],compare_range);
            for(unsigned i=1;i<8*512;++i)assert(active[i-1].start+active[i-1].size<=active[i].start);
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
        myfree(p);assert(atomic_load(&mapped)<=4096);mymalloc_trim();assert(!atomic_load(&mapped));
    }
    void *small[10000];
    for(int i=0;i<10000;++i){small[i]=mymalloc(8);assert(small[i]);}
    assert(atomic_load(&mapped)<4*10000*8);
    for(int i=0;i<10000;++i)myfree(small[i]);
    assert(atomic_load(&mapped)<=4096);
    mymalloc_trim();assert(!atomic_load(&mapped));
    pthread_t threads[8];assert(!pthread_barrier_init(&barrier,NULL,8));
    for(size_t i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,worker,(void *)i));
    for(int i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    assert(atomic_load(&mapped)<=32*4096);
    mymalloc_trim();assert(!atomic_load(&mapped));pthread_barrier_destroy(&barrier);
    size_t requested=0;
    void **mixed=calloc(32768,sizeof *mixed);assert(mixed);
    for(unsigned i=0;i<32768;++i) {
        size_t n=1+(i*193u%4096u);requested+=n;
        mixed[i]=mymalloc(n);assert(mixed[i]);memset(mixed[i],(int)(i%251),n);
    }
    size_t dense=atomic_load(&mapped);assert(dense<=4*requested+32*4096);
    for(unsigned i=0;i<32768;++i) {
        size_t n=1+(i*193u%4096u);unsigned char *p=mixed[i];
        for(size_t j=0;j<n;++j)assert(p[j]==i%251);
        myfree(p);
    }
    mymalloc_trim();assert(!atomic_load(&mapped));
    for(unsigned i=0;i<32768;++i){mixed[i]=mymalloc(8);assert(mixed[i]);}
    uintptr_t pages[128];void *survivors[128];size_t kept=0;
    for(unsigned i=0;i<32768;++i) {
        uintptr_t page=(uintptr_t)mixed[i]&~(uintptr_t)4095;
        size_t j=0;while(j<kept && pages[j]!=page)++j;
        if(j==kept){assert(kept<128);pages[kept]=page;survivors[kept++]=mixed[i];}
        else myfree(mixed[i]);
    }
    size_t sparse=atomic_load(&mapped);
    /* Refill existing pages: sparse occupancy must not become dead capacity. */
    size_t refill=0;
    for(unsigned i=0;i<32768;++i) {
        bool is_survivor=false;
        for(size_t j=0;j<kept;++j)if(mixed[i]==survivors[j])is_survivor=true;
        if(!is_survivor){mixed[refill]=mymalloc(8);assert(mixed[refill]);++refill;}
    }
    assert(atomic_load(&mapped)<=sparse+4096);
    for(size_t i=0;i<refill;++i)myfree(mixed[i]);
    for(size_t i=0;i<kept;++i)myfree(survivors[i]);
    free(mixed);mymalloc_trim();assert(!atomic_load(&mapped));
    /* Long-lived mixed churn: validate contents before every release. */
    unsigned state=0x1622026u;size_t live_requested=0,max_live=0;
    for(unsigned i=0;i<32768;++i)mixed[i]=NULL;
    size_t *lengths=calloc(32768,sizeof *lengths);assert(lengths);
    for(unsigned step=0;step<200000;++step) {
        state=state*1664525u+1013904223u;unsigned index=state%32768;
        if(mixed[index]) {
            unsigned char *p=mixed[index];
            for(size_t j=0;j<lengths[index];++j)assert(p[j]==index%251);
            myfree(p);live_requested-=lengths[index];mixed[index]=NULL;
        } else {
            state=state*1664525u+1013904223u;size_t n=1+state%12000;
            mixed[index]=mymalloc(n);assert(mixed[index]);lengths[index]=n;
            memset(mixed[index],(int)(index%251),n);live_requested+=n;
        }
        if(live_requested>max_live)max_live=live_requested;
    }
    size_t churn_mapped=atomic_load(&mapped);
    for(unsigned i=0;i<32768;++i)if(mixed[i])myfree(mixed[i]);
    free(lengths);mymalloc_trim();assert(!atomic_load(&mapped));
    printf("CHURN steps=200000 final_live_requested=%zu mapped_before_cleanup=%zu max_requested=%zu final=0\n",live_requested,churn_mapped,max_live);
    printf("FRAGMENTATION dense_requested=%zu dense_mapped=%zu sparse_requested=%zu sparse_mapped=%zu refill=%zu\n",requested,dense,kept*8,sparse,refill);
    printf("PASS sizes=8193 small=10000 cross_thread_allocations=81920 peak=%zu final=0\n",atomic_load(&peak));
}
