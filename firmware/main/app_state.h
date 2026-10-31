#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SAMPLE_FRESH_MS 6000
typedef struct {
    float first, second;
    uint64_t acquired_ms, attempted_ms;
    esp_err_t error;
    uint32_t samples, failures, injected_failures;
    bool has_value;
    int injection; /* 0 real, 1 fail, 2 stop publishing (stale) */
} measurement_t;
typedef struct { measurement_t sht, bh; uint32_t sequence; } sensor_state_t;
typedef struct {
    bool wifi_ready, associated, connected, radio_running, config_loaded;
    bool time_valid, sntp_waiting, sntp_timeout, https_done, https_ok;
    bool ram_roundtrip, ram_retention_empty;
    int ntp_mode, last_error, disconnect_reason, https_status;
    uint32_t sync_count, connection_count, config_generation;
    uint64_t last_sync_ms, hold_until_ms, disconnected_ms, reconnected_ms;
    int64_t last_sync_utc_us, last_adjust_us;
    char server_ip[48];
} network_state_t;
uint64_t app_uptime(void);
void app_state_init(void);
sensor_state_t app_state_read(void);
network_state_t app_network_read(void);
void app_network_publish(const network_state_t *value);
int64_t app_utc_us(void);
void app_state_publish(bool sht, float first, float second, esp_err_t error,
                       int observed_injection, uint64_t acquired_ms);
void app_state_inject(bool sht, int mode);
bool measurement_valid(const measurement_t *value, uint64_t now);
const char *measurement_reason(const measurement_t *value, uint64_t now);
