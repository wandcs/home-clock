#include "platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static _Thread_local int64_t clock_us;
static _Thread_local bool in_callback;
void test_set_time_us(int64_t value){clock_us=value;}
int64_t esp_timer_get_time(void){return clock_us;}
void test_callback_begin(void){if(in_callback)_Exit(90);in_callback=true;}
void test_callback_end(void){in_callback=false;}
static void reject_callback_io(void){if(in_callback)_Exit(91);}
void *__real_malloc(size_t size);
void *__real_calloc(size_t count,size_t size);
void *__real_realloc(void *ptr,size_t size);
void __real_free(void *ptr);
int __real_printf(const char *format,...);
int __real_puts(const char *text);
void *__wrap_malloc(size_t size){reject_callback_io();return __real_malloc(size);}
void *__wrap_calloc(size_t count,size_t size){reject_callback_io();return __real_calloc(count,size);}
void *__wrap_realloc(void *ptr,size_t size){reject_callback_io();return __real_realloc(ptr,size);}
void __wrap_free(void *ptr){reject_callback_io();__real_free(ptr);}
int __wrap_printf(const char *format,...){
    reject_callback_io();va_list ap;va_start(ap,format);int result=vprintf(format,ap);va_end(ap);return result;
}
int __wrap_puts(const char *text){reject_callback_io();return __real_puts(text);}
