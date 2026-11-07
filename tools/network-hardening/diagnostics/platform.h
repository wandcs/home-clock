#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void test_set_time_us(int64_t value);
void test_emit_failed_alloc(size_t size,uint32_t caps,const char *function_name);
bool test_hook_registered(void);
void test_callback_begin(void);
void test_callback_end(void);
