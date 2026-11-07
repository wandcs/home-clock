/* Offline fault injection around the actual locked Hosted reader/port functions.
 * Hardware FIFO, RTOS waits, DMA allocation and downstream queue are modeled. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <inttypes.h>

typedef int esp_err_t;
typedef int eh_host_port_sem_t;
typedef struct { int unused; } sdmmc_card_t;
typedef struct { sdmmc_card_t *card; } sdmmc_context_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_STATE 0x103
#define DO_COMBINED_REG_READ 1
#define EH_HOST_PORT_SDIO_HOST_STREAMING_MODE 1
#define EH_HOST_PORT_SDIO_HOST_RX_MODE 1
#define EH_HOST_PORT_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE 2
#define EH_HOST_HOST_USES_STATIC_NETIF 0
#define EH_HOST_TRANSPORT_RESTART_ON_FAILURE 0
#define EH_HOST_PORT_WAIT_FOREVER UINT32_MAX
#define HOSTED_MEM_ALIGNMENT_64 64
#define MAX_TRANSPORT_BUFFER_SIZE 1536
#define ACQUIRE_LOCK false
#define SDIO_FUNC_1 1
#define ESP_OPEN_DATA_PATH 1
#define SDIO_INT_NEW_PACKET 0
#define SDIO_INT_START_THROTTLE 1
#define SDIO_INT_STOP_THROTTLE 2
#define EH_SET_BIT(x) (1u << (x))
#define EH_HOST_EVENT 1
#define EH_HOST_EVENT_TRANSPORT_FAILURE 2
#define INT_RAW_INDEX 0
#define PACKET_LEN_INDEX 4
#define REG_BUF_LEN 8
#define SDIO_RX_SLOT_WAIT_MS 20
#define SDIO_RX_SLOT_WAIT_RETRIES 2
#define unlikely(x) (x)
static const char TAG[]="sdio-host-test";
static unsigned shortage_warns;
static void host_log(const char *tag,const char *fmt,...) { (void)tag;(void)fmt; }
static void host_warn(const char *tag,const char *fmt,...) {
    (void)tag;
    if(strstr(fmt,"SDIO RX DMA growth")||strstr(fmt,"SDIO RX staging unavailable"))shortage_warns++;
}
#define ESP_LOGV(...) host_log(__VA_ARGS__)
#define ESP_LOGD(...) host_log(__VA_ARGS__)
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_warn(__VA_ARGS__)
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define SDIO_FAIL_IF_NULL(p) do { if (!(p)) return ESP_FAIL; } while (0)
#define SDIO_LOCK(required) ((void)(required))
#define SDIO_UNLOCK(required) ((void)(required))
#include "port-constants.inc"
#include "ring.inc"

static volatile uint8_t s_running;
static uint32_t sdio_rx_byte_count, sdio_rx_slot_drops;
static uint32_t reg_words[2];
static uint8_t *reg_buf = (uint8_t *)reg_words;
static int sem_data, sem_free;
static eh_host_port_sem_t *sem_double_buf_xfer_data=&sem_data, *sem_double_buf_free=&sem_free;
static sdmmc_card_t card;
static sdmmc_context_t context={.card=&card};
static void *sdio_handle=&context;
static bool irq, locked;
static uint32_t produced;
static uint8_t fifo[2*1024*1024], delivered[2*1024*1024];
static size_t fifo_end, fifo_at, delivered_n;
static unsigned sdk_calls, wait_calls, events, padding_bytes, groups;
static unsigned inject_after_call, fail_sdk_call, partial_failed_bytes;
static uint8_t future_data[1025];
static size_t future_n;
static jmp_buf reader_done, drain_done;
static bool in_drain, allow_one_drain;
struct allocation { void *p; size_t size; };
static struct allocation live[16];
static unsigned alloc_calls, free_calls, fail_next, fail_on_call, fail_through_call;
static bool fail_always, fail_growth;
static size_t peak_bytes, current_bytes;
static unsigned delay_ms;

static void host_lock(void) { assert(!locked);locked=true; }
static void host_unlock(void) { assert(locked);locked=false; }
#define SDIO_DRV_LOCK() host_lock()
#define SDIO_DRV_UNLOCK() host_unlock()
static void sdio_data_to_rx_buf_task(void *);
static void *eh_host_port_dma_alloc_aligned(size_t n,size_t alignment)
{
    alloc_calls++;
    if(fail_always || (fail_growth&&n>MAX_TRANSPORT_BUFFER_SIZE) ||
       (fail_on_call&&alloc_calls>=fail_on_call&&alloc_calls<=fail_through_call) || fail_next) {
        if(fail_next)fail_next--;
        return NULL;
    }
    void *p=NULL;assert(posix_memalign(&p,alignment,n)==0);
    for(unsigned i=0;i<16;i++)if(!live[i].p){
        live[i]=(struct allocation){p,n};
        current_bytes+=n;if(current_bytes>peak_bytes)peak_bytes=current_bytes;
        return p;
    }
    abort();
}
static void eh_host_port_dma_free(void *p)
{
    assert(p);
    for(unsigned i=0;i<16;i++)if(live[i].p==p){
        current_bytes-=live[i].size;live[i]=(struct allocation){0};free_calls++;free(p);return;
    }
    assert(!"double free or unknown DMA pointer");
}
static size_t allocation_size(void *p)
{
    for(unsigned i=0;i<16;i++)if(live[i].p==p)return live[i].size;
    assert(!"unowned pointer");return 0;
}
static void append(const uint8_t *p,size_t n)
{
    assert(fifo_end+n<=sizeof fifo);
    memcpy(fifo+fifo_end,p,n);fifo_end+=n;
    produced=(produced+(uint32_t)n)%ESP_RX_BYTE_MAX;irq=true;
}
static void append_pattern(size_t n,unsigned seed)
{
    assert(fifo_end+n<=sizeof fifo);
    for(size_t i=0;i<n;i++)fifo[fifo_end+i]=(uint8_t)(seed+i*17u);
    fifo_end+=n;produced=(produced+(uint32_t)n)%ESP_RX_BYTE_MAX;irq=true;
}
/* One real SDK CMD53 model. address determines logical consumption; transfer
 * padding must never consume bytes appended by a later co-processor send. */
