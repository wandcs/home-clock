#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi.h"
#ifndef CONFIG_CLOCK_DIAGNOSTICS
#define CONFIG_CLOCK_DIAGNOSTICS 0
#endif
enum { WIFI_IDLE, WIFI_SCANNING, WIFI_LIST, WIFI_CONNECTING, WIFI_SAVING, WIFI_SAVED, WIFI_FAILED, WIFI_SAVE_FAILED };
#define WIFI_LIST_MAX 24
typedef struct { char ssid[33]; int rssi, auth; bool supported; } wifi_choice_t;
typedef struct {
    int phase, error, storage_error, count;
    bool saved_present, boot_loaded, initialized, save_unconfirmed;
    uint32_t attempt, saved_revision, scan_revision;
    wifi_choice_t choices[WIFI_LIST_MAX];
} wifi_settings_state_t;
wifi_settings_state_t network_settings_read(void);
bool network_settings_scan(void);
bool network_settings_connect(const wifi_config_t *config);
bool network_settings_cancel(void);
bool network_settings_save_retry(void);
#if CONFIG_CLOCK_DIAGNOSTICS
void network_settings_fail_save(bool fail);
#endif
void network_time_start(void);
/* 0: other command, 1: queued, -1: rejected. No input contents are logged. */
int network_time_command(char *line,uint32_t *rid);
bool network_time_result(uint32_t *rid,bool *ok);
