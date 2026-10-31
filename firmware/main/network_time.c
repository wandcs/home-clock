/* Wi-Fi, SNTP and product configuration owner. C6 credentials remain in RAM. */
#include "network_time.h"
#include "weather_service.h"
#include "wifi_store.h"
#include "nvs.h"
#include "app_state.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "esp_hosted.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

enum { CMD_WIFI=1,CMD_HOLD,CMD_NTP,CMD_RAMCHECK,EV_IP,EV_LOST,EV_ASSOC,EV_SYNC,EV_START,EV_STOP,
    CMD_SCAN,CMD_TRY,CMD_CANCEL,CMD_SAVE,EV_SCAN
#if CONFIG_CLOCK_DIAGNOSTICS
    ,CMD_WIFI_CLEAR
#endif
};
typedef struct {
    int kind,arg; uint32_t rid,generation;
    wifi_config_t config;
    esp_netif_ip_info_t ip;
    int64_t utc_us; uint64_t mono_ms; char server_ip[48];
} message_t;
typedef struct { uint32_t rid; bool ok; } reply_t;
static QueueHandle_t messages,replies;
static esp_netif_t *sta;
static network_state_t state;
static bool sntp_initialized;
static volatile uint32_t sntp_generation;
static uint64_t retry_at,sntp_deadline;
static uint64_t connection_deadline;
static bool connecting;
static wifi_settings_state_t settings, settings_public;
static portMUX_TYPE settings_lock=portMUX_INITIALIZER_UNLOCKED;
static wifi_config_t saved_config,candidate;
static bool have_saved, trying, switching, scan_pending, save_pending;
static int switch_target; /* 1 candidate, 2 saved (or idle when absent) */
static volatile uint32_t radio_generation;
static uint64_t attempt_deadline,scan_deadline,save_at;
static uint64_t switch_deadline;
#if CONFIG_CLOCK_DIAGNOSTICS
static bool fail_save;
void network_settings_fail_save(bool fail) { fail_save=fail; }
#endif
static unsigned retry_index;
static const char *const ntp_host="ntp.aliyun.com";
static const char *const test_host="192.0.2.1"; /* TEST-NET-1; development timeout only. */

