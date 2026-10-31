#include "alarm_ui.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static am_controller *model;
static am_ui_ports ports;
static lv_obj_t *root,*views[4],*notes[4],*clock_labels[4],*rows,*add,*reload[4];
static lv_obj_t *list_settings;
static lv_obj_t *hour,*minute,*keypad,*repeat_button,*enabled_button,*enabled_text,*save,*remove_button,*cancel_edit,*retry;
static lv_obj_t *repeat_options[3],*repeat_help,*edit_title,*delete_text,*delete_yes,*delete_no,*delete_back,*hour_error,*minute_error;
static lv_obj_t *row_edit[AM_CAPACITY],*row_switch[AM_CAPACITY],*row_state[AM_CAPACITY];
static const lv_font_t *body_font,*digit_font;
static uint32_t rendered=UINT32_MAX,seen_draft=UINT32_MAX,dispatched;
static uint32_t dispatched_at;
#define AM_REPLY_TIMEOUT_MS 8000u
static bool syncing;
static am_ui_list_palette list_palette={0x0b1420,0xc4d1d8,0x778994,0x344653,0x607787,0x607787,0xb4cbd0,0x071520,0x172a3a};
static bool list_palette_dirty=true;
static am_config rendered_config;
static bool rows_known;
static lv_obj_t *focused;
static const char *keys[]={"1","2","3","\n","4","5","6","\n","7","8","9","\n","退格","0","下一项",""};
static bool field_touched[2];

