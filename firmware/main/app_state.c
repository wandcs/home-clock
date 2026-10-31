#include "app_state.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t mutex;
static sensor_state_t state;
static network_state_t network;
int64_t app_utc_us(void) { struct timeval tv; gettimeofday(&tv,NULL); return (int64_t)tv.tv_sec*1000000+tv.tv_usec; }
network_state_t app_network_read(void) {
    xSemaphoreTake(mutex,portMAX_DELAY); network_state_t result=network;
    xSemaphoreGive(mutex); return result;
}
void app_network_publish(const network_state_t *value) {
    xSemaphoreTake(mutex,portMAX_DELAY); network=*value; xSemaphoreGive(mutex);
}
uint64_t app_uptime(void) { return esp_timer_get_time()/1000; }
void app_state_init(void) {
    mutex=xSemaphoreCreateMutex(); configASSERT(mutex);
    state.sht.error=state.bh.error=ESP_ERR_NOT_FOUND;
}
sensor_state_t app_state_read(void) {
    xSemaphoreTake(mutex,portMAX_DELAY);
    sensor_state_t result=state;
    xSemaphoreGive(mutex); return result;
}
void app_state_publish(bool sht, float first, float second, esp_err_t error,
                       int observed_injection, uint64_t acquired_ms) {
    xSemaphoreTake(mutex,portMAX_DELAY);
    measurement_t *m=sht?&state.sht:&state.bh;
    /* Discard an in-flight read if a development input changed meanwhile. */
    if(m->injection==observed_injection && observed_injection!=2) {
        m->attempted_ms=app_uptime(); m->error=error;
        if(error==ESP_OK) {
            m->first=first; m->second=second; m->acquired_ms=acquired_ms;
            m->has_value=true; m->samples++;
        } else if(observed_injection==1) m->injected_failures++;
        else m->failures++;
        state.sequence++;
    }
    xSemaphoreGive(mutex);
}
void app_state_inject(bool sht,int mode) {
    xSemaphoreTake(mutex,portMAX_DELAY);
    measurement_t *m=sht?&state.sht:&state.bh;
    m->injection=mode;
    if(mode==1) { m->error=ESP_FAIL; m->attempted_ms=app_uptime(); }
    /* Resume retains failure/stale validity until the next genuine read. */
    state.sequence++;
    xSemaphoreGive(mutex);
}
bool measurement_valid(const measurement_t *m,uint64_t now) {
    return m->has_value && m->error==ESP_OK && now-m->acquired_ms<=SAMPLE_FRESH_MS;
}
const char *measurement_reason(const measurement_t *m,uint64_t now) {
    if(m->injection==1) return "development_failure";
    if(!m->has_value) return m->error==ESP_ERR_NOT_FOUND?"waiting_or_missing":esp_err_to_name(m->error);
    if(m->error!=ESP_OK) return esp_err_to_name(m->error);
    return now-m->acquired_ms>SAMPLE_FRESH_MS?"stale":"ok";
}