static void wipe(void *p,size_t n) { volatile uint8_t *s=p; while(n--) *s++=0; }
static bool step(const char *name,esp_err_t result) {
    printf("{\"event\":\"network_step\",\"stage\":\"%s\",\"result\":%d,\"uptime_ms\":%"PRIu64"}\n",name,result,app_uptime());
    if(result!=ESP_OK) state.last_error=result;
    return result==ESP_OK;
}
static void publish(void) {
    app_network_publish(&state);
    portENTER_CRITICAL(&settings_lock); settings_public=settings; portEXIT_CRITICAL(&settings_lock);
}
wifi_settings_state_t network_settings_read(void) {
    wifi_settings_state_t copy;
    portENTER_CRITICAL(&settings_lock); copy=settings_public; portEXIT_CRITICAL(&settings_lock);
    return copy;
}
static bool setting_command(int kind,const wifi_config_t *config) {
    message_t m={.kind=kind}; if(config) m.config=*config;
    bool ok=messages && xQueueSend(messages,&m,0)==pdPASS;
    wipe(&m,sizeof m); return ok;
}
bool network_settings_scan(void) { return setting_command(CMD_SCAN,NULL); }
bool network_settings_connect(const wifi_config_t *c) { return setting_command(CMD_TRY,c); }
bool network_settings_cancel(void) { return setting_command(CMD_CANCEL,NULL); }
bool network_settings_save_retry(void) { return setting_command(CMD_SAVE,NULL); }
static void queue_event(message_t *msg) {
    if(xQueueSend(messages,msg,0)!=pdPASS)
        printf("{\"event\":\"network_event_overflow\",\"kind\":%d}\n",msg->kind);
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg; message_t m={.generation=radio_generation};
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) { m.kind=EV_IP; m.ip=((ip_event_got_ip_t*)data)->ip_info; }
    else if(base==IP_EVENT && id==IP_EVENT_STA_LOST_IP) m.kind=EV_LOST;
    else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) { m.kind=EV_LOST; m.arg=((wifi_event_sta_disconnected_t*)data)->reason; }
    else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_CONNECTED) { m.kind=EV_ASSOC; m.arg=((wifi_event_sta_connected_t*)data)->channel; }
    else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START) m.kind=EV_START;
    else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_STOP) m.kind=EV_STOP;
    else if(base==WIFI_EVENT && id==WIFI_EVENT_SCAN_DONE) { m.kind=EV_SCAN; m.arg=((wifi_event_sta_scan_done_t*)data)->status; }
    if(m.kind) queue_event(&m);
}
static void sync_callback(struct timeval *tv) {
    message_t m={.kind=EV_SYNC,.generation=sntp_generation,
        .utc_us=(int64_t)tv->tv_sec*1000000+tv->tv_usec,.mono_ms=app_uptime()};
    const ip_addr_t *ip=esp_sntp_getserver(0);
    if(ip) ipaddr_ntoa_r(ip,m.server_ip,sizeof m.server_ip);
    queue_event(&m);
}
static void stop_sntp(void) {
    sntp_generation++;
    if(sntp_initialized) { esp_netif_sntp_deinit(); sntp_initialized=false; }
    state.sntp_waiting=false; sntp_deadline=0;
}
static bool start_sntp(void) {
    stop_sntp(); state.sntp_timeout=false;
    if(!state.connected || state.ntp_mode==0) return true;
    const char *server=state.ntp_mode==2?test_host:ntp_host;
    esp_sntp_config_t cfg=ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    cfg.start=false; cfg.wait_for_sync=false; cfg.sync_cb=sync_callback;
    if(!step("sntp_init",esp_netif_sntp_init(&cfg))) return false;
    sntp_initialized=true;
    if(!step("sntp_start",esp_netif_sntp_start())) return false;
    state.sntp_waiting=true; sntp_deadline=app_uptime()+20000;
    printf("{\"event\":\"sntp_request\",\"source\":\"%s\",\"development_test\":%s,\"generation\":%"PRIu32",\"uptime_ms\":%"PRIu64"}\n",server,state.ntp_mode==2?"true":"false",sntp_generation,app_uptime());
    return true;
}
static bool radio_initialize(void) {
    wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT(); cfg.nvs_enable=0;
    if(!step("wifi_init",esp_wifi_init(&cfg))) return false;
    if(!step("storage_ram",esp_wifi_set_storage(WIFI_STORAGE_RAM))) return false;
    uint8_t mac[6];
    if(!step("get_mac",esp_wifi_get_mac(WIFI_IF_STA,mac))) return false;
    printf("{\"event\":\"coprocessor_mac\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}\n",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    if(!step("mode_sta",esp_wifi_set_mode(WIFI_MODE_STA))) return false;
    if(!step("wifi_start",esp_wifi_start())) return false;
    state.radio_running=true; state.wifi_ready=true; return true;
}
typedef struct { char body[4096]; size_t used; bool truncated; } http_body_t;
static esp_err_t http_event(esp_http_client_event_t *event) {
    http_body_t *b=event->user_data;
    if(event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        size_t n=(size_t)event->data_len;
        if(n>sizeof b->body-1-b->used) { n=sizeof b->body-1-b->used; b->truncated=true; }
        memcpy(b->body+b->used,event->data,n); b->used+=n; b->body[b->used]=0;
    }
    return ESP_OK;
}
static void https_probe(void) {
    if(state.https_done || !state.time_valid || !state.connected) return;
    if(!weather_http_take(0)) return; /* Diagnostic probe never competes for TLS RAM. */
    state.https_done=true; publish();
    struct addrinfo *address=NULL;
    struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM};
    int dns=getaddrinfo("www.baidu.com",NULL,&hints,&address);
    printf("{\"event\":\"dns_probe\",\"host\":\"www.baidu.com\",\"result\":%d}\n",dns);
    if(address) freeaddrinfo(address);
    if(dns!=0) { state.last_error=ESP_ERR_NOT_FOUND; weather_http_give(); return; }
    http_body_t body={0};
    esp_http_client_config_t cfg={.url="https://www.baidu.com/robots.txt",.timeout_ms=10000,
        .crt_bundle_attach=esp_crt_bundle_attach,.skip_cert_common_name_check=false,
        .disable_auto_redirect=true,.buffer_size=1024,.buffer_size_tx=512,
        .event_handler=http_event,.user_data=&body};
    esp_http_client_handle_t client=esp_http_client_init(&cfg);
    esp_err_t result=client?esp_http_client_perform(client):ESP_ERR_NO_MEM;
    state.https_status=client?esp_http_client_get_status_code(client):0;
    bool parsed=strstr(body.body,"User-agent:")!=NULL && strstr(body.body,"Disallow:")!=NULL;
    state.https_ok=result==ESP_OK && state.https_status==200 && parsed && !body.truncated;
    printf("{\"event\":\"https_probe\",\"url\":\"https://www.baidu.com/robots.txt\",\"certificate_bundle\":true,\"hostname_verified\":%s,\"result\":%d,\"http_status\":%d,\"bytes\":%u,\"parsed\":%s,\"truncated\":%s}\n",result==ESP_OK?"true":"false",result,state.https_status,(unsigned)body.used,parsed?"true":"false",body.truncated?"true":"false");
    if(client) esp_http_client_cleanup(client);
    weather_http_give();
}
static void backoff(void) {
    static const unsigned delays[]={1000,2000,4000,8000,15000,30000};
    unsigned i=retry_index<5?retry_index++:5;
    retry_at=app_uptime()+delays[i];
    if(retry_at<state.hold_until_ms) retry_at=state.hold_until_ms;
}
static bool apply_credentials(wifi_config_t *cfg) {
    if(!state.wifi_ready || !state.radio_running) return false;
    if(state.config_loaded) esp_wifi_disconnect();
    state.config_loaded=false;
    if(!step("set_config_ram",esp_wifi_set_config(WIFI_IF_STA,cfg))) return false;
    wifi_config_t check={0}; esp_err_t result=esp_wifi_get_config(WIFI_IF_STA,&check);
    bool same=result==ESP_OK && memcmp(check.sta.ssid,cfg->sta.ssid,sizeof cfg->sta.ssid)==0
        && memcmp(check.sta.password,cfg->sta.password,sizeof cfg->sta.password)==0;
    wipe(&check,sizeof check);
    printf("{\"event\":\"ram_config_roundtrip\",\"result\":%d,\"matched\":%s}\n",result,same?"true":"false");
    if(!same) return false;
    state.ram_roundtrip=true; state.ram_retention_empty=false; state.config_loaded=true;
    state.config_generation++; state.hold_until_ms=0; retry_at=app_uptime(); retry_index=0;
    return true;
}
static bool ram_check(void) {
    state.config_loaded=false; state.wifi_ready=false; stop_sntp();
    connecting=false; connection_deadline=0; retry_at=0;
    state.connected=false; state.associated=false; state.radio_running=false;
    if(!step("ramcheck_stop",esp_wifi_stop()) || !step("ramcheck_deinit",esp_wifi_deinit())) return false;
    if(!radio_initialize()) return false;
    wifi_config_t check={0}; esp_err_t result=esp_wifi_get_config(WIFI_IF_STA,&check);
    state.ram_retention_empty=result==ESP_OK && check.sta.ssid[0]==0 && check.sta.password[0]==0;
    wipe(&check,sizeof check);
    printf("{\"event\":\"ram_reinitialize\",\"result\":%d,\"config_empty\":%s}\n",result,state.ram_retention_empty?"true":"false");
    return state.ram_retention_empty;
}
static bool auth_supported(int auth) {
    return auth==WIFI_AUTH_OPEN || auth==WIFI_AUTH_WPA_PSK || auth==WIFI_AUTH_WPA2_PSK
        || auth==WIFI_AUTH_WPA_WPA2_PSK || auth==WIFI_AUTH_WPA3_PSK || auth==WIFI_AUTH_WPA2_WPA3_PSK;
}
static void switch_network(int target) {
    stop_sntp(); state.connected=false; state.associated=false; state.config_loaded=false;
    connecting=false; connection_deadline=0; retry_at=0;
    switching=true; switch_target=target;
    switch_deadline=app_uptime()+8000;
    if(state.radio_running) {
        if(!step("settings_stop",esp_wifi_stop())) { switching=false; settings.phase=WIFI_FAILED; settings.error=ESP_FAIL; }
        state.radio_running=false;
    } else {
        message_t m={.kind=EV_STOP,.generation=radio_generation}; queue_event(&m);
    }
}
static void finish_switch(void) {
    switching=false; switch_deadline=0; radio_generation++;
    const wifi_config_t *c=switch_target==1?&candidate:(have_saved?&saved_config:NULL);
    if(c) {
        if(!step("settings_config_ram",esp_wifi_set_config(WIFI_IF_STA,(wifi_config_t*)c))) {
            trying=false; settings.phase=WIFI_FAILED; settings.error=state.last_error; return;
        }
        state.config_loaded=true; state.config_generation++;
    }
    if(step("settings_start",esp_wifi_start())) {
        state.radio_running=true; retry_at=app_uptime(); retry_index=0;
    } else { trying=false; settings.phase=WIFI_FAILED; settings.error=state.last_error; }
}
static void fail_attempt(int error) {
    trying=false; save_pending=false; attempt_deadline=0;
    settings.phase=WIFI_FAILED; settings.error=error;
    wipe(&candidate,sizeof candidate); switch_network(2);
}
static void save_candidate(void) {
    save_pending=false;
    esp_err_t err;
#if CONFIG_CLOCK_DIAGNOSTICS
    if(fail_save) err=ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    else
#endif
    err=wifi_store_save(&candidate);
    settings.error=err;
    if(err==ESP_OK) {
        saved_config=candidate; have_saved=true; settings.saved_present=true; settings.saved_revision++;
        settings.storage_error=ESP_OK;settings.save_unconfirmed=false;
        settings.phase=WIFI_SAVED; trying=false; attempt_deadline=0; wipe(&candidate,sizeof candidate);
    } else {settings.phase=WIFI_SAVE_FAILED;settings.save_unconfirmed=true;}
    printf("{\"event\":\"wifi_save\",\"result\":%d,\"revision\":%"PRIu32",\"attempt\":%"PRIu32"}\n",err,settings.saved_revision,settings.attempt);
    publish();
}
#if CONFIG_CLOCK_DIAGNOSTICS
static esp_err_t clear_saved_wifi(void) {
    bool idle=settings.phase==WIFI_IDLE||settings.phase==WIFI_LIST||settings.phase==WIFI_SAVED;
    if(!settings.initialized||!state.wifi_ready||settings.storage_error!=ESP_OK||
       settings.save_unconfirmed||!idle||trying||switching||scan_pending||save_pending||
       connecting||connection_deadline) return ESP_ERR_INVALID_STATE;
    /* Keep the live connection and published saved state until a real reload. */
    return wifi_store_clear();
}
#endif
static void service(void *unused) {
    (void)unused; state.ntp_mode=1;
    esp_log_level_set("*",ESP_LOG_WARN);
    settings.storage_error=wifi_store_init();
    if(settings.storage_error==ESP_OK) {
        settings.storage_error=wifi_store_load(&saved_config);
        if(settings.storage_error==ESP_OK) {
            have_saved=true; settings.saved_present=true; settings.boot_loaded=true; settings.saved_revision=1;
        } else if(settings.storage_error==ESP_ERR_NVS_NOT_FOUND) settings.storage_error=ESP_OK;
    }
    settings.initialized=true; /* Store read completed, including a reported failure. */
    printf("{\"event\":\"wifi_store_boot\",\"loaded\":%s,\"result\":%d}\n",have_saved?"true":"false",settings.storage_error);
    if(!step("netif",esp_netif_init())) goto failed;
    esp_err_t result=esp_event_loop_create_default();
    if(result!=ESP_OK && result!=ESP_ERR_INVALID_STATE) { step("event_loop",result); goto failed; }
    sta=esp_netif_create_default_wifi_sta(); if(!sta) goto failed;
    if(!step("wifi_events",esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL))) goto failed;
    if(!step("ip_events",esp_event_handler_register(IP_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL))) goto failed;
    if(!step("hosted_init",esp_hosted_init()) || !step("link",esp_hosted_connect_to_slave())) goto failed;
    if(!radio_initialize()) goto failed;
    if(have_saved) apply_credentials(&saved_config);
    publish();
    for(;;) {
        message_t m={0};
        if(xQueueReceive(messages,&m,pdMS_TO_TICKS(100))==pdPASS) {
            bool ok=true;
            switch(m.kind) {
            case CMD_SCAN:
                if(trying || switching || scan_pending || !state.wifi_ready) { ok=false; break; }
                settings.phase=WIFI_SCANNING; settings.error=0; settings.count=0; publish();
                if(!state.radio_running) { ok=step("scan_radio_start",esp_wifi_start()); state.radio_running=ok; }
                if(ok) {
                    wifi_scan_config_t config={.show_hidden=false};
                    ok=step("settings_scan",esp_wifi_scan_start(&config,false));
                    if(ok) { scan_pending=true; scan_deadline=app_uptime()+15000; }
                }
                if(!ok) { settings.phase=WIFI_LIST; settings.error=state.last_error; }
                break;
            case EV_SCAN: {
                if(!scan_pending) { esp_wifi_clear_ap_list(); break; }
                scan_pending=false; settings.phase=WIFI_LIST; settings.error=m.arg;
                wifi_ap_record_t records[WIFI_LIST_MAX]; uint16_t count=WIFI_LIST_MAX;
                esp_err_t err=esp_wifi_scan_get_ap_records(&count,records);
                if(err!=ESP_OK) { settings.error=err; count=0; }
                settings.count=0;
                for(unsigned i=0;i<count;i++) {
                    if(!records[i].ssid[0]) continue;
                    bool duplicate=false;
                    for(int j=0;j<settings.count;j++) if(strncmp(settings.choices[j].ssid,(char*)records[i].ssid,32)==0 && settings.choices[j].auth==records[i].authmode) duplicate=true;
                    if(duplicate) continue;
                    wifi_choice_t *c=&settings.choices[settings.count++]; memset(c,0,sizeof *c);
                    memcpy(c->ssid,records[i].ssid,32); c->rssi=records[i].rssi; c->auth=records[i].authmode; c->supported=auth_supported(c->auth);
                }
                wipe(records,sizeof records); settings.scan_revision++;
                printf("{\"event\":\"wifi_scan\",\"count\":%d,\"result\":%d}\n",settings.count,settings.error); break;
            }
            case CMD_TRY:
                if(trying || switching || scan_pending || !state.wifi_ready || !auth_supported(m.config.sta.threshold.authmode) || !m.config.sta.ssid[0]) { ok=false; break; }
                candidate=m.config; trying=true; settings.attempt++; settings.phase=WIFI_CONNECTING; settings.error=0;
                attempt_deadline=app_uptime()+45000; switch_network(1); break;
            case CMD_CANCEL:
                if(settings.phase==WIFI_SAVING) { ok=false; break; }
                if(scan_pending) { esp_wifi_scan_stop(); scan_pending=false; }
                if(trying || switching || settings.phase==WIFI_SAVE_FAILED) {
                    trying=false; save_pending=false; attempt_deadline=0; wipe(&candidate,sizeof candidate); switch_network(2);
                }
                settings.phase=WIFI_IDLE; break;
            case CMD_SAVE:
                if(settings.phase!=WIFI_SAVE_FAILED || !trying || !state.connected) { ok=false; break; }
                settings.phase=WIFI_SAVING; save_pending=true; save_at=app_uptime()+300; break;
            case CMD_WIFI: ok=apply_credentials(&m.config); break;
            case CMD_NTP:
                if(m.arg==2 && state.time_valid) ok=false;
                else { state.ntp_mode=m.arg; ok=start_sntp(); }
                break;
            case CMD_HOLD:
                state.hold_until_ms=app_uptime()+(uint64_t)m.arg*1000;
                stop_sntp(); connecting=false; connection_deadline=0;
                ok=step("hold_radio_stop",esp_wifi_stop());
                if(ok) { state.radio_running=false; state.connected=false; state.associated=false; state.disconnected_ms=app_uptime(); }
                printf("{\"event\":\"radio_hold\",\"seconds\":%d,\"uptime_ms\":%"PRIu64",\"until_ms\":%"PRIu64",\"result\":%s}\n",m.arg,app_uptime(),state.hold_until_ms,ok?"true":"false");
                break;
            case CMD_RAMCHECK: ok=ram_check(); break;
#if CONFIG_CLOCK_DIAGNOSTICS
            case CMD_WIFI_CLEAR: {
                esp_err_t err=clear_saved_wifi();ok=err==ESP_OK;
                printf("{\"event\":\"wifi_clear\",\"request_id\":%"PRIu32",\"result\":%d,\"reload_required\":%s}\n",m.rid,err,ok?"true":"false");
                break;
            }
#endif
            case EV_START:
                if(m.generation!=radio_generation || switching) break;
                state.radio_running=true;
                printf("{\"event\":\"wifi_radio_started\",\"source\":\"WIFI_EVENT\",\"uptime_ms\":%"PRIu64"}\n",app_uptime()); break;
            case EV_STOP:
                if(switching) { finish_switch(); break; }
                if(m.generation!=radio_generation) break;
                state.radio_running=false; state.connected=false; state.associated=false;
                connecting=false; connection_deadline=0;
                printf("{\"event\":\"wifi_radio_stopped\",\"source\":\"WIFI_EVENT\",\"uptime_ms\":%"PRIu64"}\n",app_uptime()); break;
            case EV_ASSOC:
                if(switching || m.generation!=radio_generation || !state.config_loaded) break;
                state.associated=true; connecting=false; connection_deadline=app_uptime()+30000;
                printf("{\"event\":\"wifi_associated\",\"channel\":%d,\"uptime_ms\":%"PRIu64"}\n",m.arg,app_uptime()); break;
            case EV_LOST:
                if(switching || m.generation!=radio_generation) break;
                state.connected=false; state.associated=false; state.disconnect_reason=m.arg; connecting=false; connection_deadline=0;
                state.disconnected_ms=app_uptime(); stop_sntp(); backoff();
                printf("{\"event\":\"wifi_disconnected\",\"reason\":%d,\"uptime_ms\":%"PRIu64"}\n",m.arg,app_uptime());
                if(trying) fail_attempt(m.arg);
                break;
            case EV_IP: {
                if(switching || m.generation!=radio_generation || !state.associated || !state.config_loaded) break;
                wifi_ap_record_t current={0};
                esp_err_t ap=esp_wifi_sta_get_ap_info(&current);
                const wifi_config_t *expected=trying?&candidate:(have_saved?&saved_config:NULL);
                bool matches=ap==ESP_OK && (!expected || memcmp(current.ssid,expected->sta.ssid,32)==0);
                wipe(&current,sizeof current); if(!matches) break;
                state.connected=true; state.associated=true; state.connection_count++;
                connecting=false; connection_deadline=0;
                state.reconnected_ms=app_uptime(); retry_index=0;
                esp_netif_dns_info_t dns={0}; esp_err_t dr=esp_netif_get_dns_info(sta,ESP_NETIF_DNS_MAIN,&dns);
                printf("{\"event\":\"dhcp\",\"ip\":\""IPSTR"\",\"gateway\":\""IPSTR"\",\"netmask\":\""IPSTR"\",\"dns_result\":%d,\"dns\":\""IPSTR"\",\"uptime_ms\":%"PRIu64"}\n",IP2STR(&m.ip.ip),IP2STR(&m.ip.gw),IP2STR(&m.ip.netmask),dr,IP2STR(&dns.ip.u_addr.ip4),app_uptime());
                if(trying && settings.phase==WIFI_CONNECTING) { settings.phase=WIFI_SAVING; save_pending=true; save_at=app_uptime()+300; }
                start_sntp(); break;
            }
            case EV_SYNC:
                if(m.generation!=sntp_generation || !state.connected || state.ntp_mode!=1) break;
                state.last_adjust_us=state.time_valid?m.utc_us-(state.last_sync_utc_us+(int64_t)(m.mono_ms-state.last_sync_ms)*1000):0;
                state.time_valid=true; state.sync_count++; state.last_sync_ms=m.mono_ms; state.last_sync_utc_us=m.utc_us;
                snprintf(state.server_ip,sizeof state.server_ip,"%s",m.server_ip);
                state.sntp_waiting=false; state.sntp_timeout=false; sntp_deadline=0;
                printf("{\"event\":\"sntp_sync\",\"source\":\"%s\",\"server_ip\":\"%s\",\"utc_us\":%"PRId64",\"uptime_ms\":%"PRIu64",\"sync_count\":%"PRIu32",\"adjust_us\":%"PRId64"}\n",state.ntp_mode==2?test_host:ntp_host,state.server_ip,m.utc_us,m.mono_ms,state.sync_count,state.last_adjust_us);
                publish(); if(CONFIG_CLOCK_DIAGNOSTICS && !trying) https_probe(); break;
            default: break;
            }
            if(m.rid) {
                reply_t reply={m.rid,ok}; xQueueSend(replies,&reply,0);
            }
            wipe(&m,sizeof m); publish();
        }
        uint64_t now=app_uptime();
        if(switching && now>=switch_deadline) {
            switching=false; trying=false; save_pending=false; settings.phase=WIFI_FAILED; settings.error=ESP_ERR_TIMEOUT;
            wipe(&candidate,sizeof candidate); publish();
        }
        if(save_pending && now>=save_at) save_candidate();
        if(trying && attempt_deadline && now>=attempt_deadline && settings.phase==WIFI_CONNECTING) { fail_attempt(ESP_ERR_TIMEOUT); publish(); }
        if(scan_pending && now>=scan_deadline) {
            esp_wifi_scan_stop(); esp_wifi_clear_ap_list(); scan_pending=false; settings.phase=WIFI_LIST; settings.error=ESP_ERR_TIMEOUT; publish();
        }
        if(state.config_loaded && !state.connected && !switching && !scan_pending && now>=state.hold_until_ms) {
            if(!state.radio_running) {
                if(step("resume_radio",esp_wifi_start())) { state.radio_running=true; state.hold_until_ms=0; retry_at=now; }
                else { state.hold_until_ms=now+30000; }
                publish();
            }
            if(state.radio_running && !state.associated && !connecting && now>=retry_at) {
                if(step("wifi_connect",esp_wifi_connect())) { connecting=true; connection_deadline=now+20000; }
                else backoff();
                publish();
            }
        }
        if(connection_deadline && now>=connection_deadline && !state.connected) {
            step("connection_timeout_disconnect",esp_wifi_disconnect());
            connection_deadline=0; connecting=false; state.associated=false; backoff(); publish();
        }
        if(sntp_deadline && now>=sntp_deadline) {
            state.sntp_timeout=true; state.sntp_waiting=false; sntp_deadline=0;
            printf("{\"event\":\"sntp_timeout\",\"development_test\":%s,\"uptime_ms\":%"PRIu64"}\n",state.ntp_mode==2?"true":"false",now); publish();
        }
    }
