#include "wifi_ui.h"
#include "network_time.h"
#include "app_state.h"
#include "bsp/esp-bsp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
extern const lv_font_t clock_font_28,clock_font_44;
static lv_obj_t *layer,*list_page,*edit_page,*rows,*note,*summary,*ssid_label,*password,*keys,*submit,*show;
static lv_obj_t *list_back,*rescan,*edit_cancel,*list_title;
static ui_palette_t palette=UI_PALETTE_NEUTRAL;
static bool note_error,summary_error;
static wifi_config_t draft;
static bool visible,editing,synthetic,submitted,showing;
static bool first_context;
static void (*completed)(bool first,bool saved);
static uint32_t scan_revision;
static unsigned keyboard_mode,key_events,physical_keys;
static int previous_phase=-1;
static bool rendered_time_valid;
static uint64_t submitted_at;
static char ssid_text[256];
static const char *lower[]={"q","w","e","r","t","y","u","i","o","p","\n","a","s","d","f","g","h","j","k","l","\n","ABC","z","x","c","v","b","n","m","Del","\n","123","左","space","右","",NULL};
static const char *upper[]={"Q","W","E","R","T","Y","U","I","O","P","\n","A","S","D","F","G","H","J","K","L","\n","abc","Z","X","C","V","B","N","M","Del","\n","123","左","space","右","",NULL};
static const char *symbols[]={"1","2","3","4","5","6","7","8","9","0","-","=","\n","!","@","#","$","%","^","&","*","(",")","_","+","\n","[","]","{","}",";",":","'","\"",",",".","/","\\","\n","abc","左","右","Del","space","<",">","?","|","~","`","",NULL};
static void wipe(void *p,size_t n) { volatile uint8_t *v=p; while(n--) *v++=0; }
static lv_obj_t *text(lv_obj_t *p,const char *s,int x,int y,int w) {
    lv_obj_t *o=lv_label_create(p); lv_label_set_text(o,s); lv_obj_set_pos(o,x,y); lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,&clock_font_28,0); lv_obj_set_style_text_color(o,lv_color_hex(palette.ink),0);
    lv_obj_set_clickable(o,false); return o;
}
static lv_obj_t *panel(lv_obj_t *p,int x,int y,int w,int h) {
    lv_obj_t *o=lv_obj_create(p); lv_obj_remove_style_all(o); lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.bg),0); lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0); lv_obj_set_scrollable(o,false); return o;
}
static void theme_button(lv_obj_t *o){
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.button),0);
    lv_obj_set_style_border_color(o,lv_color_hex(palette.edge),0);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.deep),LV_STATE_PRESSED);
    lv_obj_set_style_border_color(o,lv_color_hex(palette.on),LV_STATE_PRESSED);
    lv_obj_set_style_opa(o,LV_OPA_50,LV_STATE_DISABLED);
    lv_obj_set_style_text_color(lv_obj_get_child(o,0),lv_color_hex(palette.ink),0);
}
static void theme_input(void){
    lv_obj_set_style_bg_color(password,lv_color_hex(palette.button),0);
    lv_obj_set_style_text_color(password,lv_color_hex(palette.ink),0);
    lv_obj_set_style_border_color(password,lv_color_hex(palette.edge),0);
    lv_obj_set_style_border_color(password,lv_color_hex(palette.on),LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(password,lv_color_hex(palette.deep),LV_STATE_DISABLED);
    lv_obj_set_style_text_color(password,lv_color_hex(palette.sub),LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(password,lv_color_hex(palette.ink),LV_PART_CURSOR);
    lv_obj_set_style_bg_color(password,lv_color_hex(palette.on),LV_PART_SELECTED);
    lv_obj_set_style_text_color(password,lv_color_hex(palette.deep),LV_PART_SELECTED);
    lv_obj_set_style_bg_color(keys,lv_color_hex(palette.bg),0);
    lv_obj_set_style_bg_color(keys,lv_color_hex(palette.button),LV_PART_ITEMS);
    lv_obj_set_style_text_color(keys,lv_color_hex(palette.ink),LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keys,lv_color_hex(palette.on),LV_PART_ITEMS|LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keys,lv_color_hex(palette.deep),LV_PART_ITEMS|LV_STATE_PRESSED);
    lv_obj_set_style_opa(keys,LV_OPA_50,LV_STATE_DISABLED);
}
static void theme(void){
    if(!layer)return;
    lv_obj_t *panels[]={layer,list_page,edit_page,rows};
    for(unsigned i=0;i<sizeof panels/sizeof panels[0];i++)lv_obj_set_style_bg_color(panels[i],lv_color_hex(palette.bg),0);
    lv_obj_t *buttons[]={list_back,rescan,edit_cancel,submit,show};
    for(unsigned i=0;i<sizeof buttons/sizeof buttons[0];i++)theme_button(buttons[i]);
    for(uint32_t i=0;i<lv_obj_get_child_count(rows);i++)theme_button(lv_obj_get_child(rows,(int32_t)i));
    lv_obj_set_style_text_color(list_title,lv_color_hex(palette.ink),0);
    lv_obj_set_style_text_color(ssid_label,lv_color_hex(palette.ink),0);
    lv_obj_set_style_text_color(summary,lv_color_hex(summary_error?palette.ink:palette.sub),0);
    lv_obj_set_style_text_color(note,lv_color_hex(note_error?palette.ink:palette.sub),0);
    lv_obj_set_style_bg_color(rows,lv_color_hex(palette.sub),LV_PART_SCROLLBAR);
    theme_input();
}
void wifi_ui_set_palette(const ui_palette_t *value){
    if(value&&memcmp(&palette,value,sizeof palette)){palette=*value;theme();}
}
static void show_error(lv_obj_t *o,const char *message){
    if(o==note)note_error=true;else summary_error=true;
    lv_label_set_text(o,message);lv_obj_set_style_text_color(o,lv_color_hex(palette.ink),0);
}
static lv_obj_t *button(lv_obj_t *p,const char *s,int x,int y,int w,int h,lv_event_cb_t cb,intptr_t action) {
    lv_obj_t *o=lv_button_create(p); lv_obj_remove_style_all(o); lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x172a3a),0); lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_border_color(o,lv_color_hex(0x607787),0); lv_obj_set_style_border_width(o,1,0); lv_obj_set_style_radius(o,10,0);
    lv_obj_t *l=text(o,s,0,0,w-12); lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0); lv_obj_center(l);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,(void*)action);theme_button(o);return o;
}
/* Preserve unfamiliar Unicode identifiers as codepoints rather than blank glyphs. */
static void identifier(const char *input,char *out,size_t cap) {
    size_t used=0; const uint8_t *p=(const uint8_t*)input;
    while(*p && used+12<cap) {
        const uint8_t *start=p; uint32_t cp=*p++; unsigned continuation=0;
        if(cp>=0xc2 && cp<0xe0) { cp&=31; continuation=1; }
        else if(cp>=0xe0 && cp<0xf0) { cp&=15; continuation=2; }
        else if(cp>=0xf0 && cp<0xf5) { cp&=7; continuation=3; }
        else if(cp>=128) { used+=snprintf(out+used,cap-used,"[%02X]",*start); continue; }
        bool valid=true;
        for(unsigned i=0;i<continuation;i++) { if((*p&0xc0)!=0x80) { valid=false; break; } cp=(cp<<6)|(*p++&63); }
        lv_font_glyph_dsc_t gd;
        if(valid && cp>=32 && clock_font_28.get_glyph_dsc(&clock_font_28,&gd,cp,0)) { size_t n=p-start; memcpy(out+used,start,n); used+=n; }
        else used+=snprintf(out+used,cap-used,"[U+%04"PRIX32"]",cp);
    }
    out[used]=0;
}
static void close_ui(bool saved) {
    bool first=first_context,notify=visible;
    visible=false; editing=false; showing=false; synthetic=false; submitted=false;
    first_context=false;
    lv_textarea_set_password_mode(password,true); lv_textarea_set_text(password,""); wipe(&draft,sizeof draft);
    lv_obj_set_hidden(layer,true);
    if(notify&&completed)completed(first,saved);
}
static void enter_editor(void) {
    editing=true; submitted=false; showing=false; previous_phase=-1;note_error=false;
    char name[33]={0}; memcpy(name,draft.sta.ssid,32);
    identifier(name,ssid_text,sizeof ssid_text); lv_label_set_text(ssid_label,ssid_text);
    lv_textarea_set_text(password,""); lv_textarea_set_password_mode(password,true);
    lv_obj_set_hidden(keys,draft.sta.threshold.authmode==WIFI_AUTH_OPEN);
    lv_obj_set_hidden(password,draft.sta.threshold.authmode==WIFI_AUTH_OPEN);
    lv_obj_set_hidden(show,draft.sta.threshold.authmode==WIFI_AUTH_OPEN);
    lv_obj_set_hidden(list_page,true); lv_obj_set_hidden(edit_page,false);
    lv_label_set_text(note,draft.sta.threshold.authmode==WIFI_AUTH_OPEN?"开放网络 · 无需密码":"输入密码后连接并保存");
    lv_label_set_text(lv_obj_get_child(submit,0),"连接并保存"); lv_obj_remove_state(submit,LV_STATE_DISABLED);
    lv_obj_remove_state(password,LV_STATE_DISABLED); lv_obj_remove_state(keys,LV_STATE_DISABLED);
    lv_obj_set_style_text_color(note,lv_color_hex(palette.sub),0);
}
static void select_network(lv_event_t *e) {
    wifi_settings_state_t s=network_settings_read(); int i=(intptr_t)lv_event_get_user_data(e);
    if(i<0 || i>=s.count || s.phase!=WIFI_LIST) return;
    if(!s.choices[i].supported) { show_error(summary,"此认证类型暂不支持，请选择家庭网络"); return; }
    wipe(&draft,sizeof draft); memcpy(draft.sta.ssid,s.choices[i].ssid,32); draft.sta.threshold.authmode=s.choices[i].auth;
    draft.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH; draft.sta.pmf_cfg.capable=true; synthetic=false; enter_editor();
}
static void action(lv_event_t *e) {
    int a=(intptr_t)lv_event_get_user_data(e); wifi_settings_state_t s=network_settings_read();
    if(a==1) {
        if(s.phase==WIFI_SAVING)return;
        if(!network_settings_cancel()){
            show_error(editing?note:summary,"暂时无法取消，请重试");return;
        }
        close_ui(false);
    }
    else if(a==2) { if(!network_settings_scan())show_error(summary,"暂时无法扫描，请重试"); }
    else if(a==3) {
        if(submitted && s.phase==WIFI_SAVED) { close_ui(true); return; }
        if(submitted && (s.phase==WIFI_CONNECTING || s.phase==WIFI_SAVING || app_uptime()-submitted_at<500)) return;
        if(submitted && s.phase==WIFI_SAVE_FAILED) { network_settings_save_retry(); return; }
        const char *p=lv_textarea_get_text(password); size_t n=strlen(p);
        if(draft.sta.threshold.authmode!=WIFI_AUTH_OPEN) {
            bool hex=n==64;
            if(hex) for(size_t i=0;i<n;i++) if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')||(p[i]>='A'&&p[i]<='F'))) hex=false;
            if(!(n>=8 && n<=63) && !hex) { show_error(note,"密码需8至63个字符，或64位十六进制"); return; }
        }
        wipe(draft.sta.password,sizeof draft.sta.password); memcpy(draft.sta.password,p,n);
        if(network_settings_connect(&draft)) { submitted=true; submitted_at=app_uptime(); previous_phase=-1; lv_label_set_text(note,"正在连接…"); lv_obj_add_state(submit,LV_STATE_DISABLED); }
        else show_error(note,"暂时无法连接，请重试");
        wipe(draft.sta.password,sizeof draft.sta.password);
    } else if(a==4) {
        showing=!showing; lv_textarea_set_password_mode(password,!showing);
        lv_label_set_text(lv_obj_get_child(show,0),showing?"隐藏":"显示");
    }
}
static void key(lv_event_t *e) {
    const char *s=lv_buttonmatrix_get_button_text(keys,lv_buttonmatrix_get_selected_button(keys)); if(!s) return;
    key_events++; if(lv_indev_active()==bsp_display_get_input_dev()) physical_keys++;
    if(strcmp(s,"ABC")==0 || strcmp(s,"abc")==0 || strcmp(s,"123")==0) {
        keyboard_mode=strcmp(s,"ABC")==0?1:strcmp(s,"123")==0?2:0;
        lv_buttonmatrix_set_map(keys,keyboard_mode==0?lower:keyboard_mode==1?upper:symbols);
    } else if(strcmp(s,"Del")==0) lv_textarea_delete_char(password);
    else if(strcmp(s,"左")==0) lv_textarea_cursor_left(password);
    else if(strcmp(s,"右")==0) lv_textarea_cursor_right(password);
    else lv_textarea_add_text(password,strcmp(s,"space")==0?" ":s);
}
void wifi_ui_set_done(void (*done)(bool first,bool saved)){completed=done;}
void wifi_ui_open(void){wifi_ui_open_context(false);}
void wifi_ui_open_context(bool first) {
    first_context=first;
    visible=true; editing=false; synthetic=false; submitted=false; scan_revision=UINT32_MAX;
    lv_obj_set_hidden(layer,false); lv_obj_set_hidden(list_page,false); lv_obj_set_hidden(edit_page,true);
    lv_obj_move_foreground(layer); network_settings_scan(); wifi_ui_refresh();
}
void wifi_ui_create(lv_obj_t *screen) {
    layer=panel(screen,0,0,1024,600); list_page=panel(layer,0,0,1024,600);
    list_back=button(list_page,"返回",32,24,150,64,action,1);list_title=text(list_page,"Wi-Fi",220,26,450);
    rescan=button(list_page,"重新扫描",790,24,202,64,action,2);
    summary=text(list_page,"正在扫描…",32,102,960);
    rows=panel(list_page,32,158,960,416); lv_obj_set_scrollable(rows,true); lv_obj_set_scroll_dir(rows,LV_DIR_VER);
    edit_page=panel(layer,0,0,1024,600);edit_cancel=button(edit_page,"取消",24,16,140,60,action,1);
    ssid_label=text(edit_page,"",186,16,560); lv_obj_set_height(ssid_label,88); lv_label_set_long_mode(ssid_label,LV_LABEL_LONG_WRAP);
    submit=button(edit_page,"连接并保存",768,16,232,60,action,3);
    password=lv_textarea_create(edit_page); lv_obj_set_pos(password,24,112); lv_obj_set_size(password,744,66);
    lv_obj_set_style_text_font(password,&clock_font_28,0); lv_obj_set_style_text_color(password,lv_color_hex(0xc4d1d8),0);
    lv_obj_set_style_bg_color(password,lv_color_hex(0x172a3a),0); lv_textarea_set_one_line(password,true);
    lv_textarea_set_max_length(password,64); lv_textarea_set_password_mode(password,true); lv_textarea_set_password_bullet(password,"*");
    lv_textarea_set_password_show_time(password,0);
    show=button(edit_page,"显示",790,112,210,66,action,4);
    note=text(edit_page,"",24,202,976);
    keys=lv_buttonmatrix_create(edit_page); lv_obj_set_pos(keys,16,310); lv_obj_set_size(keys,992,280);
    lv_buttonmatrix_set_map(keys,lower); lv_obj_set_style_text_font(keys,&clock_font_28,LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keys,lv_color_black(),LV_PART_MAIN); lv_obj_set_style_border_width(keys,0,LV_PART_MAIN);
    lv_obj_set_style_bg_color(keys,lv_color_hex(0x172a3a),LV_PART_ITEMS); lv_obj_set_style_text_color(keys,lv_color_hex(0xc4d1d8),LV_PART_ITEMS);
    lv_obj_add_event_cb(keys,key,LV_EVENT_VALUE_CHANGED,NULL);lv_obj_set_hidden(layer,true);theme();
}
void wifi_ui_refresh(void) {
    if(!visible) return;
    wifi_settings_state_t s=network_settings_read();
    if(!editing) {
        summary_error=s.save_unconfirmed||s.storage_error||s.error;
        lv_obj_set_style_text_color(summary,lv_color_hex(summary_error?palette.ink:palette.sub),0);
        if(s.save_unconfirmed) lv_label_set_text(summary,"未能确认保存，可重新选择网络");
        else if(s.phase==WIFI_SCANNING) lv_label_set_text(summary,"正在扫描…");
        else if(s.storage_error) lv_label_set_text(summary,"无法读取保存设置，仍可重新配网");
        else if(s.error) lv_label_set_text(summary,"扫描未完成，请重试");
        else lv_label_set_text(summary,s.saved_present?"已有保存网络 · 选择网络可更改":"选择家庭网络");
        if(s.scan_revision!=scan_revision) {
            scan_revision=s.scan_revision; lv_obj_clean(rows);
            for(int i=0;i<s.count;i++) {
                char name[256],row[320]; identifier(s.choices[i].ssid,name,sizeof name);
                const char *auth=!s.choices[i].supported?"暂不支持":s.choices[i].auth==WIFI_AUTH_OPEN?"开放":s.choices[i].auth==WIFI_AUTH_WPA3_PSK?"WPA3":"WPA / WPA2";
                snprintf(row,sizeof row,"%s\n%s   %s",name,auth,s.choices[i].rssi>=-60?"信号强":s.choices[i].rssi>=-75?"信号中":"信号弱");
                button(rows,row,0,i*108,936,98,select_network,i);
            }
        }
        return;
    }
    bool time_valid=app_network_read().time_valid;
    if(!submitted || app_uptime()-submitted_at<300 || (s.phase==previous_phase && time_valid==rendered_time_valid)) return;
    previous_phase=s.phase; rendered_time_valid=time_valid;
    bool busy=s.phase==WIFI_CONNECTING || s.phase==WIFI_SAVING;
    if(busy) { lv_obj_add_state(submit,LV_STATE_DISABLED); lv_obj_add_state(password,LV_STATE_DISABLED); lv_obj_add_state(keys,LV_STATE_DISABLED); }
    else { lv_obj_remove_state(submit,LV_STATE_DISABLED); lv_obj_remove_state(password,LV_STATE_DISABLED); lv_obj_remove_state(keys,LV_STATE_DISABLED); }
    const char *message="连接未完成，请重试",*main="重试连接";
    if(s.phase==WIFI_CONNECTING) message="正在连接…";
    else if(s.phase==WIFI_SAVING) message="已连接，正在保存…";
    else if(s.phase==WIFI_SAVED) { close_ui(true);return; }
    else if(s.phase==WIFI_SAVE_FAILED) { message="未能确认保存，可重试"; main="重试保存"; lv_obj_add_state(password,LV_STATE_DISABLED); lv_obj_add_state(keys,LV_STATE_DISABLED); }
    else if(s.phase==WIFI_FAILED) message=s.save_unconfirmed?"连接未完成 · 未能确认保存":"连接失败，请检查密码或信号";
    note_error=s.phase==WIFI_SAVE_FAILED||s.phase==WIFI_FAILED;
    lv_obj_set_style_text_color(note,lv_color_hex(note_error?palette.ink:palette.sub),0);
    lv_label_set_text(note,message); lv_label_set_text(lv_obj_get_child(submit,0),main);
}
bool wifi_ui_visible(void) { return visible; }
bool wifi_ui_private(void) { return visible && !synthetic; }
bool wifi_ui_snapshot_allowed(void) { return !visible || synthetic; }
void wifi_ui_status(void) {
    wifi_settings_state_t s=network_settings_read();
    printf(",\"wifi_page\":%s,\"wifi_edit\":%s,\"wifi_phase\":%d,\"wifi_error\":%d,\"store_error\":%d,\"saved_present\":%s,\"boot_loaded\":%s,\"saved_revision\":%"PRIu32",\"wifi_attempt\":%"PRIu32",\"scan_revision\":%"PRIu32",\"scan_count\":%d,\"password_length\":%u,\"password_hidden\":%s,\"keyboard_mode\":%u,\"key_events\":%u,\"physical_keys\":%u",
        visible?"true":"false",editing?"true":"false",s.phase,s.error,s.storage_error,s.saved_present?"true":"false",s.boot_loaded?"true":"false",s.saved_revision,s.attempt,s.scan_revision,s.count,(unsigned)strlen(lv_textarea_get_text(password)),showing?"false":"true",keyboard_mode,key_events,physical_keys);
    printf(",\"keyboard_hidden\":%s,\"keyboard_state\":%u,\"keyboard_x\":%d,\"keyboard_y\":%d,\"keyboard_width\":%d,\"keyboard_height\":%d",
        lv_obj_is_hidden(keys)?"true":"false",(unsigned)lv_obj_get_state(keys),(int)lv_obj_get_x(keys),(int)lv_obj_get_y(keys),(int)lv_obj_get_width(keys),(int)lv_obj_get_height(keys));
    printf(",\"keyboard_visible\":%s,\"keyboard_opa\":%u,\"first_key_hidden\":%s",lv_obj_is_visible(keys)?"true":"false",(unsigned)lv_obj_get_style_opa(keys,LV_PART_MAIN),lv_buttonmatrix_has_button_ctrl(keys,0,LV_BUTTONMATRIX_CTRL_HIDDEN)?"true":"false");
}
#if CONFIG_CLOCK_DIAGNOSTICS
static int hex(char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; }
bool wifi_ui_command(char *line) {
    unsigned rid; char op[24]; if(sscanf(line,"%u %23s",&rid,op)!=2) return false;
    if(strcmp(op,"wifi_demo")==0) {
        network_settings_cancel(); wipe(&draft,sizeof draft); memcpy(draft.sta.ssid,"家庭网络-Long-SSID-123456789",32);
        draft.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK; visible=true; synthetic=true; lv_obj_set_hidden(layer,false); lv_obj_move_foreground(layer); enter_editor(); return true;
    }
    if(strcmp(op,"savefail")==0) { int value; if(sscanf(line,"%u %23s %d",&rid,op,&value)!=3 || value<0 || value>1) return false; network_settings_fail_save(value); return true; }
    if(strcmp(op,"wifi_fill")==0) {
        char name[65]={0},pass[129]={0}; int count=sscanf(line,"%u %23s %64s %128s",&rid,op,name,pass); bool ok=count==4;
        uint8_t ssid[33]={0},secret[65]={0}; size_t a=strlen(name),b=strlen(pass);
        if(!a || a>64 || a%2 || b>128 || b%2) ok=false;
        if(ok) {
            for(size_t i=0;i<a;i+=2) { int x=hex(name[i]),y=hex(name[i+1]); if(x<0||y<0) {ok=false;break;} ssid[i/2]=(x<<4)|y; }
            for(size_t i=0;i<b;i+=2) { int x=hex(pass[i]),y=hex(pass[i+1]); if(x<0||y<0) {ok=false;break;} secret[i/2]=(x<<4)|y; }
        }
        if(ok) {
            wifi_settings_state_t s=network_settings_read(); int found=-1;
            for(int i=0;i<s.count;i++) if(memcmp(ssid,s.choices[i].ssid,32)==0 && s.choices[i].supported) {found=i;break;}
            bool fake=synthetic && strcmp((char*)ssid,"synthetic-network")==0;
            ok=found>=0 || fake;
            if(ok) {
                wipe(&draft,sizeof draft); memcpy(draft.sta.ssid,ssid,32); draft.sta.threshold.authmode=fake?WIFI_AUTH_WPA2_PSK:s.choices[found].auth;
                draft.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH; draft.sta.pmf_cfg.capable=true;
                visible=true; synthetic=fake; lv_obj_set_hidden(layer,false); lv_obj_move_foreground(layer); enter_editor(); lv_textarea_set_text(password,(char*)secret);
            }
        }
        wipe(name,sizeof name); wipe(pass,sizeof pass); wipe(ssid,sizeof ssid); wipe(secret,sizeof secret); return ok;
    }
    return false;
}
#endif
