#include "weather_http_guard.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

static SemaphoreHandle_t lifetime, dns_gate;
static portMUX_TYPE metadata = portMUX_INITIALIZER_UNLOCKED;
static weather_http_progress_t progress;
static uint32_t sequence;
static bool stack_sampled;
static int socket_fd = -1;
/* Static storage is not reused until the queued callback finishes. */
static struct { uint32_t attempt; bool outstanding; } dns_callback;
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time()/1000; }
weather_http_progress_t weather_http_guard_read(void) {
    portENTER_CRITICAL(&metadata);
    weather_http_progress_t copy = progress;
    portEXIT_CRITICAL(&metadata);
    return copy;
}
void weather_http_guard_request_recovery(void) {
    uint64_t now = now_ms();
    portENTER_CRITICAL(&metadata);
    if(progress.active && now >= progress.deadline_ms)
        progress.recovery_requested = true;
    portEXIT_CRITICAL(&metadata);
}
void weather_http_guard_phase(uint32_t attempt, weather_http_phase_t phase) {
    xSemaphoreTake(dns_gate, portMAX_DELAY);
    portENTER_CRITICAL(&metadata);
    if(progress.active && progress.attempt_id == attempt) {
        progress.phase = phase; progress.progress_ms = now_ms();
    }
    portEXIT_CRITICAL(&metadata);
    xSemaphoreGive(dns_gate);
}
void weather_http_guard_connected(uint32_t attempt, int fd) {
    /* Only from synchronous ON_CONNECTED. CONNECTING/TLS can destroy an fd
     * internally, without an HTTP DISCONNECTED notification. */
    weather_http_guard_phase(attempt, WHG_HEADERS);
    xSemaphoreTake(lifetime, portMAX_DELAY);
    portENTER_CRITICAL(&metadata);
    if(progress.active && progress.attempt_id == attempt && fd >= 0) {
        socket_fd = fd; progress.fd_present = true;
    }
    portEXIT_CRITICAL(&metadata);
    xSemaphoreGive(lifetime);
}
void weather_http_guard_detach(uint32_t attempt) {
    /* The locked IDF dispatches DISCONNECTED synchronously BEFORE actual close.
     * Holding this same mutex across shutdown prevents fd reuse/ABA. */
    xSemaphoreTake(lifetime, portMAX_DELAY);
    portENTER_CRITICAL(&metadata);
    if(progress.attempt_id == attempt) { socket_fd = -1; progress.fd_present = false; }
    portEXIT_CRITICAL(&metadata);
    xSemaphoreGive(lifetime);
}
uint32_t weather_http_guard_begin(weather_http_owner_t owner, uint32_t generation,
                                  uint32_t budget_ms) {
    configASSERT(lifetime && dns_gate);
    /* Never hold the TCPIP callback gate while waiting for shutdown. */
    xSemaphoreTake(lifetime, portMAX_DELAY);
    xSemaphoreTake(dns_gate, portMAX_DELAY);
    uint64_t now = now_ms();
    portENTER_CRITICAL(&metadata);
    configASSERT(!progress.active);
    if(!++sequence) ++sequence;
    uint32_t aborts=progress.socket_aborts, resets=progress.dns_resets;
    uint32_t stale=progress.stale_callbacks;
    uint32_t stack_min=progress.guard_task_stack_min_bytes;
    progress=(weather_http_progress_t){.attempt_id=sequence,.generation=generation,
        .owner=owner,.phase=WHG_CONNECT,.started_ms=now,.progress_ms=now,
        .deadline_ms=now+budget_ms,.active=true,.socket_aborts=aborts,
        .dns_resets=resets,.stale_callbacks=stale,
        .guard_task_stack_min_bytes=stack_min};
    socket_fd=-1;
    portEXIT_CRITICAL(&metadata);
    xSemaphoreGive(dns_gate); xSemaphoreGive(lifetime);
    return sequence;
}
void weather_http_guard_end(uint32_t attempt) {
    /* Only AFTER owner cleanup. DNS admission is separate from fd lifetime. */
    xSemaphoreTake(lifetime, portMAX_DELAY);
    xSemaphoreTake(dns_gate, portMAX_DELAY);
    portENTER_CRITICAL(&metadata);
    if(progress.attempt_id == attempt) {
        socket_fd=-1; progress.fd_present=false;
        progress.active=false; progress.phase=WHG_DONE;
        progress.finished_ms=progress.progress_ms=now_ms();
    }
    portEXIT_CRITICAL(&metadata);
    xSemaphoreGive(dns_gate);
    xSemaphoreGive(lifetime);
}
bool weather_http_guard_expired(uint32_t attempt) {
    weather_http_progress_t p=weather_http_guard_read();
    return p.attempt_id==attempt && (p.interrupted || now_ms()>=p.deadline_ms);
}
bool weather_http_guard_dns(uint32_t attempt, const char *host, int *error) {
    weather_http_guard_phase(attempt, WHG_DNS);
    struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM}, *address=NULL;
    int rc=getaddrinfo(host,NULL,&hints,&address);
    if(address) freeaddrinfo(address);
    *error=rc;
    weather_http_guard_phase(attempt, WHG_CONNECT);
    return !rc && !weather_http_guard_expired(attempt);
}
static void reset_dns(void *unused) {
    (void)unused;
    portENTER_CRITICAL(&metadata);
    uint32_t attempt=dns_callback.attempt;
    portEXIT_CRITICAL(&metadata);
    bool cleared=false;
    /* Never wait in TCPIP or take lifetime: shutdown may wait for this core. */
    if(xSemaphoreTake(dns_gate,0)==pdTRUE) {
        uint64_t now=now_ms();
        portENTER_CRITICAL(&metadata);
        bool eligible=progress.active && progress.attempt_id==attempt &&
            now>=progress.deadline_ms && !progress.interrupted &&
            !progress.fd_present && (progress.phase==WHG_DNS ||
            (progress.phase==WHG_CONNECT && progress.recovery_requested));
        portEXIT_CRITICAL(&metadata);
        if(eligible) {
            /* Full-table vendor operation, potentially failing other pending
             * DNS (including SNTP). Only one overdue recovery uses it. */
            dns_clear_cache();
            portENTER_CRITICAL(&metadata);
            progress.dns_resets++; progress.interrupted=true;
            portEXIT_CRITICAL(&metadata);
            cleared=true;
        } else {
            portENTER_CRITICAL(&metadata); progress.stale_callbacks++;
            portEXIT_CRITICAL(&metadata);
        }
        xSemaphoreGive(dns_gate);
    }
    portENTER_CRITICAL(&metadata);
    /* Queue/gate admission is retryable; an actual clear stays latched.
     * Never alter a replacement job when this callback arrives late. */
    if(!cleared && progress.active && progress.attempt_id==attempt &&
            !progress.interrupted) progress.dns_requested=false;
    dns_callback.outstanding=false;
    portEXIT_CRITICAL(&metadata);
}
static void guard_tick(void) {
    weather_http_progress_t p=weather_http_guard_read();
    if(!p.active || now_ms()<p.deadline_ms) return;
    if(p.fd_present) {
        if(xSemaphoreTake(lifetime,0)!=pdTRUE) return;
        portENTER_CRITICAL(&metadata);
        bool eligible=progress.active && progress.attempt_id==p.attempt_id &&
            socket_fd>=0 && !progress.interrupted;
        int fd=socket_fd;
        if(eligible) { progress.interrupted=true; progress.socket_aborts++; }
        portEXIT_CRITICAL(&metadata);
        if(eligible) (void)shutdown(fd,SHUT_RDWR); /* Never HTTP close/cleanup. */
        xSemaphoreGive(lifetime);
    } else if(p.phase==WHG_DNS || (p.phase==WHG_CONNECT && p.recovery_requested)) {
        portENTER_CRITICAL(&metadata);
        bool queue=progress.active && progress.attempt_id==p.attempt_id &&
            !progress.fd_present && (progress.phase==WHG_DNS ||
            (progress.phase==WHG_CONNECT && progress.recovery_requested)) &&
            !progress.interrupted && !progress.dns_requested && !dns_callback.outstanding;
        if(queue) {
            progress.dns_requested=true;
            dns_callback.attempt=p.attempt_id; dns_callback.outstanding=true;
        }
        portEXIT_CRITICAL(&metadata);
        if(queue && tcpip_try_callback(reset_dns,NULL)!=ERR_OK) {
            portENTER_CRITICAL(&metadata);
            if(progress.active && progress.attempt_id==p.attempt_id &&
                    !progress.interrupted) progress.dns_requested=false;
            dns_callback.outstanding=false;
            portEXIT_CRITICAL(&metadata);
        }
    }
}
static void sample_stack_min(void) {
    /* ESP-IDF reports this watermark in bytes; never hold a network mutex. */
    uint32_t bytes=(uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&metadata);
    if(!stack_sampled || bytes<progress.guard_task_stack_min_bytes) {
        progress.guard_task_stack_min_bytes=bytes; stack_sampled=true;
    }
    portEXIT_CRITICAL(&metadata);
}
static void guard_task(void *unused) {
    (void)unused;
    for(;;) { sample_stack_min(); guard_tick(); vTaskDelay(pdMS_TO_TICKS(50)); }
}
bool weather_http_guard_start(void) {
    lifetime=xSemaphoreCreateMutex(); dns_gate=xSemaphoreCreateMutex();
    return lifetime && dns_gate &&
        xTaskCreate(guard_task,"http_guard",3072,NULL,2,NULL)==pdPASS;
}
const char *weather_http_owner_name(weather_http_owner_t owner) {
    static const char *names[]={"none","primary","met","air_quality","calendar"};
    return (unsigned)owner<sizeof names/sizeof names[0]?names[owner]:"unknown";
}
const char *weather_http_phase_name(weather_http_phase_t phase) {
    static const char *names[]={"idle","dns","connect","headers","body","cleanup","done"};
    return (unsigned)phase<sizeof names/sizeof names[0]?names[phase]:"unknown";
}
