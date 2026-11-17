#define _GNU_SOURCE
#include <mymalloc.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static atomic_size_t mapped, denied;
static atomic_int deny_mapping;
void *vmalloc(void *hint,size_t n) {
    if(atomic_load(&deny_mapping)){atomic_fetch_add(&denied,1);return NULL;}
    void *p=mmap(hint,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED)return NULL;
    atomic_fetch_add(&mapped,n);return p;
}
void vmfree(void *p,size_t n){assert(!munmap(p,n));atomic_fetch_sub(&mapped,n);}
static unsigned producer;
static int ran,scenario;
static size_t recovered;
static void *objects[128];
static void *consumer(void *unused) {
    (void)unused;
    if((unsigned)syscall(SYS_gettid)%32==producer)return NULL;
    ran=1;
    if(!scenario) {
        while(recovered<128) {
            void *p=mymalloc(1024);if(!p)break;
            memset(p,0x69,1024);objects[recovered++]=p;
        }
        assert(recovered<128 && !mymalloc(1024));
    } else {
        objects[0]=mymalloc(2048);assert(objects[0]);
        memset(objects[0],0x69,2048);recovered=1;
    }
    return NULL;
}
static void run_consumer(void) {
    ran=0;recovered=0;atomic_store(&deny_mapping,1);
    for(unsigned attempt=0;!ran && attempt<64;++attempt) {
        pthread_t thread;assert(!pthread_create(&thread,NULL,consumer,NULL));
        assert(!pthread_join(thread,NULL));
    }
    assert(ran);atomic_store(&deny_mapping,0);
}
int main(void) {
    producer=(unsigned)syscall(SYS_gettid)%32;
    void *live[96];uintptr_t previous=0;size_t holes=0;
    for(size_t i=0;i<96;++i) {
        live[i]=mymalloc(1024);assert(live[i]);memset(live[i],0x35,1024);
    }
    /* Keep every producer page live, with one available slot per page. */
    for(size_t i=0;i<96;++i) {
        uintptr_t page=(uintptr_t)live[i]&~(uintptr_t)4095;
        if(page!=previous){previous=page;myfree(live[i]);live[i]=NULL;++holes;}
    }
    size_t before=atomic_load(&mapped);run_consumer();
    assert(recovered==holes && atomic_load(&mapped)==before);
    for(size_t i=0;i<recovered;++i) {
        for(size_t j=0;j<1024;++j)assert(((unsigned char *)objects[i])[j]==0x69);
        for(size_t k=0;k<i;++k)assert(objects[i]!=objects[k]);
        myfree(objects[i]);
    }
    for(size_t i=0;i<96;++i)if(live[i]) {
        for(size_t j=0;j<1024;++j)assert(((unsigned char *)live[i])[j]==0x35);
        myfree(live[i]);
    }
    mymalloc_trim();assert(!atomic_load(&mapped));
    scenario=1;void *p=mymalloc(8);assert(p);myfree(p);
    assert(atomic_load(&mapped)==4096);run_consumer();
    assert(recovered==1 && atomic_load(&mapped)==4096);
    for(size_t j=0;j<2048;++j)assert(((unsigned char *)objects[0])[j]==0x69);
    myfree(objects[0]);mymalloc_trim();assert(!atomic_load(&mapped));
    assert(atomic_load(&denied)>0);
    printf("PASS: mapping denied; recovered %zu cross-shard live-page slots and one idle page across size classes; no growth, content preserved, final=0\n",holes);
}