static void activity(lv_event_t *e){(void)e;if(ports.activity)ports.activity(ports.ctx);}
static lv_obj_t *panel(lv_obj_t *p,int x,int y,int w,int h){
    lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_scrollable(o,false);lv_obj_set_event_bubble(o,true);return o;
}
static lv_obj_t *label(lv_obj_t *p,const char *s,int x,int y,int w,const lv_font_t *font){
    lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,s);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(0xc4d1d8),0);
    lv_obj_set_clickable(o,false);return o;
}
static void set_disabled(lv_obj_t *o,bool disabled){if(disabled)lv_obj_add_state(o,LV_STATE_DISABLED);else lv_obj_remove_state(o,LV_STATE_DISABLED);}
static void action(lv_event_t *e);
static lv_obj_t *button(lv_obj_t *p,const char *s,int x,int y,int w,int action_id){
    lv_obj_t *o=panel(p,x,y,w,64);lv_obj_set_clickable(o,true);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x172a3a),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_radius(o,8,0);
    lv_obj_set_style_bg_opa(o,LV_OPA_30,LV_STATE_DISABLED);
    lv_obj_t *l=label(o,s,0,0,w,body_font);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(l);
    lv_obj_add_event_cb(o,action,LV_EVENT_CLICKED,(void*)(intptr_t)action_id);return o;
}
static void button_text(lv_obj_t *o,const char *text){lv_label_set_text(lv_obj_get_child(o,0),text);}
static void text_if_changed(lv_obj_t *o,const char *text){if(strcmp(lv_label_get_text(o),text))lv_label_set_text(o,text);}
static int number(const char *s){if(!*s)return -1;int n=0;while(*s){if(*s<'0'||*s>'9')return -1;n=n*10+*s++-'0';}return n;}
static void field_errors(void){
    lv_obj_t *fields[]={hour,minute},*errors[]={hour_error,minute_error};
    for(unsigned i=0;i<2;i++){
        const char *s=lv_textarea_get_text(fields[i]);int n=number(s),max=i?59:23;
        const char *message=!field_touched[i]?"":n<0?(i?"请输入分钟":"请输入小时"):n>max?(i?"请输入00-59":"请输入00-23"):"";
        text_if_changed(errors[i],message);
        lv_obj_set_style_border_color(fields[i],lv_color_hex(*message?list_palette.ink:list_palette.edge),0);
    }
}
static void update_time(lv_obj_t *field){
    if(syncing)return;
    field_touched[field==minute]=true;
    (void)am_edit_time(model,number(lv_textarea_get_text(hour)),number(lv_textarea_get_text(minute)));
    field_errors();
}
static void time_changed(lv_event_t *e){update_time(lv_event_get_target_obj(e));}
static void select_field(lv_obj_t *field){
    focused=field;lv_obj_remove_state(hour,LV_STATE_FOCUSED);lv_obj_remove_state(minute,LV_STATE_FOCUSED);
    lv_obj_add_state(field,LV_STATE_FOCUSED);lv_textarea_clear_selection(hour);lv_textarea_clear_selection(minute);
    lv_textarea_set_cursor_pos(field,LV_TEXTAREA_CURSOR_LAST);
    lv_obj_t *text=lv_textarea_get_label(field);
    lv_label_set_text_selection_start(text,0);lv_label_set_text_selection_end(text,(uint32_t)strlen(lv_textarea_get_text(field)));
}
static void select_time(lv_event_t *e){if(!model->busy&&!model->blocked)select_field(lv_event_get_target_obj(e));}
static void keypad_changed(lv_event_t *e){
    if(model->busy||model->blocked)return;
    uint32_t index=lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e));
    const char *key=lv_buttonmatrix_get_button_text(keypad,index);if(!key||!focused)return;
    lv_obj_t *text=lv_textarea_get_label(focused);
    bool selected=lv_label_get_text_selection_start(text)!=LV_DRAW_LABEL_NO_TXT_SEL&&
        lv_label_get_text_selection_end(text)>lv_label_get_text_selection_start(text);
    if(!strcmp(key,"下一项"))select_field(focused==hour?minute:hour);
    else if(!strcmp(key,"退格")){
        if(selected){lv_textarea_set_text(focused,"");update_time(focused);}
        else lv_textarea_delete_char(focused);
    }
    else if(selected){lv_textarea_set_text(focused,"");lv_textarea_add_text(focused,key);}
    else if(strlen(lv_textarea_get_text(focused))<2u)lv_textarea_add_text(focused,key);
    if(ports.activity)ports.activity(ports.ctx);
}
static void row_clicked(lv_event_t *e){
    (void)am_begin(model,(uint32_t)(uintptr_t)lv_event_get_user_data(e));
}
static void switched(lv_event_t *e){
    if(syncing)return;
    lv_obj_t *o=lv_event_get_target_obj(e);uint32_t id=(uint32_t)(uintptr_t)lv_event_get_user_data(e);
    (void)am_toggle(model,id,lv_obj_has_state(o,LV_STATE_CHECKED));
    int i=am_find(&model->confirmed,id);syncing=true;
    if(i>=0&&model->confirmed.entries[i].enabled)lv_obj_add_state(o,LV_STATE_CHECKED);else lv_obj_remove_state(o,LV_STATE_CHECKED);
    syncing=false;
}
static void draft_switched(lv_event_t *e){
    if(!syncing)(void)am_edit_enabled(model,lv_obj_has_state(lv_event_get_target_obj(e),LV_STATE_CHECKED));
}
static void switch_state(lv_obj_t *o,bool enabled){
    if(enabled)lv_obj_add_state(o,LV_STATE_CHECKED);else lv_obj_remove_state(o,LV_STATE_CHECKED);
    lv_obj_set_hidden(lv_obj_get_child(o,0),!enabled);
}
static lv_obj_t *common_switch(lv_obj_t *p,int x,int y){
    lv_obj_t *o=lv_switch_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,96,48);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_radius(o,24,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x112330),0);lv_obj_set_style_bg_color(o,lv_color_hex(0xb4cbd0),LV_STATE_CHECKED);
    lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(0x607787),0);
    lv_obj_set_style_bg_opa(o,LV_OPA_TRANSP,LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,LV_PART_KNOB);lv_obj_set_style_radius(o,16,LV_PART_KNOB);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x607787),LV_PART_KNOB);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x071520),LV_PART_KNOB|LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(o,-8,LV_PART_KNOB);lv_obj_set_style_anim_duration(o,0,0);
    lv_obj_set_style_opa(o,LV_OPA_40,LV_STATE_DISABLED);lv_obj_set_event_bubble(o,true);
    static const lv_point_precise_t check[]={{66,24},{71,29},{79,18}};
    lv_obj_t *mark=lv_line_create(o);lv_line_set_points(mark,check,3);lv_obj_set_clickable(mark,false);
    lv_obj_set_style_line_width(mark,2,0);lv_obj_set_style_line_color(mark,lv_color_hex(0xb4cbd0),0);
    lv_obj_set_hidden(mark,true);return o;
}
static void action(lv_event_t *e){
    int a=(int)(intptr_t)lv_event_get_user_data(e);
    if(a==1){if(!model->busy&&ports.home)ports.home(ports.ctx);}
    else if(a==2)(void)am_begin(model,0);
    else if(a==3)(void)am_back(model);
    else if(a==4){field_touched[0]=field_touched[1]=true;update_time(focused?focused:hour);(void)am_save(model);}
    else if(a==6)(void)am_edit_enabled(model,!model->draft.enabled);
    else if(a==7)(void)am_ask_delete(model);
    else if(a==8)(void)am_delete(model);
    else if(a==9)(void)am_reload(model);
    else if(a>=10&&a<=12)(void)am_edit_mode(model,(unsigned)(a-10));
    else if(a==13){if(!model->busy&&ports.settings)ports.settings(ports.ctx);}
    else if(a==14&&model->note==AM_NOTE_FAILED&&model->view==AM_LIST){
        for(unsigned i=0;i<model->job.proposed.count;i++){
            const am_entry *proposed=&model->job.proposed.entries[i];int at=am_find(&model->confirmed,proposed->id);
            if(at>=0&&model->confirmed.entries[at].enabled!=proposed->enabled){(void)am_toggle(model,proposed->id,proposed->enabled);break;}
        }
    }
}
static void draw_rows(void){
    lv_obj_clean(rows);memset(row_edit,0,sizeof row_edit);memset(row_switch,0,sizeof row_switch);memset(row_state,0,sizeof row_state);
    if(!model->confirmed.count){label(rows,model->known?"添加闹铃后可在这里管理":"读取后显示已保存的闹铃",0,24,850,body_font);return;}
    for(unsigned i=0;i<model->confirmed.count;i++){
        const am_entry *e=&model->confirmed.entries[i];int y=(int)i*120;
        lv_obj_t *line=panel(rows,0,y,928,120);lv_obj_set_clickable(line,false);
        lv_obj_t *edit=panel(line,0,0,670,118);lv_obj_set_clickable(edit,true);
        char text[12];snprintf(text,sizeof text,"%02u:%02u",e->hour,e->minute);
        label(edit,text,8,23,220,digit_font);label(edit,am_mode_text(e->mode),250,37,400,body_font);
        lv_obj_add_event_cb(edit,row_clicked,LV_EVENT_CLICKED,(void*)(uintptr_t)e->id);row_edit[i]=edit;
        row_state[i]=label(line,e->enabled?"开启":"关闭",680,37,128,body_font);
        lv_obj_set_style_text_align(row_state[i],LV_TEXT_ALIGN_RIGHT,0);
        lv_obj_t *sw=common_switch(line,816,36);switch_state(sw,e->enabled);
        lv_obj_add_event_cb(sw,switched,LV_EVENT_VALUE_CHANGED,(void*)(uintptr_t)e->id);row_switch[i]=sw;
        if(i+1<model->confirmed.count){lv_obj_t *rule=panel(line,0,119,928,1);lv_obj_set_style_bg_color(rule,lv_color_hex(0x344653),0);lv_obj_set_style_bg_opa(rule,LV_OPA_COVER,0);}
    }
}
static void recolor(lv_obj_t *o,uint32_t color){
    lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    if(lv_obj_check_type(o,&lv_buttonmatrix_class))lv_obj_set_style_text_color(o,lv_color_hex(color),LV_PART_ITEMS);
    for(uint32_t i=0;i<lv_obj_get_child_count(o);i++)recolor(lv_obj_get_child(o,(int32_t)i),color);
}
void am_ui_set_list_palette(const am_ui_list_palette *palette){
    if(palette&&memcmp(&list_palette,palette,sizeof list_palette)){
        list_palette=*palette;list_palette_dirty=true;
    }
}
static void theme_switch(lv_obj_t *sw,const am_ui_list_palette *p){
    lv_obj_set_style_bg_opa(sw,LV_OPA_TRANSP,0);
    lv_obj_set_style_bg_opa(sw,LV_OPA_COVER,LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw,lv_color_hex(p->on),LV_STATE_CHECKED);
    lv_obj_set_style_border_color(sw,lv_color_hex(p->edge),0);
    lv_obj_set_style_border_color(sw,lv_color_hex(p->on),LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw,lv_color_hex(p->off),LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw,lv_color_hex(p->deep),LV_PART_KNOB|LV_STATE_CHECKED);
    lv_obj_set_style_line_color(lv_obj_get_child(sw,0),lv_color_hex(p->on),0);
}
static void theme_list(void){
    const am_ui_list_palette *p=&list_palette;
    recolor(views[AM_LIST],p->ink);
    lv_obj_set_style_text_color(clock_labels[AM_LIST],lv_color_hex(p->sub),0);
    lv_obj_set_style_text_color(notes[AM_LIST],lv_color_hex(p->sub),0);
    lv_obj_t *buttons[]={add,retry,reload[AM_LIST]};
    for(unsigned i=0;i<3;i++){
        lv_obj_set_style_bg_color(buttons[i],lv_color_hex(p->button),0);
        lv_obj_set_style_border_color(buttons[i],lv_color_hex(p->edge),0);
    }
    if(list_settings){
        lv_obj_set_style_bg_color(list_settings,lv_color_hex(p->sub),LV_STATE_PRESSED);
        if(lv_obj_get_child_count(list_settings)){
            lv_obj_t *gear=lv_obj_get_child(list_settings,0);
            lv_obj_set_style_image_recolor(gear,lv_color_hex(p->sub),0);
            lv_obj_set_style_image_recolor_opa(gear,LV_OPA_COVER,0);
        }
    }
    lv_obj_set_style_bg_color(rows,lv_color_hex(p->sub),LV_PART_SCROLLBAR);
    for(unsigned i=0;i<model->confirmed.count;i++){
        lv_obj_t *sw=row_switch[i];
        lv_obj_set_style_text_color(lv_obj_get_child(row_edit[i],1),lv_color_hex(p->sub),0);
        lv_obj_set_style_text_color(row_state[i],lv_color_hex(p->sub),0);
        theme_switch(sw,p);
        lv_obj_t *row=lv_obj_get_parent(row_edit[i]);
        if(lv_obj_get_child_count(row)>3)lv_obj_set_style_bg_color(lv_obj_get_child(row,3),lv_color_hex(p->line),0);
    }
}
static void theme_views(void){
    const am_ui_list_palette *p=&list_palette;
    /* The same scene reaches every alarm surface, including an open draft. */
    lv_obj_set_style_bg_color(root,lv_color_hex(p->bg),0);
    recolor(root,p->ink);
    for(unsigned i=0;i<4u;i++){
        lv_obj_set_style_text_color(clock_labels[i],lv_color_hex(p->sub),0);
        lv_obj_set_style_text_color(notes[i],lv_color_hex(p->sub),0);
        lv_obj_set_style_bg_color(reload[i],lv_color_hex(p->button),0);
    }
    theme_list();
    lv_obj_t *buttons[]={cancel_edit,remove_button,delete_back,delete_no,delete_yes};
    for(unsigned i=0;i<sizeof buttons/sizeof buttons[0];i++)
        lv_obj_set_style_bg_color(buttons[i],lv_color_hex(p->button),0);
    lv_obj_set_style_bg_color(save,lv_color_hex(p->on),0);
    lv_obj_set_style_text_color(lv_obj_get_child(save,0),lv_color_hex(p->deep),0);
    lv_obj_t *fields[]={hour,minute};
    for(unsigned i=0;i<2u;i++){
        lv_obj_set_style_bg_color(fields[i],lv_color_hex(p->button),0);
        lv_obj_set_style_outline_color(fields[i],lv_color_hex(p->on),LV_STATE_FOCUSED);
        lv_obj_t *text=lv_textarea_get_label(fields[i]);
        lv_obj_set_style_bg_color(text,lv_color_hex(p->on),LV_PART_SELECTED);
        lv_obj_set_style_text_color(text,lv_color_hex(p->deep),LV_PART_SELECTED);
    }
    lv_obj_set_style_border_color(repeat_button,lv_color_hex(p->edge),0);
    for(unsigned i=0;i<3u;i++){
        lv_obj_set_style_border_color(repeat_options[i],lv_color_hex(p->line),0);
        lv_obj_set_style_bg_color(repeat_options[i],lv_color_hex(p->on),LV_STATE_CHECKED);
        lv_obj_set_style_text_color(lv_obj_get_child(repeat_options[i],0),lv_color_hex(model->draft.mode==i?p->deep:p->ink),0);
        lv_obj_set_style_bg_color(lv_obj_get_child(repeat_options[i],1),lv_color_hex(p->deep),0);
    }
    lv_obj_set_style_text_color(repeat_help,lv_color_hex(p->sub),0);
    lv_obj_set_style_bg_color(keypad,lv_color_hex(p->bg),0);
    lv_obj_set_style_bg_color(keypad,lv_color_hex(p->button),LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad,lv_color_hex(p->on),LV_PART_ITEMS|LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keypad,lv_color_hex(p->deep),LV_PART_ITEMS|LV_STATE_PRESSED);
    theme_switch(enabled_button,p);
    field_errors();
    list_palette_dirty=false;
}
bool am_ui_create(lv_obj_t *p,am_controller *c,const am_ui_ports *io,const lv_font_t *body,const lv_font_t *digits){
    if(root||!p||!c||!io||!body||!digits)return false;
    model=c;ports=*io;body_font=body;digit_font=digits;
    root=panel(p,0,0,1024,600);lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
    lv_obj_add_event_cb(root,activity,LV_EVENT_PRESSED,NULL);lv_obj_add_event_cb(root,activity,LV_EVENT_SCROLL_BEGIN,NULL);
    for(unsigned i=0;i<4u;i++){
        views[i]=panel(root,0,0,1024,600);
        lv_obj_set_hidden(views[i],i!=AM_LIST);
        clock_labels[i]=label(views[i],"--:--",820,112,160,body_font);
        notes[i]=label(views[i],"",48,540,690,body_font);lv_obj_set_height(notes[i],60);
        reload[i]=button(views[i],"重新读取",560,30,216,9);
    }
    label(views[AM_LIST],"闹铃",48,47,250,body_font);
    lv_obj_set_pos(notes[AM_LIST],48,504);lv_obj_set_size(notes[AM_LIST],690,42);
    lv_label_set_long_mode(notes[AM_LIST],LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(clock_labels[AM_LIST],680,47);lv_obj_set_width(clock_labels[AM_LIST],108);
    lv_obj_set_pos(reload[AM_LIST],440,36);
    add=button(views[AM_LIST],"新增",804,36,100,2);
    lv_obj_set_style_border_width(add,1,0);lv_obj_set_style_border_color(add,lv_color_hex(0x607787),0);
    if(ports.settings){
        list_settings=panel(views[AM_LIST],920,32,64,72);lv_obj_set_clickable(list_settings,true);
        lv_obj_set_style_radius(list_settings,12,0);lv_obj_set_style_bg_color(list_settings,lv_color_hex(0xc4d1d8),LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(list_settings,20,LV_STATE_PRESSED);
        lv_obj_add_event_cb(list_settings,action,LV_EVENT_CLICKED,(void*)(intptr_t)13);
        if(ports.settings_icon){
            lv_obj_t *gear=lv_image_create(list_settings);lv_image_set_src(gear,ports.settings_icon);lv_obj_set_pos(gear,16,20);
            lv_image_set_scale(gear,224);lv_obj_set_clickable(gear,false);
        }
    }
    rows=panel(views[AM_LIST],48,140,928,360);lv_obj_set_scrollable(rows,true);lv_obj_set_scroll_dir(rows,LV_DIR_VER);lv_obj_set_scrollbar_mode(rows,LV_SCROLLBAR_MODE_AUTO);
    retry=button(views[AM_LIST],"重试",804,528,160,14);lv_obj_set_hidden(retry,true);
    cancel_edit=button(views[AM_EDIT],"取消",48,36,112,3);edit_title=label(views[AM_EDIT],"编辑闹铃",176,47,300,body_font);
    lv_obj_set_style_bg_opa(cancel_edit,LV_OPA_TRANSP,0);
    save=button(views[AM_EDIT],"保存",882,36,102,4);
    lv_obj_set_style_bg_color(save,lv_color_hex(0xb4cbd0),0);
    lv_obj_set_pos(clock_labels[AM_EDIT],0,-100); /* Editing has its own stable field focus, not a second clock. */
    lv_obj_set_pos(notes[AM_EDIT],544,470);lv_obj_set_size(notes[AM_EDIT],416,60);
    label(views[AM_EDIT],"小时",80,134,160,body_font);label(views[AM_EDIT],"分钟",268,134,160,body_font);
    hour=lv_textarea_create(views[AM_EDIT]);minute=lv_textarea_create(views[AM_EDIT]);
    lv_obj_t *fields[]={hour,minute};
    for(unsigned i=0;i<2u;i++){
        lv_obj_set_pos(fields[i],80+(int)i*188,176);lv_obj_set_size(fields[i],144,84);lv_textarea_set_one_line(fields[i],true);
        lv_textarea_set_accepted_chars(fields[i],"0123456789");lv_textarea_set_max_length(fields[i],2);
        lv_obj_set_style_text_font(fields[i],digit_font,0);lv_obj_set_style_text_align(fields[i],LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_style_text_color(fields[i],lv_color_hex(0xf0f3f3),0);lv_obj_set_style_bg_color(fields[i],lv_color_hex(0x112330),0);
        lv_obj_set_style_border_color(fields[i],lv_color_hex(0x607787),0);lv_obj_set_event_bubble(fields[i],true);
        lv_obj_set_style_border_width(fields[i],1,0);lv_obj_set_style_radius(fields[i],8,0);lv_obj_set_style_pad_all(fields[i],8,0);
        lv_obj_set_style_outline_width(fields[i],2,LV_STATE_FOCUSED);lv_obj_set_style_outline_pad(fields[i],3,LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(fields[i],lv_color_hex(0xb4cbd0),LV_STATE_FOCUSED);
        lv_textarea_set_cursor_click_pos(fields[i],false);lv_textarea_set_text_selection(fields[i],true);
        lv_obj_t *text=lv_textarea_get_label(fields[i]);
        lv_obj_set_style_bg_color(text,lv_color_hex(0xb4cbd0),LV_PART_SELECTED);
        lv_obj_set_style_text_color(text,lv_color_hex(0x071520),LV_PART_SELECTED);
        lv_obj_add_event_cb(fields[i],time_changed,LV_EVENT_VALUE_CHANGED,NULL);lv_obj_add_event_cb(fields[i],select_time,LV_EVENT_CLICKED,NULL);
    }
    /* Center the shorter punctuation ink beside the unchanged number fields. */
    label(views[AM_EDIT],":",238,180,30,digit_font);
    hour_error=label(views[AM_EDIT],"",80,264,170,body_font);minute_error=label(views[AM_EDIT],"",268,264,170,body_font);
    label(views[AM_EDIT],"重复",544,160,416,body_font);
    repeat_button=panel(views[AM_EDIT],544,210,416,68);
    lv_obj_set_style_radius(repeat_button,8,0);lv_obj_set_style_border_width(repeat_button,1,0);
    lv_obj_set_style_border_color(repeat_button,lv_color_hex(0x607787),0);
    for(unsigned i=0;i<3;i++){
        int x=(int)i*416/3,w=(int)(i+1)*416/3-x;
        repeat_options[i]=button(repeat_button,am_mode_text(i),x,0,w,(int)i+10);lv_obj_set_height(repeat_options[i],68);
        lv_obj_set_style_radius(repeat_options[i],0,0);lv_obj_set_style_bg_opa(repeat_options[i],LV_OPA_TRANSP,0);
        lv_obj_set_style_bg_opa(repeat_options[i],LV_OPA_COVER,LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(repeat_options[i],lv_color_hex(0xb4cbd0),LV_STATE_CHECKED);
        if(i){lv_obj_set_style_border_width(repeat_options[i],1,0);lv_obj_set_style_border_side(repeat_options[i],LV_BORDER_SIDE_LEFT,0);lv_obj_set_style_border_color(repeat_options[i],lv_color_hex(0x344653),0);}
        lv_obj_t *underline=panel(repeat_options[i],24,57,w-48,2);lv_obj_set_style_bg_color(underline,lv_color_hex(0x071520),0);lv_obj_set_style_bg_opa(underline,LV_OPA_COVER,0);
    }
    repeat_help=label(views[AM_EDIT],"",544,290,416,body_font);
    label(views[AM_EDIT],"启用",544,372,160,body_font);
    enabled_text=label(views[AM_EDIT],"开启",784,372,64,body_font);
    enabled_button=common_switch(views[AM_EDIT],864,370);lv_obj_add_event_cb(enabled_button,draft_switched,LV_EVENT_VALUE_CHANGED,NULL);
    label(views[AM_EDIT],"保存后生效",544,432,416,body_font);
    remove_button=button(views[AM_EDIT],"删除闹铃",536,528,200,7);lv_obj_set_style_bg_opa(remove_button,LV_OPA_TRANSP,0);
    keypad=lv_buttonmatrix_create(views[AM_EDIT]);lv_obj_set_pos(keypad,64,302);lv_obj_set_size(keypad,400,262);
    lv_buttonmatrix_set_map(keypad,keys);lv_obj_set_style_text_font(keypad,body_font,LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad,lv_color_hex(0x071520),0);lv_obj_set_style_bg_color(keypad,lv_color_hex(0x112a3a),LV_PART_ITEMS);
    lv_obj_set_style_text_color(keypad,lv_color_hex(0xc4d1d8),LV_PART_ITEMS);lv_obj_set_style_border_width(keypad,0,0);lv_obj_set_event_bubble(keypad,true);
    lv_obj_set_style_pad_all(keypad,0,0);lv_obj_set_style_pad_row(keypad,10,0);lv_obj_set_style_pad_column(keypad,10,0);
    lv_obj_set_style_radius(keypad,8,LV_PART_ITEMS);lv_obj_set_style_border_width(keypad,0,LV_PART_ITEMS);
    lv_obj_add_event_cb(keypad,keypad_changed,LV_EVENT_VALUE_CHANGED,NULL);
    delete_back=button(views[AM_DELETE],"返回编辑",48,30,216,3);label(views[AM_DELETE],"删除闹铃",360,40,400,body_font);
    delete_text=label(views[AM_DELETE],"",120,210,784,body_font);
    delete_no=button(views[AM_DELETE],"取消",180,340,280,3);delete_yes=button(views[AM_DELETE],"确认删除",564,340,280,8);
    return true;
}
void am_ui_visible(bool visible){if(root)lv_obj_set_hidden(root,!visible);}
void am_ui_refresh(const af_time *r,bool trusted,bool inclusive,const af_year *years,size_t n,bool night){
    if(!root)return;
    (void)night; /* The shared scene palette includes night and low-light colors. */
    am_reply reply;if(ports.take&&ports.take(ports.ctx,&reply))(void)am_accept(model,&reply);
    if(model->busy&&dispatched!=model->job.token){
        dispatched=model->job.token;
        dispatched_at=lv_tick_get();
        if(!ports.submit||!ports.submit(ports.ctx,&model->job))am_dispatch_failed(model);
    }
    if(model->busy&&lv_tick_elaps(dispatched_at)>=AM_REPLY_TIMEOUT_MS)am_request_timeout(model);
    bool locked=model->busy||model->blocked||!model->known;
    bool redraw=rendered!=model->revision;
    if(redraw){
        rendered=model->revision;syncing=true;
        for(unsigned i=0;i<4u;i++)lv_obj_set_hidden(views[i],i!=(unsigned)model->view);
        if(!am_config_equal(&rendered_config,&model->confirmed)||rows_known!=model->known){
            draw_rows();rendered_config=model->confirmed;rows_known=model->known;
        }
        for(unsigned i=0;i<model->confirmed.count;i++){
            const am_entry *entry=&model->confirmed.entries[i];set_disabled(row_edit[i],locked);set_disabled(row_switch[i],locked);
            switch_state(row_switch[i],entry->enabled);
            int next=am_find(&model->job.proposed,entry->id);
            bool changed_switch=model->job.kind==AM_JOB_SAVE&&next>=0&&model->job.proposed.entries[next].enabled!=entry->enabled;
            text_if_changed(row_state[i],changed_switch&&model->busy?"保存中":changed_switch&&model->note==AM_NOTE_UNCERTAIN?"待确认":entry->enabled?"开启":"关闭");
        }
        set_disabled(add,locked||model->confirmed.count>=AM_CAPACITY);button_text(add,model->confirmed.count>=AM_CAPACITY?"已满":"新增");
        set_disabled(save,locked||model->draft.hour>23||model->draft.minute>59);set_disabled(hour,locked);set_disabled(minute,locked);set_disabled(keypad,locked);
        if(list_settings)set_disabled(list_settings,model->busy);
        set_disabled(enabled_button,locked);set_disabled(remove_button,locked);set_disabled(cancel_edit,model->busy||model->blocked);
        set_disabled(delete_yes,locked);set_disabled(delete_no,model->busy||model->blocked);
        lv_obj_set_hidden(remove_button,model->draft_new);
        switch_state(enabled_button,model->draft.enabled);text_if_changed(enabled_text,model->draft.enabled?"开启":"关闭");
        text_if_changed(edit_title,model->draft_new?"新增闹铃":"编辑闹铃");
        text_if_changed(repeat_help,model->draft.mode==AF_WORKDAY?"按国家调休安排":"");
        for(unsigned i=0;i<3u;i++){
            bool selected=model->draft.mode==i;
            if(selected)lv_obj_add_state(repeat_options[i],LV_STATE_CHECKED);else lv_obj_remove_state(repeat_options[i],LV_STATE_CHECKED);
            lv_obj_set_hidden(lv_obj_get_child(repeat_options[i],1),!selected);set_disabled(repeat_options[i],locked);
        }
        lv_obj_set_hidden(retry,model->note!=AM_NOTE_FAILED||model->view!=AM_LIST);set_disabled(retry,locked);
        if(seen_draft!=model->draft_epoch&&model->view==AM_EDIT){
            char text[4];snprintf(text,sizeof text,"%02u",model->draft.hour);lv_textarea_set_text(hour,text);
            snprintf(text,sizeof text,"%02u",model->draft.minute);lv_textarea_set_text(minute,text);
            field_touched[0]=field_touched[1]=false;field_errors();select_field(hour);seen_draft=model->draft_epoch;
        }
        const am_entry *deleting=&model->draft;int saved_index=am_find(&model->confirmed,model->draft.id);
        if(saved_index>=0)deleting=&model->confirmed.entries[saved_index];
        char text[128];snprintf(text,sizeof text,"删除 %02u:%02u · %s?",deleting->hour,deleting->minute,am_mode_text(deleting->mode));lv_label_set_text(delete_text,text);
        syncing=false;
    }
    char clock[12]="--:--";if(trusted&&af_time_valid(r))snprintf(clock,sizeof clock,"%02u:%02u",r->hour,r->minute);
    char message[256];snprintf(message,sizeof message,"%s",am_note_text(model->note));
    if(model->view==AM_EDIT&&model->note==AM_NOTE_INVALID)message[0]=0; /* Field feedback is specific and adjacent. */
    if(model->note==AM_NOTE_NONE&&model->view==AM_LIST){
        am_summary s=am_next_summary(model,r,trusted,inclusive,years,n);
        bool calendar_notice=am_calendar_notice(model,r,trusted,years,n),audio_ready=true;
        if(ports.summary)(void)ports.summary(ports.ctx,&s,&calendar_notice,&audio_ready);
        /* Home already carries the next-alarm summary. This one quiet line is
         * reserved for actionable exceptions, above the shared pager. */
        if(s.status==AM_SUMMARY_WAIT)snprintf(message,sizeof message,"待校时，暂不响铃");
        else if(s.status==AM_SUMMARY_UNKNOWN)snprintf(message,sizeof message,"下次闹铃待确认");
        if(!audio_ready&&s.status!=AM_SUMMARY_HIDDEN)snprintf(message,sizeof message,"声音未就绪");
        if(calendar_notice){
            if(message[0])strncat(message," · ",sizeof message-strlen(message)-1u);
            strncat(message,"调休未更新，暂按周一至周五",sizeof message-strlen(message)-1u);
        }
    }
    for(unsigned i=0;i<4u;i++){
        text_if_changed(clock_labels[i],clock);text_if_changed(notes[i],message);
        lv_obj_set_hidden(reload[i],!model->blocked);set_disabled(reload[i],model->busy);
    }
    lv_obj_set_style_opa(root,LV_OPA_COVER,0);
    if(redraw||list_palette_dirty)theme_views();
}
bool am_ui_editing(void){return model&&model->view!=AM_LIST;}
bool am_ui_saving(void){return model&&model->busy;}
lv_obj_t *am_ui_root(void){return root;}
lv_obj_t *am_ui_control(const char *name,uint32_t id){
    if(!strcmp(name,"list"))return rows;
    if(!strcmp(name,"add"))return add;
    if(!strcmp(name,"settings"))return list_settings;
    if(!strcmp(name,"hour"))return hour;
    if(!strcmp(name,"minute"))return minute;
    if(!strcmp(name,"keypad"))return keypad;
    if(!strcmp(name,"save"))return save;
    if(!strcmp(name,"cancel"))return cancel_edit;
    if(!strcmp(name,"repeat"))return repeat_button;
    if(!strcmp(name,"repeat_back"))return NULL; /* Removed repeat subpage; mode controls are inline. */
    if(!strcmp(name,"hour_error"))return hour_error;
    if(!strcmp(name,"minute_error"))return minute_error;
    if(!strcmp(name,"retry"))return retry;
    if(!strcmp(name,"enabled"))return enabled_button;
    if(!strcmp(name,"delete"))return remove_button;
    if(!strcmp(name,"delete_yes"))return delete_yes;
    if(!strcmp(name,"delete_no"))return delete_no;
    if(!strcmp(name,"reload"))return reload[model->view];
    if(!strcmp(name,"mode"))return id<3u?repeat_options[id]:NULL;
    int i=am_find(&model->confirmed,id);if(i<0)return NULL;
    if(!strcmp(name,"row"))return row_edit[i];
    if(!strcmp(name,"switch"))return row_switch[i];
    return NULL;
}
