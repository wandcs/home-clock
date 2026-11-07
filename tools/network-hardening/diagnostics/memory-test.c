#define _POSIX_C_SOURCE 200809L
#include "network_memory.h"
#include "esp_heap_caps.h"
#include "platform.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static atomic_uint checks,failures;
#define CHECK(value) do{atomic_fetch_add(&checks,1);if(!(value)){atomic_fetch_add(&failures,1);fprintf(stderr,"check failed at line %u: %s\n",__LINE__,#value);}}while(0)
static void startup(void){
    CHECK(!test_hook_registered());
    CHECK(heap_caps_register_failed_alloc_callback(NULL)==ESP_ERR_INVALID_ARG);
    CHECK(!test_hook_registered());
    CHECK(network_memory_start()==ESP_OK);CHECK(test_hook_registered());
    network_memory_snapshot_t s=network_memory_read();
    CHECK(s.failures==0&&s.first_us==0&&s.first_size==0&&s.first_caps==0);
}
static void filtering(void){
    test_set_time_us(1000000);
    test_emit_failed_alloc(512,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT,"external");
    test_emit_failed_alloc(256,MALLOC_CAP_DEFAULT|MALLOC_CAP_8BIT,"default");
    test_emit_failed_alloc(128,0,NULL);
    CHECK(network_memory_read().failures==0);
    test_set_time_us(12345000);
    test_emit_failed_alloc(12288,MALLOC_CAP_DMA|MALLOC_CAP_8BIT,NULL);
    network_memory_snapshot_t first=network_memory_read();
    CHECK(first.failures==1&&first.first_size==12288);
    CHECK(first.first_caps==(MALLOC_CAP_DMA|MALLOC_CAP_8BIT)&&first.first_us==12345000);
    test_set_time_us(20000000);
    test_emit_failed_alloc(4096,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT,(const char *)1);
    test_emit_failed_alloc(8192,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA,"later");
    network_memory_snapshot_t last=network_memory_read();
    CHECK(last.failures==3&&last.first_size==first.first_size);
    CHECK(last.first_caps==first.first_caps&&last.first_us==first.first_us);
    test_emit_failed_alloc(1024,MALLOC_CAP_SPIRAM,"ignored after first");
    CHECK(network_memory_read().failures==3);
}
enum { WRITERS=4, ITERATIONS=2000 };
static pthread_barrier_t barrier;
static atomic_uint finished;
static unsigned reader_count;
static void *writer(void *arg){
    unsigned id=(unsigned)(uintptr_t)arg,number=id+1;
    uint32_t caps=id%2?MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT:MALLOC_CAP_DMA|MALLOC_CAP_8BIT;
    test_set_time_us((int64_t)number*1000000+321000);
    pthread_barrier_wait(&barrier);
    for(unsigned n=0;n<ITERATIONS;n++)test_emit_failed_alloc(number*4096+128,caps,(const char *)1);
    atomic_fetch_add(&finished,1);return NULL;
}
static void *reader(void *arg){
    (void)arg;uint32_t previous=0;network_memory_snapshot_t first={0};
    pthread_barrier_wait(&barrier);
    do{
        network_memory_snapshot_t s=network_memory_read();reader_count++;
        CHECK(s.failures>=previous);previous=s.failures;
        if(s.failures){
            unsigned number=(unsigned)((s.first_size-128)/4096);
            CHECK(number>=1&&number<=WRITERS&&s.first_size==number*4096+128);
            CHECK(s.first_us==(uint64_t)number*1000000+321000);
            CHECK(s.first_caps==((number-1)%2?MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT:MALLOC_CAP_DMA|MALLOC_CAP_8BIT));
            if(!first.failures)first=s;
            CHECK(s.first_size==first.first_size&&s.first_caps==first.first_caps&&s.first_us==first.first_us);
        }else CHECK(s.first_size==0&&s.first_caps==0&&s.first_us==0);
        if(!(reader_count%16))sched_yield();
    }while(atomic_load(&finished)<WRITERS);
    return NULL;
}
static void concurrent(void){
    pthread_t writers[WRITERS],watcher;
    CHECK(!pthread_barrier_init(&barrier,NULL,WRITERS+2));
    CHECK(!pthread_create(&watcher,NULL,reader,NULL));
    for(unsigned n=0;n<WRITERS;n++)CHECK(!pthread_create(&writers[n],NULL,writer,(void *)(uintptr_t)n));
    pthread_barrier_wait(&barrier);
    for(unsigned n=0;n<WRITERS;n++)CHECK(!pthread_join(writers[n],NULL));
    CHECK(!pthread_join(watcher,NULL));CHECK(!pthread_barrier_destroy(&barrier));
    CHECK(reader_count>0&&network_memory_read().failures==WRITERS*ITERATIONS);
}
int main(int argc,char **argv){
    if(argc!=2)return 2;
    startup();
    if(!strcmp(argv[1],"filter"))filtering();
    else if(!strcmp(argv[1],"concurrent"))concurrent();else return 2;
    printf("{\"passed\":%s,\"case\":\"%s\",\"checks\":%u,\"failures\":%u,\"matched_failures\":%u,\"reader_snapshots\":%u}\n",atomic_load(&failures)?"false":"true",argv[1],atomic_load(&checks),atomic_load(&failures),network_memory_read().failures,reader_count);
    return atomic_load(&failures)?1:0;
}
