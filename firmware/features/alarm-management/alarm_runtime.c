#include "alarm_runtime.h"
#include <string.h>
static bool clock_read(ar_runtime *r,at_clock *c){
    memset(c,0,sizeof *c);
    return r->read_clock&&r->read_clock(r->clock_ctx,c);
}
static bool known(const as_result *s){return s->origin!=AS_ORIGIN_NONE;}
static void stop_pending(ar_runtime *r){
    if(atomic_exchange_explicit(&r->stop_pending,false,memory_order_acq_rel))at_stop(&r->trigger);
}
void ar_init(ar_runtime *r,as_port port,ar_clock_fn read,void *ctx){
    memset(r,0,sizeof *r);as_init(&r->store,port);at_init(&r->trigger);
    atomic_init(&r->stop_pending,false);r->read_clock=read;r->clock_ctx=ctx;r->storage_status=AS_BLOCKED;
}
void ar_request_stop(ar_runtime *r){if(r)atomic_store_explicit(&r->stop_pending,true,memory_order_release);}
static am_io_status ui_status(as_status s){
    switch(s){
    case AS_CURRENT:case AS_LEGACY:return AM_IO_OK;
    case AS_MISSING:return AM_IO_MISSING;
    case AS_SAVED:case AS_UNCHANGED:return AM_IO_SAVED;
    case AS_OLD_VALUE:case AS_DIFFERENT:return AM_IO_DIFFERENT;
    case AS_BAD:return AM_IO_BAD;
    case AS_FOREIGN:return AM_IO_FOREIGN;
    case AS_UNCERTAIN:return AM_IO_UNCERTAIN;
    default:return AM_IO_ERROR;
    }
}
am_reply ar_ui(ar_runtime *r,const am_job *job){
    am_reply reply={.token=job->token};stop_pending(r);
    as_result result=job->kind==AM_JOB_LOAD?as_reload(&r->store):as_save_ui(&r->store,&job->expected,&job->proposed);
    r->storage_status=result.status;
    reply.result=(am_io_result){.status=ui_status(result.status),.write_error=result.write_error,.read_error=result.read_error};
    if(known(&result)){
        reply.result.config=result.image.config;
        at_clock clock;bool valid=clock_read(r,&clock);stop_pending(r);
        bool quiet=r->trigger.phase==AT_SILENT||r->trigger.phase==AT_AUDIO_FAILED;
        bool accepted;
        if(!r->trigger.loaded||(job->kind==AM_JOB_LOAD&&r->trigger.storage_blocked&&quiet))accepted=at_restore(&r->trigger,&result.image);
        else accepted=valid&&at_observe_image(&r->trigger,&result.image,&clock);
        if(!accepted){reply.result.status=AM_IO_UNCERTAIN;r->trigger.storage_blocked=true;}
    }
    ++r->revision;return reply;
}
void ar_step(ar_runtime *r,aa_player *player,bool ready,const af_year *years,size_t count){
    at_clock clock;if(!clock_read(r,&clock))return;
    stop_pending(r);
    aa_result error=AA_INVALID_CONFIG;aa_state audio=player?aa_status(player,&error):AA_IDLE;
    if(audio==AA_FAILED||audio==AA_RECOVERING)ready=false;
    /* Bootstrap has no usable output path: do not consume a due occurrence.
     * Restoring readiness establishes a new baseline, without replay. */
    if(!ready)clock.trusted=false;
    (void)at_step(&r->trigger,&clock,years,count,audio,error);
    at_job job;
    if(at_take_job(&r->trigger,&job)){
        as_result result=as_save_trigger(&r->store,&job.expected,&job.proposed);
        r->storage_status=result.status;
        at_store_result status=result.status==AS_SAVED||result.status==AS_UNCHANGED?AT_COMMITTED:
            result.status==AS_OLD_VALUE?AT_UNCHANGED:result.status==AS_DIFFERENT?AT_DIFFERENT:AT_UNCERTAIN;
        (void)clock_read(r,&clock);
        audio=player?aa_status(player,NULL):AA_IDLE;
        if(audio==AA_FAILED||audio==AA_RECOVERING)ready=false;
        if(!ready)clock.trusted=false;
        stop_pending(r);
        (void)at_accept_job(&r->trigger,job.token,status,known(&result)?&result.image:NULL,&clock);
    }
    ++r->revision;
}
void ar_dispatch(ar_runtime *r,aa_player *player,bool ready){
    stop_pending(r);
    if(!ready&&r->trigger.phase==AT_START_COMMAND)at_stop(&r->trigger);
    /* Unavailability gates starts, never an outstanding stop command. */
    if(player)(void)at_dispatch_audio(&r->trigger,player);
}
void ar_copy_into(ar_runtime *r,aa_player *player,bool ready,const af_year *years,size_t count,ar_snapshot *out){
    ar_snapshot s={.revision=r->revision,.phase=r->trigger.phase,.error=r->trigger.error,
        .storage=r->storage_status,.audio_ready=ready,.known=r->trigger.loaded,
        .blocked=r->store.blocked||r->trigger.storage_blocked,.config=r->trigger.image.config,
        .session_count=r->trigger.session_count};
    s.audio=player?aa_status(player,&s.audio_error):AA_IDLE;
    if(s.audio==AA_FAILED||s.audio==AA_RECOVERING){ready=false;s.audio_ready=false;}
    s.active=s.phase==AT_START_COMMAND||s.phase==AT_STARTING||s.phase==AT_RINGING||s.phase==AT_STOPPING;
    s.next.status=AM_SUMMARY_HIDDEN;
    at_clock clock;int64_t wall;
    bool enabled=false;for(unsigned i=0;i<s.config.count;++i)enabled|=s.config.entries[i].enabled!=0;
    if(s.known&&!enabled)s.next.status=AM_SUMMARY_HIDDEN;
    else if(!s.known||s.blocked||!ready)s.next.status=AM_SUMMARY_UNKNOWN;
    else if(!clock_read(r,&clock)||!at_clock_wall(&clock,&wall)){
        for(unsigned i=0;i<s.config.count;++i)if(s.config.entries[i].enabled)s.next.status=AM_SUMMARY_WAIT;
    }else{
        af_time now;(void)at_civil_from_wall(wall,&now);
        for(unsigned i=0;i<s.config.count;++i){
            const am_entry *e=&s.config.entries[i];const at_record *seen=&r->trigger.seen[i];
            if(!e->enabled||(e->mode==AF_ONCE&&seen->once_revision==seen->revision))continue;
            af_time reference=now;
            if(seen->day1){
                int64_t after=(int64_t)seen->day1*86400000-1;
                if(after>wall)(void)at_civil_from_wall(after,&reference);
            }
            af_alarm alarm={true,true,e->hour,e->minute,(af_mode)e->mode};
            af_candidate candidate=af_next(&alarm,&reference,true,false,years,count);int64_t due,best;
            if(candidate.status!=AF_OK||!at_wall_ms(&candidate.at,&due)){s.next.status=AM_SUMMARY_UNKNOWN;break;}
            if(s.next.status!=AM_SUMMARY_NEXT||!at_wall_ms(&s.next.at,&best)||due<best){s.next.at=candidate.at;s.next.id=e->id;}
            s.next.status=AM_SUMMARY_NEXT;s.next.fallback|=candidate.calendar_fallback;
            if(e->mode==AF_WORKDAY){bool work,missing;if(af_is_workday(&now,years,count,&work,&missing))s.calendar_notice|=missing||candidate.calendar_fallback;}
        }
    }
    *out=s;
}
