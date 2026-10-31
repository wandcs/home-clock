#include "sensors.h"
#include "app_state.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static i2c_master_dev_handle_t sht, bh;
#define TIMEOUT_MS 100
static uint8_t crc8(const uint8_t *bytes) {
    uint8_t crc=0xff;
    for(int i=0;i<2;i++) {
        crc^=bytes[i];
        for(int bit=0;bit<8;bit++) crc=(crc&0x80)?(crc<<1)^0x31:crc<<1;
    }
    return crc;
}
static esp_err_t sht_read(float *temperature,float *humidity) {
    const uint8_t command=0xfd;
    esp_err_t err=i2c_master_transmit(sht,&command,1,TIMEOUT_MS);
    if(err!=ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t data[6]; err=i2c_master_receive(sht,data,sizeof data,TIMEOUT_MS);
    if(err!=ESP_OK) return err;
    if(crc8(data)!=data[2] || crc8(data+3)!=data[5]) return ESP_ERR_INVALID_CRC;
    *temperature=-45.0f+175.0f*(((uint16_t)data[0]<<8)|data[1])/65535.0f;
    *humidity=-6.0f+125.0f*(((uint16_t)data[3]<<8)|data[4])/65535.0f;
    if(*humidity<0) *humidity=0;
    if(*humidity>100) *humidity=100;
    return ESP_OK;
}
static esp_err_t bh_read(float *lux) {
    const uint8_t commands[]={0x01,0x42,0x65,0x20};
    for(unsigned i=0;i<sizeof commands;i++) {
        esp_err_t err=i2c_master_transmit(bh,&commands[i],1,TIMEOUT_MS);
        if(err!=ESP_OK) return err;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    uint8_t data[2]; esp_err_t err=i2c_master_receive(bh,data,2,TIMEOUT_MS);
    if(err==ESP_OK) *lux=(((uint16_t)data[0]<<8)|data[1])/1.2f;
    return err;
}
static void sensor_task(void *arg) {
    (void)arg;
    TickType_t next=xTaskGetTickCount();
    for(;;) {
        sensor_state_t state=app_state_read();
        float t=0,h=0,lux=0;
        if(state.sht.injection!=2) {
            esp_err_t err=state.sht.injection==1?ESP_FAIL:sht_read(&t,&h);
            app_state_publish(true,t,h,err,state.sht.injection,app_uptime());
        }
        state=app_state_read();
        if(state.bh.injection!=2) {
            esp_err_t err=state.bh.injection==1?ESP_FAIL:bh_read(&lux);
            app_state_publish(false,lux,0,err,state.bh.injection,app_uptime());
        }
        xTaskDelayUntil(&next,pdMS_TO_TICKS(2000));
    }
}
void sensors_start(void) {
    /* LVGL polling is the only other bus user. Keep it out while adding handles;
       afterward all transfers use the driver's operation semaphore. */
    ESP_ERROR_CHECK(bsp_display_lock(-1)?ESP_OK:ESP_FAIL);
    i2c_master_bus_handle_t bus=bsp_i2c_get_handle(); configASSERT(bus);
    i2c_device_config_t config={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=0x44,.scl_speed_hz=100000};
    /* Add handles even if a sensor is missing: each later read can recover. */
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&config,&sht));
    config.device_address=0x23;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&config,&bh));
    bsp_display_unlock();
    const uint8_t example[]={0xbe,0xef}; configASSERT(crc8(example)==0x92);
    BaseType_t created=xTaskCreate(sensor_task,"sensors",4096,NULL,4,NULL);
    configASSERT(created==pdPASS);
}