failed:
    state.wifi_ready=false; publish(); vTaskDelete(NULL);
}
#if CONFIG_CLOCK_DIAGNOSTICS
static int hex_value(char c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static bool decode(const char *text,uint8_t *out,size_t capacity) {
    size_t n=strlen(text); if(!n || n%2 || n/2>capacity) return false;
    for(size_t i=0;i<n;i+=2) { int a=hex_value(text[i]),b=hex_value(text[i+1]); if(a<0||b<0) return false; out[i/2]=(a<<4)|b; }
    return true;
}
int network_time_command(char *line,uint32_t *rid) {
    char op[24]; unsigned id=0;
    if(sscanf(line,"%u %23s",&id,op)!=2 || !id) return 0;
    message_t m={.rid=id}; char extra;
    if(strcmp(op,"wifi")==0) {
        *rid=id; char ssid[65]={0},password[129]={0};
        int count=sscanf(line,"%u %23s %64s %128s %c",&id,op,ssid,password,&extra);
        bool valid=count==4 && decode(ssid,m.config.sta.ssid,sizeof m.config.sta.ssid)
            && decode(password,m.config.sta.password,sizeof m.config.sta.password);
        wipe(ssid,sizeof ssid); wipe(password,sizeof password);
        if(!valid || !app_network_read().wifi_ready) { wipe(&m,sizeof m); return -1; }
        m.kind=CMD_WIFI; m.config.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
        m.config.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH;
        m.config.sta.pmf_cfg.capable=true;
    } else if(strcmp(op,"hold")==0 || strcmp(op,"ntp")==0) {
        *rid=id;
        if(sscanf(line,"%u %23s %d %c",&id,op,&m.arg,&extra)!=3) return -1;
        m.kind=strcmp(op,"hold")==0?CMD_HOLD:CMD_NTP;
        if((m.kind==CMD_HOLD && (m.arg<1||m.arg>300)) || (m.kind==CMD_NTP && (m.arg<0||m.arg>2))) return -1;
    } else if(strcmp(op,"wifi_clear")==0) {
        *rid=id;char confirm[2]={0};
        if(sscanf(line,"%u %23s %1s %c",&id,op,confirm,&extra)!=3||strcmp(confirm,"1")) return -1;
        m.kind=CMD_WIFI_CLEAR;
    } else if(strcmp(op,"ramcheck")==0) { *rid=id; m.kind=CMD_RAMCHECK; }
    else if(strcmp(op,"lateip")==0 || strcmp(op,"latesync")==0) {
        *rid=id; m.kind=strcmp(op,"lateip")==0?EV_IP:EV_SYNC;
        m.generation=(m.kind==EV_IP?radio_generation:sntp_generation)-1;
        printf("{\"event\":\"development_late_event\",\"kind\":%d}\n",m.kind);
    }
    else return 0;
    bool queued=messages && xQueueSend(messages,&m,0)==pdPASS;
    wipe(&m,sizeof m); return queued?1:-1;
}
bool network_time_result(uint32_t *rid,bool *ok) {
    reply_t reply;
    if(!replies || xQueueReceive(replies,&reply,0)!=pdPASS) return false;
    *rid=reply.rid; *ok=reply.ok; return true;
}
#endif
void network_time_start(void) {
    messages=xQueueCreate(8,sizeof(message_t)); replies=xQueueCreate(4,sizeof(reply_t));
    configASSERT(messages && replies);
    configASSERT(xTaskCreate(service,"network_time",12288,NULL,4,NULL)==pdPASS);
}