static int read_wire(uint32_t addr,uint8_t *data,size_t transfer,bool block)
{
    assert(locked);
    assert(addr>=0x90 && addr<ESP_SLAVE_CMD53_END_ADDR);
    if(block)assert(transfer%ESP_BLOCK_SIZE==0);
    size_t request=ESP_SLAVE_CMD53_END_ADDR-addr;
    assert(transfer>=request);
    assert(request<=fifo_end-fifo_at);
    assert(allocation_size(data)>=transfer);
    sdk_calls++;
    if(sdk_calls==fail_sdk_call){
        size_t partial=partial_failed_bytes<request?partial_failed_bytes:request;
        fifo_at+=partial;return ESP_FAIL;
    }
    memcpy(data,fifo+fifo_at,request);
    memset(data+request,0,transfer-request);
    padding_bytes+=(unsigned)(transfer-request);
    fifo_at+=request;
    if(sdk_calls==inject_after_call)append(future_data,future_n);
    return ESP_OK;
}
static int sdmmc_io_read_blocks(sdmmc_card_t *c,uint32_t fn,uint32_t addr,uint8_t *data,size_t size)
{ assert(c==&card&&fn==1);return read_wire(addr,data,size,true); }
static int sdmmc_io_read_bytes(sdmmc_card_t *c,uint32_t fn,uint32_t addr,uint8_t *data,size_t size)
{ assert(c==&card&&fn==1);return read_wire(addr,data,size,false); }
static int sdmmc_io_read_byte(sdmmc_card_t *c,uint32_t fn,uint32_t addr,uint8_t *data)
{ return sdmmc_io_read_bytes(c,fn,addr,data,1); }
#include "port-functions.inc"

