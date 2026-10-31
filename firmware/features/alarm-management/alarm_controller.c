#include "alarm_controller.h"
#include <string.h>
static void changed(am_controller *c){c->revision++;}
static bool writable(const am_controller *c){return c&&c->known&&!c->blocked&&!c->busy;}
static bool request(am_controller *c,am_job_kind kind,const am_config *next){
    if(c->busy)return false;
    c->busy=true;c->token++;if(!c->token)c->token++;
    c->job=(am_job){.token=c->token,.kind=kind,.expected=c->confirmed};
    if(next)c->job.proposed=*next;
    c->note=kind==AM_JOB_LOAD?AM_NOTE_LOADING:AM_NOTE_SAVING;changed(c);return true;
}
void am_init(am_controller *c){memset(c,0,sizeof *c);am_config_empty(&c->confirmed);c->blocked=true;(void)request(c,AM_JOB_LOAD,NULL);}
bool am_reload(am_controller *c){
    if(!c||c->busy)return false;
    bool recovering=c->recovering||(c->blocked&&c->note==AM_NOTE_UNCERTAIN&&c->job.kind==AM_JOB_SAVE);
    am_config unresolved=c->job.proposed;
    bool accepted=request(c,AM_JOB_LOAD,NULL);
    if(accepted){c->recovering=recovering;if(recovering)c->job.proposed=unresolved;}
    return accepted;
}
bool am_begin(am_controller *c,uint32_t id){
    if(!writable(c)||c->view!=AM_LIST)return false;
    if(!id){
        if(c->confirmed.count==AM_CAPACITY||c->confirmed.next_id==UINT32_MAX){c->note=AM_NOTE_FULL;changed(c);return false;}
        c->draft=(am_entry){c->confirmed.next_id,7,0,AF_ONCE,1};c->draft_new=true;
    }else{int i=am_find(&c->confirmed,id);if(i<0)return false;c->draft=c->confirmed.entries[i];c->draft_new=false;}
    c->view=AM_EDIT;c->note=AM_NOTE_NONE;c->draft_epoch++;changed(c);return true;
}
bool am_edit_time(am_controller *c,int h,int m){
    if(!writable(c)||c->view!=AM_EDIT)return false;
    c->draft.hour=(h>=0&&h<24)?(uint8_t)h:UINT8_MAX;c->draft.minute=(m>=0&&m<60)?(uint8_t)m:UINT8_MAX;
    if(c->note==AM_NOTE_FAILED||(c->note==AM_NOTE_INVALID&&h>=0&&h<24&&m>=0&&m<60))c->note=AM_NOTE_NONE;
    changed(c);return true;
}
bool am_edit_mode(am_controller *c,unsigned mode){
    if(!writable(c)||(c->view!=AM_EDIT&&c->view!=AM_REPEAT)||mode>AF_DAILY)return false;
    c->draft.mode=(uint8_t)mode;if(c->note==AM_NOTE_FAILED)c->note=AM_NOTE_NONE;changed(c);return true;
}
bool am_edit_enabled(am_controller *c,bool enabled){
    if(!writable(c)||c->view!=AM_EDIT)return false;
    c->draft.enabled=(uint8_t)enabled;if(c->note==AM_NOTE_FAILED)c->note=AM_NOTE_NONE;changed(c);return true;
}
bool am_show_repeat(am_controller *c){if(!writable(c)||c->view!=AM_EDIT)return false;c->view=AM_REPEAT;changed(c);return true;}
bool am_back(am_controller *c){
    if(!c||c->busy)return false;
    if(c->view==AM_REPEAT||c->view==AM_DELETE)c->view=AM_EDIT;
    else if(c->view==AM_EDIT){c->view=AM_LIST;memset(&c->draft,0,sizeof c->draft);}
    else return false;
    if(!c->blocked)c->note=AM_NOTE_NONE;
    changed(c);return true;
}
bool am_ask_delete(am_controller *c){
    if(!writable(c)||c->view!=AM_EDIT||c->draft_new)return false;
    c->view=AM_DELETE;changed(c);return true;
}
bool am_save(am_controller *c){
    if(!writable(c)||c->view!=AM_EDIT)return false;
    am_config next=c->confirmed;
    if(c->draft_new){
        if(next.count==AM_CAPACITY||next.next_id==UINT32_MAX||c->draft.id!=next.next_id){c->note=AM_NOTE_CHANGED;changed(c);return false;}
        next.entries[next.count++]=c->draft;next.next_id++;
    }else{int i=am_find(&next,c->draft.id);if(i<0){c->note=AM_NOTE_CHANGED;changed(c);return false;}next.entries[i]=c->draft;}
    if(!am_config_valid(&next)){c->note=AM_NOTE_INVALID;changed(c);return false;}
    return request(c,AM_JOB_SAVE,&next);
}
bool am_delete(am_controller *c){
    if(!writable(c)||c->view!=AM_DELETE||c->draft_new)return false;
    am_config next=c->confirmed;int i=am_find(&next,c->draft.id);if(i<0)return false;
    for(unsigned j=(unsigned)i;j+1u<next.count;j++)next.entries[j]=next.entries[j+1u];
    next.count--;memset(&next.entries[next.count],0,sizeof next.entries[0]);
    return request(c,AM_JOB_SAVE,&next);
}
bool am_toggle(am_controller *c,uint32_t id,bool enabled){
    if(!writable(c)||c->view!=AM_LIST)return false;
    am_config next=c->confirmed;int i=am_find(&next,id);if(i<0)return false;
    next.entries[i].enabled=(uint8_t)enabled;return request(c,AM_JOB_SAVE,&next);
}
void am_dispatch_failed(am_controller *c){
    if(!c||!c->busy)return;
    c->busy=false;c->note=c->job.kind==AM_JOB_LOAD?AM_NOTE_READ_ERROR:AM_NOTE_FAILED;changed(c);
}
void am_request_timeout(am_controller *c){
    if(!c||!c->busy)return;
    c->busy=false;c->blocked=true;c->note=c->job.kind==AM_JOB_SAVE?AM_NOTE_UNCERTAIN:AM_NOTE_READ_ERROR;changed(c);
}
bool am_accept(am_controller *c,const am_reply *r){
    if(!c||!r||!c->busy||r->token!=c->job.token)return false;
    c->busy=false;am_io_status s=r->result.status;bool had_config=c->known;
    if(c->job.kind==AM_JOB_LOAD&&c->recovering&&s==AM_IO_OK&&am_config_equal(&r->result.config,&c->job.proposed))s=AM_IO_SAVED;
    if(s==AM_IO_SAVED&&((c->job.kind!=AM_JOB_SAVE&&!c->recovering)||!am_config_equal(&r->result.config,&c->job.proposed)))s=AM_IO_UNCERTAIN;
    bool readable=s==AM_IO_OK||s==AM_IO_MISSING||s==AM_IO_SAVED||s==AM_IO_DIFFERENT;
    if(readable&&am_config_valid(&r->result.config)){
        c->confirmed=r->result.config;c->known=true;c->blocked=false;
        c->note=(s==AM_IO_DIFFERENT||(c->job.kind==AM_JOB_LOAD&&had_config))?AM_NOTE_CHANGED:AM_NOTE_NONE;
        if(s==AM_IO_SAVED){c->view=AM_LIST;c->recovering=false;c->note=AM_NOTE_NONE;memset(&c->draft,0,sizeof c->draft);}
        else if(c->job.kind==AM_JOB_LOAD)c->recovering=false;
    }else{
        c->blocked=true;
        c->note=s==AM_IO_BAD?AM_NOTE_BAD:s==AM_IO_FOREIGN?AM_NOTE_FOREIGN:s==AM_IO_UNCERTAIN?AM_NOTE_UNCERTAIN:AM_NOTE_READ_ERROR;
    }
    changed(c);return true;
}
static int compare(const af_time *a,const af_time *b){
    const unsigned x[]={a->year,a->month,a->day,a->hour,a->minute,a->second};
    const unsigned y[]={b->year,b->month,b->day,b->hour,b->minute,b->second};
    for(unsigned i=0;i<6u;i++){if(x[i]<y[i])return -1;if(x[i]>y[i])return 1;}return 0;
}
am_summary am_next_summary(const am_controller *c,const af_time *r,bool trusted,bool inclusive,const af_year *years,size_t n){
    am_summary s={.status=AM_SUMMARY_HIDDEN};
    if(!c||!c->known||c->blocked){s.status=AM_SUMMARY_UNKNOWN;return s;}
    for(unsigned i=0;i<c->confirmed.count;i++){
        const am_entry *e=&c->confirmed.entries[i];if(!e->enabled)continue;
        if(!trusted){s.status=AM_SUMMARY_WAIT;return s;}
        af_alarm alarm={true,true,e->hour,e->minute,(af_mode)e->mode};
        af_candidate x=af_next(&alarm,r,trusted,inclusive,years,n);
        if(x.status!=AF_OK){s.status=AM_SUMMARY_UNKNOWN;return s;}
        if(s.status!=AM_SUMMARY_NEXT||compare(&x.at,&s.at)<0){s.at=x.at;s.id=e->id;}
        s.status=AM_SUMMARY_NEXT;s.fallback|=x.calendar_fallback;
    }
    return s;
}
bool am_calendar_notice(const am_controller *c,const af_time *r,bool trusted,const af_year *years,size_t n){
    if(!c||!c->known||c->blocked||c->view!=AM_LIST||!trusted||!af_time_valid(r))return false;
    bool enabled=false;for(unsigned i=0;i<c->confirmed.count;i++)enabled|=c->confirmed.entries[i].enabled&&c->confirmed.entries[i].mode==AF_WORKDAY;
    if(!enabled)return false;
    bool work=false,missing=false;
    if(!af_is_workday(r,years,n,&work,&missing))return false;
    am_summary summary=am_next_summary(c,r,true,true,years,n);
    return missing||(summary.status==AM_SUMMARY_NEXT&&summary.fallback);
}
const char *am_mode_text(unsigned m){return m==AF_ONCE?"仅响一次":m==AF_WORKDAY?"工作日":m==AF_DAILY?"每天":"重复无效";}
const char *am_note_text(am_note n){
    switch(n){
    case AM_NOTE_LOADING:return "正在读取闹铃…";
    case AM_NOTE_NONE:return "";
    case AM_NOTE_SAVING:return "正在保存…";
    case AM_NOTE_FULL:return "闹铃已满，请先删除一项";
    case AM_NOTE_INVALID:return "请输入有效时分：00:00 至 23:59";
    case AM_NOTE_FAILED:return "暂时无法保存，请稍后重试";
    case AM_NOTE_CHANGED:return "已重新读取当前设置，请核对后再操作";
    case AM_NOTE_UNCERTAIN:return "保存结果未确认，请重新读取核对";
    case AM_NOTE_BAD:return "闹铃配置损坏，已保留原设置";
    case AM_NOTE_FOREIGN:return "闹铃配置版本暂不支持，已保留原设置";
    case AM_NOTE_READ_ERROR:return "读取失败，请重新读取核对";
    }return "";
}
