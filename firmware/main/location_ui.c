#include "location_ui.h"
#include "location_service.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
extern const lv_font_t clock_font_28;
extern lv_pinyin_dict_t clock_pinyin_dict[];
static lv_font_t keyboard_font;
static lv_obj_t *layer,*edit,*confirm,*sources,*query,*composition,*chinese,*english,*ime,*candidates,*note,*rows,*details,*save,*cancel,*mode,*error_note,*source_text;
static lv_obj_t *detail_scroll,*source_scroll,*confirm_title,*source_title,*source_back,*source_info;
static ui_palette_t palette=UI_PALETTE_NEUTRAL;
static lv_timer_t *debounce,*result_timer;
static bool visible,latin=true,submitted,suppress,note_error;
static bool first_context;
static void (*completed)(bool first,bool saved);
static lv_obj_t *close_button;
static uint32_t generation,rendered=UINT32_MAX;
static unsigned selected,key_events,candidate_events;
static int stage,source_return; /* 0 input + full-place results, 2 confirm, 3 source */
static location_record_t draft,row_scratch;
static location_state_t snapshot;
/* LVGL-lock-owned scratch; no full records or expanded text on its task stack. */
static char raw[2400],shown[8000];
static const char *pinyin_map[]={"q","w","e","r","t","y","u","i","o","p","\n","a","s","d","f","g","h","j","k","l","\n","z","x","c","v","b","n","m",LV_SYMBOL_BACKSPACE,LV_SYMBOL_NEW_LINE,""};
static lv_buttonmatrix_ctrl_t pinyin_ctrl[28],latin_ctrl[32],number_ctrl[26];
static const char *latin_lower[]={"q","w","e","r","t","y","u","i","o","p",LV_SYMBOL_BACKSPACE,"\n","a","s","d","f","g","h","j","k","l"," ","\n","ABC","z","x","c","v","b","n","m","1#",LV_SYMBOL_LEFT,LV_SYMBOL_RIGHT,""};
static const char *latin_upper[]={"Q","W","E","R","T","Y","U","I","O","P",LV_SYMBOL_BACKSPACE,"\n","A","S","D","F","G","H","J","K","L"," ","\n","abc","Z","X","C","V","B","N","M","1#",LV_SYMBOL_LEFT,LV_SYMBOL_RIGHT,""};
static const char *latin_numbers[]={"1","2","3","4","5","6","7","8","9","0",LV_SYMBOL_BACKSPACE,"\n","-","_","/","(",")","'",".",","," ","\n","abc",LV_SYMBOL_LEFT,LV_SYMBOL_RIGHT,""};
static lv_obj_t *panel(lv_obj_t *p,int x,int y,int w,int h) {
    lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.bg),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_scrollable(o,false);return o;
}
static lv_obj_t *text(lv_obj_t *p,const char *s,int x,int y,int w) {
    lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,&clock_font_28,0);lv_obj_set_style_text_color(o,lv_color_hex(palette.ink),0);lv_obj_set_clickable(o,false);return o;
}
static void theme_button(lv_obj_t *o){
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.button),0);
    lv_obj_set_style_border_color(o,lv_color_hex(palette.edge),0);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.deep),LV_STATE_PRESSED);
    lv_obj_set_style_border_color(o,lv_color_hex(palette.on),LV_STATE_PRESSED);
    lv_obj_set_style_opa(o,LV_OPA_50,LV_STATE_DISABLED);
    lv_obj_set_style_text_color(lv_obj_get_child(o,0),lv_color_hex(palette.ink),0);
}
static void theme_keyboard(lv_obj_t *o){
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.bg),0);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.button),LV_PART_ITEMS);
    lv_obj_set_style_text_color(o,lv_color_hex(palette.ink),LV_PART_ITEMS);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.on),LV_PART_ITEMS|LV_STATE_PRESSED);
    lv_obj_set_style_text_color(o,lv_color_hex(palette.deep),LV_PART_ITEMS|LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.on),LV_PART_ITEMS|LV_STATE_CHECKED);
    lv_obj_set_style_text_color(o,lv_color_hex(palette.deep),LV_PART_ITEMS|LV_STATE_CHECKED);
    lv_obj_set_style_opa(o,LV_OPA_50,LV_STATE_DISABLED);
}
static void theme_input(lv_obj_t *o){
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.button),0);
    lv_obj_set_style_text_color(o,lv_color_hex(palette.ink),0);
    lv_obj_set_style_border_color(o,lv_color_hex(palette.edge),0);
    lv_obj_set_style_border_color(o,lv_color_hex(palette.on),LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.ink),LV_PART_CURSOR);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.on),LV_PART_SELECTED);
    lv_obj_set_style_text_color(o,lv_color_hex(palette.deep),LV_PART_SELECTED);
    lv_obj_set_style_text_color(o,lv_color_hex(palette.sub),LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(o,lv_color_hex(palette.deep),LV_STATE_DISABLED);
    lv_obj_set_style_text_color(o,lv_color_hex(palette.sub),LV_PART_TEXTAREA_PLACEHOLDER);
}
static lv_obj_t *button(lv_obj_t *p,const char *s,int x,int y,int w,int h,lv_event_cb_t cb,intptr_t action) {
    lv_obj_t *o=panel(p,x,y,w,h);lv_obj_set_clickable(o,true);lv_obj_set_style_radius(o,10,0);lv_obj_set_style_bg_color(o,lv_color_hex(0x172a3a),0);
    lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(0x607787),0);
    lv_obj_t *l=text(o,s,12,6,w-24);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,(void*)action);theme_button(o);return o;
}
static void theme(void){
    if(!layer)return;
    lv_obj_t *panels[]={layer,edit,confirm,sources,rows,detail_scroll,source_scroll};
    for(unsigned i=0;i<sizeof panels/sizeof panels[0];i++){
        lv_obj_set_style_bg_color(panels[i],lv_color_hex(palette.bg),0);
        lv_obj_set_style_bg_color(panels[i],lv_color_hex(palette.sub),LV_PART_SCROLLBAR);
    }
    lv_obj_t *buttons[]={close_button,mode,cancel,save,source_back,source_info};
    for(unsigned i=0;i<sizeof buttons/sizeof buttons[0];i++)theme_button(buttons[i]);
    lv_obj_t *labels[]={confirm_title,source_title,details,error_note};
    for(unsigned i=0;i<sizeof labels/sizeof labels[0];i++)lv_obj_set_style_text_color(labels[i],lv_color_hex(palette.ink),0);
    lv_obj_set_style_text_color(note,lv_color_hex(note_error?palette.ink:palette.sub),0);
    lv_obj_set_style_text_color(source_text,lv_color_hex(palette.sub),0);
    for(uint32_t i=0;i<lv_obj_get_child_count(rows);i++){
        lv_obj_t *row=lv_obj_get_child(rows,(int32_t)i);
        lv_obj_set_style_bg_color(row,lv_color_hex(palette.button),0);
        lv_obj_set_style_bg_color(row,lv_color_hex(palette.deep),LV_STATE_PRESSED);
        lv_obj_set_style_text_color(lv_obj_get_child(row,0),lv_color_hex(palette.ink),0);
        lv_obj_set_style_text_color(lv_obj_get_child(row,1),lv_color_hex(palette.sub),0);
    }
    theme_input(query);theme_input(composition);
    theme_keyboard(english);theme_keyboard(chinese);theme_keyboard(candidates);
}
void location_ui_set_palette(const ui_palette_t *value){
    if(value&&memcmp(&palette,value,sizeof palette)){palette=*value;theme();}
}
static void display(const char *s,char *out,size_t capacity) {
    size_t used=0;const unsigned char *p=(const unsigned char*)s;
    while(*p&&used+16<capacity){const unsigned char *start=p;uint32_t cp=*p++;unsigned n=0;
        if(cp>=0xf0){cp&=7;n=3;}else if(cp>=0xe0){cp&=15;n=2;}else if(cp>=0xc0){cp&=31;n=1;}
        while(n--)cp=(cp<<6)|(*p++&63);
        lv_font_glyph_dsc_t gd;
        if(cp=='\n'||clock_font_28.get_glyph_dsc(&clock_font_28,&gd,cp,0)){size_t bytes=p-start;memcpy(out+used,start,bytes);used+=bytes;}
        else used+=snprintf(out+used,capacity-used,"[U+%04"PRIX32"]",cp);
    }out[used]=0;
}
static void reset_composition(void) {
    uint32_t id=0;const char *s;
    while((s=lv_buttonmatrix_get_button_text(chinese,id))){if(!strcmp(s,LV_SYMBOL_NEW_LINE)||!strcmp(s,"Enter"))break;id++;}
    if(s){lv_buttonmatrix_set_selected_button(chinese,id);lv_obj_send_event(chinese,LV_EVENT_VALUE_CHANGED,NULL);}
    lv_textarea_set_text(composition,"");lv_obj_set_hidden(candidates,true);
}
static void show_stage(int value) {
    stage=value;lv_obj_set_hidden(edit,value!=0);lv_obj_set_hidden(confirm,value!=2);lv_obj_set_hidden(sources,value!=3);
    lv_obj_set_hidden(candidates,true);
}
static void close_ui(bool saved) {
    bool first=first_context,notify=visible;
    lv_timer_pause(debounce);lv_timer_pause(result_timer);location_cancel();visible=false;submitted=false;reset_composition();
    suppress=true;lv_textarea_set_text(query,"");suppress=false;memset(&draft,0,sizeof draft);lv_obj_set_hidden(layer,true);
    first_context=false;
    if(notify&&completed)completed(first,saved);
}
static void launch(lv_timer_t *timer) {
    (void)timer;lv_timer_pause(debounce);
    if(!visible||stage!=0)return;
    const char *q=lv_textarea_get_text(query);
    if(!q[0]){lv_label_set_text(note,"输入全拼、简拼或英文");return;}
    generation=location_search(q);rendered=UINT32_MAX;
    note_error=!generation;lv_obj_set_style_text_color(note,lv_color_hex(note_error?palette.ink:palette.sub),0);
    lv_label_set_text(note,generation?"正在匹配…":"输入过长或暂不可用");
}
static void query_changed(lv_event_t *event) {
    (void)event;if(suppress||!visible||stage!=0)return;
    location_cancel();generation=0;rendered=UINT32_MAX;lv_obj_clean(rows);
    lv_timer_reset(debounce);lv_timer_resume(debounce);
}
static void confirm_text(void) {
    if(!location_display_context(&draft,shown,sizeof shown))snprintf(shown,sizeof shown,"%s",draft.admin[0]);
    const char *basis=draft.version==2?location_weather_date_basis(draft.timezone):NULL;
    /* Four full admin fields plus country/separators fit below 1024 bytes. */
    snprintf(raw,sizeof raw,"%s%s%.1024s\n\n天气日期%s%s\n仅更改天气位置，主钟保持北京时间。\n天气为所选地域的参考数据。",draft.name,shown[0]?"\n":"",shown,basis?"使用":"以返回数据为准",basis?basis:"");
    display(raw,shown,sizeof shown);lv_label_set_text(details,shown);lv_obj_scroll_to_y(lv_obj_get_parent(details),0,LV_ANIM_OFF);
}
static void selected_result(lv_event_t *event) {
    unsigned index=(intptr_t)lv_event_get_user_data(event);
    if(!location_result(index,generation,&draft))return;
    lv_timer_pause(debounce);selected=index;show_stage(2);submitted=false;confirm_text();
    lv_label_set_text(error_note,"");lv_obj_remove_state(save,LV_STATE_DISABLED);lv_obj_remove_state(cancel,LV_STATE_DISABLED);
    lv_label_set_text(lv_obj_get_child(save,0),"保存位置");
}
static void action(lv_event_t *event) {
    int a=(intptr_t)lv_event_get_user_data(event);snapshot=location_read();
    if(a==1){if(!snapshot.saving)close_ui(false);}
    else if(a==3&&!snapshot.saving){show_stage(0);rendered=UINT32_MAX;submitted=false;}
    else if(a==4){if(snapshot.saving||submitted)return;if(location_save(selected,generation)){submitted=true;lv_obj_add_state(save,LV_STATE_DISABLED);lv_obj_add_state(cancel,LV_STATE_DISABLED);lv_label_set_text(lv_obj_get_child(save,0),"正在保存…");}}
    else if(a==5){reset_composition();latin=!latin;lv_obj_set_hidden(chinese,latin);lv_obj_set_hidden(composition,latin);lv_obj_set_hidden(english,!latin);lv_obj_set_height(rows,latin?258:170);
        lv_label_set_text(lv_obj_get_child(mode,0),latin?"切换汉字":"全拼 / 英文");}
    else if(a==7){
        source_return=stage;show_stage(3);
        snprintf(raw,sizeof raw,"地点数据：GeoNames(CC BY 4.0)、Wikidata(CC0)\n内政部地政司2026行政地名(OGDL 1.0)\n本机整理、关联与拼音索引；地域代表点不是精确住址。\nhttps://www.geonames.org/\nhttps://creativecommons.org/licenses/by/4.0/\nhttps://www.wikidata.org/wiki/Wikidata:Licensing\nhttps://data.gov.tw/dataset/40281\nhttps://data.gov.tw/license\n%s%s",source_return==2?"\n所选地点数据：\n":"",source_return==2?draft.source:"");
        if(source_return==2){size_t n=strlen(raw);snprintf(raw+n,sizeof raw-n,"\n%s\n%.5f, %.5f\n天气时区 %s\n来源时区 %s\n%s\n%s",draft.source_id,draft.latitude,draft.longitude,draft.timezone,draft.source_timezone[0]?draft.source_timezone:"原来源缺失",draft.source_version,draft.point_purpose);}
        display(raw,shown,sizeof shown);lv_label_set_text(source_text,shown);
    }else if(a==8){show_stage(source_return);rendered=UINT32_MAX;}
}
static void guard(lv_event_t *event) {
    const char *s=lv_buttonmatrix_get_button_text(chinese,lv_buttonmatrix_get_selected_button(chinese));if(!s)return;
    key_events++;
    if((s[0]>='a'&&s[0]<='z'&&s[1]==0)&&strlen(lv_textarea_get_text(composition))>=6){lv_event_stop_processing(event);return;}
    if(!(s[0]>='a'&&s[0]<='z'&&s[1]==0)&&strcmp(s,LV_SYMBOL_BACKSPACE)&&strcmp(s,LV_SYMBOL_NEW_LINE)&&strcmp(s,"Enter"))lv_event_stop_processing(event);
}
static void candidate_chosen(lv_event_t *event) {
    (void)event;const char *s=lv_textarea_get_text(composition);
    if((unsigned char)s[0]>=0xe0){lv_textarea_add_text(query,s);lv_textarea_set_text(composition,"");candidate_events++;}
}
static void check_candidates(lv_event_t *event) {
    (void)event;const char *py=lv_textarea_get_text(composition);bool match=false;
    if(py[0])for(unsigned i=0;clock_pinyin_dict[i].py;i++)if(!strncmp(clock_pinyin_dict[i].py,py,strlen(py))){match=true;break;}
    if(!match)lv_obj_set_hidden(candidates,true);
}
static void latin_key(lv_event_t *event){(void)event;key_events++;}
static void style_keys(lv_obj_t *o) {
    lv_obj_set_style_text_font(o,&keyboard_font,LV_PART_ITEMS);lv_obj_set_style_bg_color(o,lv_color_black(),0);lv_obj_set_style_border_width(o,0,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x172a3a),LV_PART_ITEMS);lv_obj_set_style_text_color(o,lv_color_hex(0xc4d1d8),LV_PART_ITEMS);
    lv_obj_set_style_pad_all(o,2,0);lv_obj_set_style_pad_row(o,2,0);
    theme_keyboard(o);
}
static void result_tick(lv_timer_t *timer){(void)timer;location_ui_refresh();}
void location_ui_create(lv_obj_t *screen) {
    keyboard_font=clock_font_28;keyboard_font.fallback=&lv_font_montserrat_24;
    layer=panel(screen,0,0,1024,600);edit=panel(layer,0,0,1024,600);confirm=panel(layer,0,0,1024,600);sources=panel(layer,0,0,1024,600);
    close_button=button(edit,"取消",24,16,140,52,action,1);note=text(edit,"输入全拼、简拼或英文",200,22,570);lv_label_set_long_mode(note,LV_LABEL_LONG_DOT);
    mode=button(edit,"切换汉字",800,16,200,52,action,5);
    query=lv_textarea_create(edit);lv_obj_set_pos(query,24,78);lv_obj_set_size(query,976,54);lv_textarea_set_one_line(query,true);lv_textarea_set_max_length(query,64);
    lv_textarea_set_placeholder_text(query,"例如 beijing、bj 或 Beijing");lv_obj_set_style_text_font(query,&clock_font_28,0);
    lv_obj_set_style_pad_ver(query,4,0);
    lv_obj_set_style_bg_color(query,lv_color_hex(0x172a3a),0);lv_obj_set_style_text_color(query,lv_color_hex(0xc4d1d8),0);
    rows=panel(edit,24,142,976,258);lv_obj_set_scrollable(rows,true);lv_obj_set_scroll_dir(rows,LV_DIR_VER);
    english=lv_keyboard_create(edit);lv_obj_set_align(english,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(english,16,410);lv_obj_set_size(english,992,180);style_keys(english);lv_keyboard_set_textarea(english,query);
    for(unsigned i=0;i<32;i++)latin_ctrl[i]=1;
    latin_ctrl[20]=3;
    for(unsigned i=0;i<26;i++)number_ctrl[i]=1;
    number_ctrl[19]=3;
    lv_keyboard_set_map(english,LV_KEYBOARD_MODE_TEXT_LOWER,latin_lower,latin_ctrl);lv_keyboard_set_map(english,LV_KEYBOARD_MODE_TEXT_UPPER,latin_upper,latin_ctrl);
    lv_keyboard_set_map(english,LV_KEYBOARD_MODE_SPECIAL,latin_numbers,number_ctrl);lv_keyboard_set_mode(english,LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(english,latin_key,LV_EVENT_VALUE_CHANGED,NULL);
    composition=lv_textarea_create(edit);lv_obj_set_pos(composition,24,330);lv_obj_set_size(composition,976,54);lv_textarea_set_one_line(composition,true);lv_textarea_set_max_length(composition,6);
    lv_obj_set_style_text_font(composition,&clock_font_28,0);lv_obj_set_style_bg_color(composition,lv_color_hex(0x172a3a),0);lv_obj_set_style_text_color(composition,lv_color_hex(0xc4d1d8),0);
    lv_obj_set_style_pad_ver(composition,4,0);
    chinese=lv_keyboard_create(edit);lv_obj_set_align(chinese,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(chinese,16,450);lv_obj_set_size(chinese,992,140);style_keys(chinese);
    for(unsigned i=0;i<28;i++)pinyin_ctrl[i]=1;
    lv_keyboard_set_map(chinese,LV_KEYBOARD_MODE_USER_1,pinyin_map,pinyin_ctrl);lv_keyboard_set_mode(chinese,LV_KEYBOARD_MODE_USER_1);
    lv_obj_remove_event_cb(chinese,lv_keyboard_def_event_cb);lv_obj_add_event_cb(chinese,guard,LV_EVENT_VALUE_CHANGED,NULL);lv_obj_add_event_cb(chinese,lv_keyboard_def_event_cb,LV_EVENT_VALUE_CHANGED,NULL);
    ime=lv_ime_pinyin_create(edit);lv_ime_pinyin_set_dict(ime,clock_pinyin_dict);lv_obj_set_style_text_font(ime,&clock_font_28,0);lv_ime_pinyin_set_keyboard(ime,chinese);lv_keyboard_set_textarea(chinese,composition);
    candidates=lv_ime_pinyin_get_cand_panel(ime);lv_obj_set_align(candidates,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(candidates,16,390);lv_obj_set_size(candidates,992,54);style_keys(candidates);
    lv_obj_add_event_cb(candidates,candidate_chosen,LV_EVENT_VALUE_CHANGED,NULL);lv_obj_add_event_cb(chinese,check_candidates,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(query,query_changed,LV_EVENT_VALUE_CHANGED,NULL);debounce=lv_timer_create(launch,80,NULL);lv_timer_pause(debounce);
    result_timer=lv_timer_create(result_tick,40,NULL);lv_timer_pause(result_timer);
    confirm_title=text(confirm,"确认天气位置",24,24,800);detail_scroll=panel(confirm,24,94,976,340);lv_obj_set_scrollable(detail_scroll,true);details=text(detail_scroll,"",12,0,928);
    error_note=text(confirm,"",24,446,976);cancel=button(confirm,"返回候选",24,522,230,60,action,3);
    source_info=button(confirm,"数据说明",414,522,200,60,action,7);save=button(confirm,"保存位置",770,522,230,60,action,4);
    source_back=button(sources,"返回",24,16,180,60,action,8);source_title=text(sources,"地点来源与代表点",240,24,730);
    source_scroll=panel(sources,24,104,976,470);lv_obj_set_scrollable(source_scroll,true);source_text=text(source_scroll,"",12,0,928);
    lv_obj_set_hidden(chinese,true);lv_obj_set_hidden(composition,true);lv_obj_set_hidden(candidates,true);lv_obj_set_hidden(layer,true);theme();
}
void location_ui_set_done(void (*done)(bool first,bool saved)){completed=done;}
void location_ui_open(void){location_ui_open_context(false);}
void location_ui_open_context(bool first) {
    first_context=first;note_error=false;lv_obj_set_style_text_color(note,lv_color_hex(palette.sub),0);lv_label_set_text(lv_obj_get_child(close_button,0),first?"稍后":"取消");
    visible=true;submitted=false;latin=true;generation=0;rendered=UINT32_MAX;location_cancel();
    lv_obj_set_hidden(layer,false);lv_obj_move_foreground(layer);show_stage(0);reset_composition();suppress=true;lv_textarea_set_text(query,"");suppress=false;lv_obj_clean(rows);
    lv_obj_set_hidden(chinese,true);lv_obj_set_hidden(composition,true);lv_obj_set_hidden(english,false);lv_obj_set_height(rows,258);
    lv_label_set_text(lv_obj_get_child(mode,0),"切换汉字");lv_label_set_text(note,"输入全拼、简拼或英文");
    lv_timer_resume(result_timer);
}
void location_ui_refresh(void) {
    if(!visible)return;
    snapshot=location_read();
    if(stage==2&&submitted){
        if(snapshot.phase==LOC_SAVED){close_ui(true);return;}
        if(snapshot.phase==LOC_SAVE_FAILED){submitted=false;lv_label_set_text(lv_obj_get_child(save,0),"重试保存");lv_obj_remove_state(save,LV_STATE_DISABLED);lv_obj_remove_state(cancel,LV_STATE_DISABLED);lv_label_set_text(error_note,"未能确认保存，草稿保留，可重试或返回。");}
    }
    if(stage!=0||snapshot.generation!=generation)return;
    uint32_t signature=(generation<<8)|snapshot.phase;if(signature==rendered)return;rendered=signature;
    note_error=snapshot.phase!=LOC_RESULTS&&snapshot.phase!=LOC_SEARCHING&&snapshot.phase!=LOC_EMPTY;
    lv_obj_set_style_text_color(note,lv_color_hex(note_error?palette.ink:palette.sub),0);
    lv_obj_clean(rows);
    if(snapshot.phase==LOC_RESULTS){lv_label_set_text(note,snapshot.total>8?"请选择地点，或继续输入缩小范围":"选择地点，再确认保存");
        for(unsigned i=0;i<snapshot.count;i++){
            if(!location_result(i,generation,&row_scratch))break;
            lv_obj_t *b=panel(rows,0,i*86,952,84);lv_obj_set_clickable(b,true);lv_obj_set_style_bg_color(b,lv_color_hex(palette.button),0);lv_obj_set_style_bg_color(b,lv_color_hex(palette.deep),LV_STATE_PRESSED);lv_obj_add_event_cb(b,selected_result,LV_EVENT_CLICKED,(void*)(intptr_t)i);
            display(row_scratch.name,shown,sizeof shown);lv_obj_t *name=text(b,shown,12,0,928);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);
            if(!location_display_context(&row_scratch,raw,sizeof raw))snprintf(raw,sizeof raw,"%s",row_scratch.admin[0]);
            display(raw,shown,sizeof shown);lv_obj_t *context=text(b,shown,12,42,928);lv_label_set_long_mode(context,LV_LABEL_LONG_DOT);lv_obj_set_style_text_color(context,lv_color_hex(palette.sub),0);
        }
    }else lv_label_set_text(note,snapshot.phase==LOC_SEARCHING?"正在匹配…":snapshot.phase==LOC_EMPTY?"没有匹配地点，请修改输入":!snapshot.catalog_ready?"地点库暂不可用":"暂时无法匹配，请修改或重试");
}
bool location_ui_visible(void){return visible;}
bool location_ui_private(void){
#if CONFIG_CLOCK_DIAGNOSTICS
    return visible&&!location_public_test();
#else
    return visible;
#endif
}
bool location_ui_snapshot_allowed(void) {
    if(location_ui_private())return false;
    snapshot=location_read();
#if CONFIG_CLOCK_DIAGNOSTICS
    return !snapshot.configured||location_public_test();
#else
    return !snapshot.configured;
#endif
}
void location_ui_saved_label(lv_obj_t *label) {
    snapshot=location_read();display(snapshot.configured?snapshot.saved.name:"未配置",shown,sizeof shown);
    if(strcmp(lv_label_get_text(label),shown))lv_label_set_text(label,shown);
}
void location_ui_saved_context(lv_obj_t *label) {
    snapshot=location_read();
    raw[0]=0;
    if(snapshot.configured&&!location_display_context(&snapshot.saved,raw,sizeof raw))snprintf(raw,sizeof raw,"%s",snapshot.saved.admin[0]);
    display(raw,shown,sizeof shown);if(strcmp(lv_label_get_text(label),shown))lv_label_set_text(label,shown);
}
void location_ui_saved_weather(lv_obj_t *name,lv_obj_t *context,lv_obj_t *details,int width) {
    snapshot=location_read();
    display(snapshot.configured?snapshot.saved.name:"未配置",shown,sizeof shown);
    if(strcmp(lv_label_get_text(name),shown))lv_label_set_text(name,shown);
    raw[0]=0;
    if(snapshot.configured&&!location_display_context(&snapshot.saved,raw,sizeof raw))snprintf(raw,sizeof raw,"%s",snapshot.saved.admin[0]);
    display(raw,shown,sizeof shown);
    if(strcmp(lv_label_get_text(context),shown))lv_label_set_text(context,shown);
    lv_point_t size;lv_text_get_size(&size,lv_label_get_text(name),&clock_font_28,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    bool has_context=shown[0]!=0;
    /* Keep a parent-region portion visible even with a very long place name.
     * Both strings retain their full identity; LVGL ellipsis is display only. */
    int name_width=has_context?(size.x<width-212?size.x:width-212):width;
    if(name_width<1)name_width=1;
    lv_obj_set_size(name,name_width,clock_font_28.line_height);
    lv_obj_set_x(context,lv_obj_get_x(name)+name_width+12);lv_obj_set_y(context,lv_obj_get_y(name));
    lv_obj_set_size(context,has_context?width-name_width-12:1,clock_font_28.line_height);lv_obj_set_hidden(context,!has_context);
    if(!details)return;
    const location_record_t *r=&snapshot.saved;
    if(!snapshot.configured)snprintf(raw,sizeof raw,"天气位置未配置");
    else snprintf(raw,sizeof raw,"完整地点身份：%s\n国家：%s\n行政区：%s / %s / %s / %s\n坐标：%.5f, %.5f\n位置ID：%"PRIu32"\n来源ID：%s\n天气位置时区：%s\n来源时区：%s\n来源：%s\n来源版本：%s\n目录版本：%s\n代表点：%s",
        r->name,r->country,r->admin[0],r->admin[1],r->admin[2],r->admin[3],r->latitude,r->longitude,r->id,
        r->source_id,r->timezone,r->source_timezone[0]?r->source_timezone:"原来源缺失",r->source,r->source_version,r->catalog_version,r->point_purpose);
    display(raw,shown,sizeof shown);if(strcmp(lv_label_get_text(details),shown))lv_label_set_text(details,shown);
}
void location_ui_status(void) {
    lv_obj_t *kb=latin?english:chinese;
    printf(",\"location_ui\":{\"visible\":%s,\"stage\":%d,\"latin\":%s,\"query_bytes\":%u,\"composition_bytes\":%u,\"key_events\":%u,\"candidate_events\":%u,\"keyboard_x\":%d,\"keyboard_y\":%d,\"keyboard_width\":%d,\"keyboard_height\":%d",visible?"true":"false",stage,latin?"true":"false",(unsigned)strlen(lv_textarea_get_text(query)),(unsigned)strlen(lv_textarea_get_text(composition)),key_events,candidate_events,(int)lv_obj_get_x(kb),(int)lv_obj_get_y(kb),(int)lv_obj_get_width(kb),(int)lv_obj_get_height(kb));
#if CONFIG_CLOCK_DIAGNOSTICS
    if(location_public_test()){printf(",\"query_hex\":\"");for(const unsigned char *p=(const unsigned char*)lv_textarea_get_text(query);*p;p++)printf("%02x",*p);printf("\"");}
#endif
    printf("}");
}
#if CONFIG_CLOCK_DIAGNOSTICS
bool location_ui_command(const char *line) {
    if(!location_public_test()||!visible||stage!=0)return false;
    unsigned id;char op[24],hex[LOCATION_QUERY_BYTES*2]={0},extra,value[LOCATION_QUERY_BYTES]={0};bool ok=false;
    if(sscanf(line,"%u %23s %513s %c",&id,op,hex,&extra)!=3||strcmp(op,"location_fill"))goto done;
    size_t n=strlen(hex);if(n%2||n/2>=LOCATION_QUERY_BYTES)goto done;
    for(size_t i=0;i<n;i+=2){unsigned byte=0;
        for(unsigned k=0;k<2;k++){unsigned char c=hex[i+k];unsigned nibble;
            if(c>='0'&&c<='9')nibble=c-'0';else if(c>='a'&&c<='f')nibble=c-'a'+10;else if(c>='A'&&c<='F')nibble=c-'A'+10;else goto done;
            byte=(byte<<4)|nibble;
        }
        if(!byte)goto done;
        value[i/2]=(char)byte;
    }
    if(!location_query_valid(value))goto done;
    reset_composition();lv_textarea_set_text(query,value);ok=true;
done:
    for(size_t i=0;i<sizeof value;i++)((volatile char*)value)[i]=0;
    for(size_t i=0;i<sizeof hex;i++)((volatile char*)hex)[i]=0;
    return ok;
}
#endif
