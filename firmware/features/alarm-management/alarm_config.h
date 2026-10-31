#ifndef ALARM_CONFIG_H
#define ALARM_CONFIG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "alarm_core.h" /* Read-only F5-R1 source supplied by integration include path. */
#define AM_CAPACITY 8u
#define AM_WIRE_MAX (20u + AM_CAPACITY * 8u)
typedef struct { uint32_t id; uint8_t hour, minute, mode, enabled; } am_entry;
typedef struct { uint32_t next_id; uint8_t count; am_entry entries[AM_CAPACITY]; } am_config;
typedef enum { AM_DECODE_OK, AM_DECODE_BAD, AM_DECODE_FOREIGN } am_decode_status;
void am_config_empty(am_config *config);
bool am_config_valid(const am_config *config);
bool am_config_equal(const am_config *a, const am_config *b);
int am_find(const am_config *config, uint32_t id);
size_t am_encode(const am_config *config, uint8_t *bytes, size_t capacity);
am_decode_status am_decode(const uint8_t *bytes, size_t size, am_config *out);
#endif
