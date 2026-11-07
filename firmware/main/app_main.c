#include "app_state.h"
#include "network_memory.h"
#include "app_ui.h"
#include "sensors.h"
#include "network_time.h"
#include "weather_service.h"
#include "location_service.h"
#include "alarm_bsp.h"
#include "alarm_service.h"
#include "calendar_runtime.h"
#include <stdio.h>
#include "driver/uart.h"
#include "soc/uart_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"

void app_main(void) {
    ESP_ERROR_CHECK(network_memory_start());
    app_state_init();
    weather_start();
    location_start();
    app_ui_start();
    if(aa_bsp_startup()!=AA_OK)printf("{\"event\":\"component_unavailable\",\"component\":\"alarm_audio\"}\n");
    if(!calendar_runtime_start(am_service_audio_busy))printf("{\"event\":\"component_unavailable\",\"component\":\"calendar\"}\n");
    sensors_start();
    if(CONFIG_CLOCK_DIAGNOSTICS) {
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0,2048,2048,0,NULL,0));
        const uart_config_t cfg={.baud_rate=460800,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
        ESP_ERROR_CHECK(uart_param_config(UART_NUM_0,&cfg));
        ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0,U0TXD_GPIO_NUM,U0RXD_GPIO_NUM,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    }
    bsp_display_lock(-1); app_ui_status("boot",0); bsp_display_unlock();
    network_time_start();
    char line[640]; size_t used=0; bool overflow=false; uint64_t next=app_uptime()+5000;
    for(;;) {
        if(CONFIG_CLOCK_DIAGNOSTICS) {
            uint8_t ch; int n=uart_read_bytes(UART_NUM_0,&ch,1,pdMS_TO_TICKS(100));
            if(n==1) {
                if(ch=='\n') {
                    line[used]=0;
                    if(!overflow) {
                        uint32_t rid=0; int decision=network_time_command(line,&rid);
                        if(decision==0) {
                            int weather=weather_command(line,&rid);
                            if(weather==0) app_ui_command(line);
                            else if(bsp_display_lock(1000)) {
                                app_ui_status(weather>0?"result":"rejected",rid); bsp_display_unlock();
                            }
                        }
                        else if(decision<0 && bsp_display_lock(1000)) {
                            app_ui_status("rejected",rid); bsp_display_unlock();
                        }
                    }
                    volatile char *wipe=line;
                    for(size_t i=0;i<sizeof line;i++) wipe[i]=0;
                    used=0; overflow=false;
                } else if(ch!='\r' && !overflow) {
                    if(used<sizeof line-1) line[used++]=ch;
                    else overflow=true; /* Discard the whole oversized line. */
                }
            }
        } else vTaskDelay(pdMS_TO_TICKS(100));
        if(CONFIG_CLOCK_DIAGNOSTICS) {
            uint32_t rid; bool ok;
            if(network_time_result(&rid,&ok) && bsp_display_lock(1000)) {
                app_ui_status(ok?"result":"rejected",rid); bsp_display_unlock();
            }
        }
        if(app_uptime()>=next) {
            if(bsp_display_lock(100)) { app_ui_status("health",0); bsp_display_unlock(); }
            next=app_uptime()+5000;
        }
    }
}