static void eh_host_port_task_delay_ms(unsigned ms)
{ assert(!locked);delay_ms+=ms; }
static int is_transport_rx_ready(void) { return 1; }
static int sdio_generate_slave_intr(unsigned what)
{ assert(what==ESP_OPEN_DATA_PATH);return ESP_OK; }
static void eh_host_wifi_tx_set_throttle(bool active) { (void)active; }
static int esp_event_post(int base,int id,const void *p,size_t n,unsigned wait)
{ assert(!locked&&base==EH_HOST_EVENT&&id==EH_HOST_EVENT_TRANSPORT_FAILURE&&!p&&!n&&!wait);events++;return ESP_OK; }
static int sdio_read_regs(uint8_t *out)
{ assert(locked);uint32_t words[]={irq?EH_SET_BIT(SDIO_INT_NEW_PACKET):0,produced};memcpy(out,words,sizeof words);return ESP_OK; }
static int sdio_clear_intr(uint32_t interrupts)
{ assert(locked&&interrupts==EH_SET_BIT(SDIO_INT_NEW_PACKET));irq=false;return ESP_OK; }
static int eh_host_port_sdio_wait_slave_intr(void *h,uint32_t ticks)
{
    assert(h==sdio_handle&&ticks==EH_HOST_PORT_WAIT_FOREVER&&!locked);
    wait_calls++;
    if(!irq)longjmp(reader_done,1); /* Represents the real forever wait. */
    return ESP_OK;
}
static int sdio_rx_push_data_to_queue(uint8_t *buf,uint32_t n)
{
    assert(!locked&&n<=allocation_size(buf));
    assert(delivered_n+n<=sizeof delivered);memcpy(delivered+delivered_n,buf,n);delivered_n+=n;
    return ESP_OK;
}
static int eh_host_port_sem_wait_ms(eh_host_port_sem_t *sem,unsigned timeout)
{
    (void)timeout;assert(!locked);
    if(in_drain&&sem==sem_double_buf_xfer_data){
        if(allow_one_drain){allow_one_drain=false;return ESP_OK;}
        longjmp(drain_done,1);
    }
    return ESP_ERR_TIMEOUT;
}
static int eh_host_port_sem_post(eh_host_port_sem_t *sem)
{
    assert(!locked);
    if(sem==sem_double_buf_xfer_data){
        assert(!in_drain);in_drain=true;allow_one_drain=true;
        if(setjmp(drain_done)==0)sdio_data_to_rx_buf_task(NULL);
        in_drain=false;
    }
    return ESP_OK;
}
#include "rx-functions.inc"
static void release_buffers(void) {
#include "teardown.inc"
}
static void run_reader(void)
{
    s_running=1;
    if(setjmp(reader_done)==0)sdio_read_task(NULL);
    assert(!locked&&!in_drain);
}
static void reset(void)
{
    release_buffers();assert(!current_bytes);
    memset(&double_buf,0,sizeof double_buf);
    memset(live,0,sizeof live);
    fifo_at=fifo_end=delivered_n=0;produced=sdio_rx_byte_count=0;
    sdk_calls=wait_calls=events=padding_bytes=0;
    inject_after_call=fail_sdk_call=partial_failed_bytes=0;
    alloc_calls=free_calls=fail_next=fail_on_call=fail_through_call=0;
    fail_always=fail_growth=irq=locked=false;
    peak_bytes=current_bytes=delay_ms=shortage_warns=0;s_running=1;
#ifndef HC_BASELINE
    hc_sdio_rx_oom_reported=false;
#endif
}
static void verify_slice(size_t at,size_t n)
{ assert(delivered_n==n);assert(!memcmp(delivered,fifo+at,n)); }
int main(void)
{
    (void)groups;
#ifdef HC_BASELINE
    reset();append_pattern(5003,1);fail_next=1;run_reader();
    assert(wait_calls==2&&!irq&&fifo_at==0&&sdio_rx_byte_count==0&&!delivered_n&&!events);
    release_buffers();assert(!current_bytes);
    puts("{\"baseline_stalls_after_one_dma_failure\":true,\"irq_cleared\":true,\"pending_bytes\":5003,\"actual_reader\":true}");
#else
    /* Early single failure retries; a later reserve failure rolls back all
     * earlier slots. The wrapper returns init failure before other resources. */
    reset();fail_next=1;assert(hc_sdio_rx_reserve()==0);assert(alloc_calls==3&&current_bytes==3072);
    release_buffers();assert(!current_bytes);release_buffers();
    reset();fail_on_call=2;fail_through_call=3;
    assert(hc_sdio_rx_reserve()!=0&&!current_bytes&&free_calls==1&&alloc_calls==3);
    assert(!double_buf.buffer[0].buf&&!double_buf.buffer[1].buf);release_buffers();
    reset();fail_always=true;assert(hc_sdio_rx_reserve()!=0&&!current_bytes);++groups;

    /* No run-time allocation is needed for ordinary frames, even under OOM. */
    reset();assert(hc_sdio_rx_reserve()==0);fail_always=true;
    append_pattern(1025,2);run_reader();verify_slice(0,1025);
    append_pattern(1001,3);run_reader();assert(delivered_n==2026&&!memcmp(delivered+1025,fifo+1025,1001));
    assert(alloc_calls==2&&fifo_at==2026&&sdio_rx_byte_count==2026&&!events);++groups;

    /* The exact pending window retries without waiting for a second IRQ. */
    reset();assert(hc_sdio_rx_reserve()==0);append_pattern(5003,4);fail_next=1;run_reader();
    verify_slice(0,5003);assert(fifo_at==5003&&sdio_rx_byte_count==5003&&!events&&delay_ms>=120);
    assert(double_buf.buffer[0].buf_size==5120);++groups;

    /* Persistent growth OOM discards the snapshotted stream, not later bytes.
     * Inject a new IRQ/data midway through discard; final 395B request is
     * padded to 512B but consumes only 395B. The next request remains intact. */
    reset();assert(hc_sdio_rx_reserve()==0);fail_growth=true;
    const uint32_t start_count=ESP_RX_BYTE_MAX-1234;
    produced=sdio_rx_byte_count=start_count;
    append_pattern(5003,5);for(unsigned i=0;i<sizeof future_data;i++)future_data[i]=(uint8_t)(i^0xa5u);
    future_n=sizeof future_data;inject_after_call=2;run_reader();verify_slice(5003,future_n);
    assert(fifo_at==5003+future_n&&sdio_rx_byte_count==(start_count+fifo_at)%ESP_RX_BYTE_MAX&&!events);
    assert(double_buf.count==0&&padding_bytes==(512-395)+(1536-1025));
    append_pattern(1537,9);run_reader();assert(delivered_n==future_n&&shortage_warns==1);
    fail_growth=false;const size_t next_at=fifo_end;append_pattern(5003,10);run_reader();
    assert(delivered_n==future_n+5003&&!memcmp(delivered+future_n,fifo+next_at,5003));
    assert(fifo_at==fifo_end&&sdio_rx_byte_count==(start_count+fifo_at)%ESP_RX_BYTE_MAX&&shortage_warns==1);++groups;

    /* The port's uint16+padding bound is a policy limit, not a slave maximum.
     * Larger valid PKT_LEN is safely consumed in small real FIFO transfers. */
    reset();assert(hc_sdio_rx_reserve()==0);append_pattern(HC_SDIO_RX_LINEAR_MAX+1,6);run_reader();
    assert(fifo_at==HC_SDIO_RX_LINEAR_MAX+1&&sdio_rx_byte_count==fifo_at&&!delivered_n&&!events&&alloc_calls==2);
    append_pattern(1025,7);run_reader();verify_slice(HC_SDIO_RX_LINEAR_MAX+1,1025);++groups;

    /* A failed DMA growth never frees the head reserve or the consumer's
     * pending slot. Actual source teardown frees each allocation only once. */
    reset();assert(hc_sdio_rx_reserve()==0);void *tail=double_buf.buffer[0].buf,*head=double_buf.buffer[1].buf;
    memset(tail,0x5a,1536);double_buf.head=1;double_buf.tail=0;double_buf.count=1;
    fail_growth=true;assert(!sdio_rx_get_buffer(3073));
    assert(double_buf.buffer[0].buf==tail&&double_buf.buffer[1].buf==head&&((uint8_t*)tail)[1535]==0x5a);
    release_buffers();assert(!current_bytes);release_buffers();assert(free_calls==2);++groups;

    /* Partial lower-level error: only prior successful logical chunks count.
     * An error may have consumed unknown bytes. Emit failure; do not lie about
     * alignment or successful delivery, and never count the failed chunk. */
    reset();assert(hc_sdio_rx_reserve()==0);append_pattern(5003,8);fail_growth=true;
    fail_sdk_call=2;partial_failed_bytes=128;run_reader();
    assert(fifo_at==1536+128&&sdio_rx_byte_count==1536&&events==1&&!delivered_n&&sdk_calls==2);++groups;

    release_buffers();assert(!current_bytes);release_buffers();
    printf("{\"passed\":true,\"groups\":%u,\"actual_reader_and_port\":true,\"real_hardware\":false,\"reserved_bytes\":3072,\"linear_limit\":%u}\n",groups,(unsigned)HC_SDIO_RX_LINEAR_MAX);
#endif
    return 0;
}
