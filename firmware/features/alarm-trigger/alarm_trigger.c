#include "alarm_trigger.h"
#include <limits.h>
#include <string.h>

#define AT_LAST_WALL INT64_C(3155759999999)
static bool clock_guard(at_state *,const at_clock *,int64_t *,bool *);
static bool entry_equal(const am_entry *a,const am_entry *b) {
    return a->id==b->id&&a->hour==b->hour&&a->minute==b->minute&&a->mode==b->mode&&a->enabled==b->enabled;
}
/* Same stable ID: an explicitly changed enabled entry is a new plan.
 * Closing (including automatic one-shot close), no-op saves and clock-only
 * changes keep consumption; editing while closed takes effect on later enable. */
static bool new_plan(const am_entry *before,const am_entry *after) {
    return after->enabled&&!entry_equal(before,after);
}
bool at_image_valid(const at_image *p) {
    if(!p||!am_config_valid(&p->config))return false;
    for(unsigned i=0;i<p->config.count;++i) {
        const at_record *r=&p->records[i];
        if(!r->revision||r->day1>36525u||r->once_revision>r->revision)return false;
        if(r->once_revision&&!r->day1)return false;
        if(r->once_revision==r->revision&&(p->config.entries[i].mode!=AF_ONCE||!p->config.entries[i].enabled))return false;
    }
    return true;
}
bool at_image_equal(const at_image *a,const at_image *b) {
    if(!at_image_valid(a)||!at_image_valid(b)||!am_config_equal(&a->config,&b->config))return false;
    for(unsigned i=0;i<a->config.count;++i) {
        const at_record *x=&a->records[i],*y=&b->records[i];
        if(x->revision!=y->revision||x->once_revision!=y->once_revision||x->day1!=y->day1)return false;
    }
    return true;
}
bool at_image_first(const am_config *config,at_image *out) {
    if(!out||!am_config_valid(config))return false;
    at_image p={.config=*config};
    for(unsigned i=0;i<config->count;++i)p.records[i].revision=1;
    *out=p;
    return true;
}
bool at_image_reconfigure(const at_image *old,const am_config *config,at_image *out) {
    if(!out||!at_image_valid(old)||!am_config_valid(config)||config->next_id<old->config.next_id)return false;
    at_image p={.config=*config};
    for(unsigned i=0;i<config->count;++i) {
        int k=am_find(&old->config,config->entries[i].id);
        if(k<0) {
            if(config->entries[i].id<old->config.next_id)return false;
            p.records[i].revision=1;
        } else {
            p.records[i]=old->records[k];
            if(!entry_equal(&config->entries[i],&old->config.entries[k])) {
                if(p.records[i].revision==UINT32_MAX)return false;
                ++p.records[i].revision;
                p.records[i].once_revision=0;
                if(new_plan(&old->config.entries[k],&config->entries[i]))p.records[i].day1=0;
            }
        }
    }
    *out=p;
    return true;
}
static bool forward_image(const at_image *old,const at_image *p) {
    if(!at_image_valid(p)||p->config.next_id<old->config.next_id)return false;
    for(unsigned i=0;i<p->config.count;++i) {
        int k=am_find(&old->config,p->config.entries[i].id);
        if(k<0) {
            if(p->config.entries[i].id<old->config.next_id)return false;
        } else {
            if(p->records[i].revision<old->records[k].revision)return false;
            bool fresh=p->records[i].revision>old->records[k].revision&&
                new_plan(&old->config.entries[k],&p->config.entries[i]);
            if(p->records[i].day1<old->records[k].day1&&!fresh)return false;
            if(!entry_equal(&p->config.entries[i],&old->config.entries[k])&&p->records[i].revision<=old->records[k].revision)return false;
        }
    }
    return true;
}
static bool leap(unsigned y) { return y%4u==0; } /* Supported years 2000..2099. */
bool at_wall_ms(const af_time *t,int64_t *out) {
    uint16_t index;
    if(!out||!af_day_index(t,&index))return false;
    uint32_t day=index;
    for(unsigned y=2000;y<t->year;++y)day+=leap(y)?366u:365u;
    *out=(int64_t)day*86400000+(int64_t)t->hour*3600000+(int64_t)t->minute*60000+(int64_t)t->second*1000;
    return true;
}
bool at_clock_wall(const at_clock *c,int64_t *out) {
    if(!c||!out||!c->trusted||!c->revision||c->wall_anchor_ms<0||c->wall_anchor_ms>AT_LAST_WALL||c->now_mono_ms<c->mono_anchor_ms)return false;
    uint64_t d=c->now_mono_ms-c->mono_anchor_ms;
    if(d>(uint64_t)(AT_LAST_WALL-c->wall_anchor_ms))return false;
    *out=c->wall_anchor_ms+(int64_t)d;
    return true;
}
bool at_clock_from_sync(bool trusted,uint32_t revision,int64_t utc_us,uint64_t sync_mono,uint64_t now,at_clock *out) {
    if(!out)return false;
    at_clock c={.trusted=trusted,.revision=revision,.mono_anchor_ms=sync_mono,.now_mono_ms=now};
    if(trusted) {
        int64_t ms=utc_us/1000;
        const int64_t first=INT64_C(946656000000); /* 2000-01-01 00:00 UTC+8. */
        if(ms<first||ms>first+AT_LAST_WALL||!revision||now<sync_mono)return false;
        c.wall_anchor_ms=ms-first;
        int64_t checked;if(!at_clock_wall(&c,&checked))return false;
    }
    *out=c;return true;
}
static af_time wall_time(int64_t wall) {
    uint32_t day=(uint32_t)(wall/86400000),seconds=(uint32_t)((wall%86400000)/1000);
    unsigned y=2000;
    while(day>=(leap(y)?366u:365u)) { day-=leap(y)?366u:365u; ++y; }
    static const unsigned lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned m=1;
    while(day>=lengths[m-1u]+((m==2u&&leap(y))?1u:0u)) {
        day-=lengths[m-1u]+((m==2u&&leap(y))?1u:0u); ++m;
    }
    return (af_time){(uint16_t)y,(uint8_t)m,(uint8_t)(day+1u),(uint8_t)(seconds/3600u),(uint8_t)((seconds/60u)%60u),(uint8_t)(seconds%60u)};
}
bool at_civil_from_wall(int64_t wall,af_time *out) {
    if(!out||wall<0||wall>AT_LAST_WALL)return false;
    *out=wall_time(wall);return true;
}
static bool same_mapping(const at_clock *a,const at_clock *b) {
    return a->revision==b->revision&&a->wall_anchor_ms==b->wall_anchor_ms&&a->mono_anchor_ms==b->mono_anchor_ms;
}
void at_init(at_state *s) {
    if(!s)return;
    memset(s,0,sizeof *s);
    s->storage_blocked=true;
}
static void adopt(at_state *s,const at_image *p,bool preserve) {
    at_record next[AM_CAPACITY]={0};
    uint64_t next_after[AM_CAPACITY]={0};
    for(unsigned i=0;i<p->config.count;++i) {
        next[i]=p->records[i];
        int k=preserve?am_find(&s->image.config,p->config.entries[i].id):-1;
        if(k>=0) {
            bool fresh=next[i].revision>s->image.records[k].revision&&
                new_plan(&s->image.config.entries[k],&p->config.entries[i]);
            if(!fresh&&s->seen[k].day1>next[i].day1)next[i].day1=s->seen[k].day1;
            if(s->seen[k].revision==next[i].revision&&s->seen[k].once_revision)next[i].once_revision=s->seen[k].once_revision;
            if(s->seen[k].revision==next[i].revision)next_after[i]=s->effective_after[k];
        }
        if(next[i].once_revision!=next[i].revision)next[i].once_revision=0;
    }
    s->image=*p;
    memcpy(s->seen,next,sizeof next);
    memcpy(s->effective_after,next_after,sizeof next_after);
    s->loaded=true;
}
bool at_restore(at_state *s,const at_image *p) {
    if(!s||!at_image_valid(p)||s->phase==AT_START_COMMAND||s->phase==AT_STARTING||s->phase==AT_RINGING||s->phase==AT_STOPPING)return false;
    if(s->loaded&&!forward_image(&s->image,p))return false;
    adopt(s,p,s->loaded);
    s->storage_blocked=false;
    s->job_pending=false;s->job_taken=false;s->batch_valid=false;
    s->command_pending=false;s->command_taken=false;s->baseline=false;
    s->error=AT_ERROR_NONE;
    return true;
}
bool at_observe_image(at_state *s,const at_image *p,const at_clock *confirmed_at) {
    if(!s||!confirmed_at||!s->loaded||!forward_image(&s->image,p))return false;
    int64_t boundary=0;bool mapping_changed=false;
    bool clock_ok=clock_guard(s,confirmed_at,&boundary,&mapping_changed);
    if(!clock_ok)s->baseline=false;
    if((s->phase==AT_START_COMMAND||s->phase==AT_STARTING||s->phase==AT_RINGING)&&confirmed_at->now_mono_ms>=s->deadline_ms)at_stop(s);
    bool changed[AM_CAPACITY]={0};
    for(unsigned i=0;i<p->config.count;++i) {
        int k=am_find(&s->image.config,p->config.entries[i].id);
        changed[i]=k<0||s->image.records[k].revision!=p->records[i].revision;
    }
    if(!at_image_equal(&s->image,p)) {
        s->batch_valid=false;
        if(s->phase==AT_START_COMMAND)at_stop(s);
    }
    unsigned kept=0;
    for(unsigned i=0;i<s->session_count;++i)if(am_find(&p->config,s->session_ids[i])>=0)s->session_ids[kept++]=s->session_ids[i];
    s->session_count=(uint8_t)kept;
    adopt(s,p,true);
    for(unsigned i=0;i<p->config.count;++i)if(changed[i])s->effective_after[i]=confirmed_at->now_mono_ms;
    return true;
}
static bool audio_command(at_state *s,at_audio_kind kind) {
    if(s->next_audio_token==UINT32_MAX&&kind==AT_AUDIO_START) {
        s->error=AT_SEQUENCE_LIMIT;return false;
    }
    if(s->next_audio_token<UINT32_MAX)++s->next_audio_token;
    s->audio_command=(at_audio_command){s->next_audio_token,kind};
    s->command_pending=true;s->command_taken=false;
    return true;
}
void at_stop(at_state *s) {
    if(!s)return;
    s->batch_valid=false;
    if(s->phase==AT_START_COMMAND&&!s->command_taken) {
        s->command_pending=false;s->phase=AT_SILENT;
    } else if(s->phase==AT_START_COMMAND||s->phase==AT_STARTING||s->phase==AT_RINGING) {
        (void)audio_command(s,AT_AUDIO_STOP);s->phase=AT_STOPPING;s->quiet_observed=false;
    }
}
/* Only one consecutive, fresh event inside the last poll interval is known.
 * Other revisions carry no event history and must establish a new baseline. */
static bool transition(const at_state *s,const at_clock *c,int64_t *old_event) {
    if(!s->baseline||s->mapping.revision==UINT32_MAX||c->revision!=s->mapping.revision+1u||
       c->mono_anchor_ms<s->poll_mono||c->now_mono_ms<c->mono_anchor_ms||
       c->now_mono_ms-c->mono_anchor_ms>AT_POLL_MAX_MS||
       c->now_mono_ms<s->poll_mono||c->now_mono_ms-s->poll_mono>AT_POLL_MAX_MS)return false;
    at_clock old=s->mapping;old.now_mono_ms=c->mono_anchor_ms;
    return at_clock_wall(&old,old_event);
}
static bool fact_survives(const at_state *s,const at_clock *c,uint64_t due_mono) {
    if(same_mapping(c,&s->mapping))return true;
    int64_t old_event;
    return transition(s,c,&old_event)&&
        (old_event==c->wall_anchor_ms||due_mono<=c->mono_anchor_ms);
}
static bool clock_guard(at_state *s,const at_clock *c,int64_t *wall,bool *changed) {
    *changed=false;
    if(!c)return false;
    if(s->have_mono&&c->now_mono_ms<s->last_mono) {
        s->error=AT_CLOCK_CONTRACT;s->baseline=false;s->batch_valid=false;at_stop(s);return false;
    }
    s->have_mono=true;s->last_mono=c->now_mono_ms;
    if(!at_clock_wall(c,wall)) {
        s->baseline=false;s->batch_valid=false;
        if(s->phase==AT_START_COMMAND)at_stop(s);
        if(c->trusted)s->error=AT_CLOCK_CONTRACT;
        return false;
    }
    *changed=!s->baseline||!same_mapping(c,&s->mapping);
    if(*changed) {
        if(!fact_survives(s,c,s->batch_due_mono))s->batch_valid=false;
        if(s->phase==AT_START_COMMAND&&!fact_survives(s,c,s->session_due_mono))at_stop(s);
    }
    return true;
}
static bool record_equal(const at_record *a,const at_record *b) {
    return a->revision==b->revision&&a->once_revision==b->once_revision&&a->day1==b->day1;
}
static bool make_job(at_state *s,at_job_kind kind,const at_image *proposed,uint64_t now) {
    if(s->job_pending||s->storage_blocked)return false;
    if(s->next_job_token==UINT32_MAX) { s->storage_blocked=true;s->error=AT_SEQUENCE_LIMIT;return false; }
    ++s->next_job_token;
    s->job=(at_job){s->next_job_token,kind,s->image,*proposed};
    s->job_pending=true;s->job_taken=false;s->job_started_ms=now;
    return true;
}
static void flush_intent(at_state *s,uint64_t now,uint8_t due_mask,bool fallback) {
    if(!s->loaded||s->storage_blocked||s->job_pending)return;
    at_image proposed=s->image;
    bool dirty=false,close=false;
    bool end=s->phase==AT_SILENT||s->phase==AT_AUDIO_FAILED;
    for(unsigned i=0;i<proposed.config.count;++i) {
        proposed.records[i]=s->seen[i];
        at_record *r=&proposed.records[i];am_entry *e=&proposed.config.entries[i];
        if(end&&r->once_revision) {
            if(r->once_revision==r->revision&&e->mode==AF_ONCE&&e->enabled) {
                if(r->revision==UINT32_MAX) { s->error=AT_SEQUENCE_LIMIT;s->storage_blocked=true;return; }
                e->enabled=0;++r->revision;
            }
            r->once_revision=0;close=true;
        }
        dirty|=!record_equal(r,&s->image.records[i]);
    }
    /* Newly due once entries must first be marked without closing them. */
    if(due_mask) {
        proposed=s->image;
        for(unsigned i=0;i<proposed.config.count;++i)proposed.records[i]=s->seen[i];
        close=false;dirty=true;
    }
    if(!dirty)return;
    if(!make_job(s,close?AT_CLOSE_ONCE:AT_MARK,&proposed,now))return;
    s->batch_count=0;s->batch_valid=false;s->batch_fallback=fallback;
    if(due_mask) {
        for(unsigned i=0;i<s->image.config.count;++i)if(due_mask&(1u<<i)) {
            unsigned k=s->batch_count++;
            s->batch_ids[k]=s->image.config.entries[i].id;s->batch_revisions[k]=s->seen[i].revision;
        }
        s->batch_valid=s->phase==AT_SILENT||s->phase==AT_STARTING||s->phase==AT_RINGING||s->phase==AT_START_COMMAND;
    }
}
static void observe_audio(at_state *s,aa_state audio,aa_result error) {
    if(s->phase==AT_STARTING||s->phase==AT_RINGING) {
        if(audio==AA_PLAYING||audio==AA_WRITING)s->phase=AT_RINGING;
        else if(audio==AA_FAILED||audio==AA_IDLE) {
            s->phase=AT_AUDIO_FAILED;s->audio_error=error==AA_OK?AA_OPEN_ERROR:error;s->error=AT_AUDIO_ERROR;s->quiet_observed=audio==AA_IDLE;
        }
    } else if(s->phase==AT_STOPPING) {
        if(audio==AA_IDLE) { s->phase=AT_SILENT;s->quiet_observed=true;s->command_pending=false; }
        else if(audio==AA_FAILED) { s->phase=AT_AUDIO_FAILED;s->error=AT_AUDIO_ERROR;s->audio_error=error;s->quiet_observed=false; }
    } else if(s->phase==AT_AUDIO_FAILED&&audio==AA_IDLE) {
        s->phase=AT_SILENT;s->quiet_observed=true;
    }
}
static bool calendars_valid(const af_year *years,size_t n) {
    if(n>100u||(n&&!years))return false;
    for(size_t i=0;i<n;++i)if(!af_year_valid(&years[i])||(i&&years[i-1u].year>=years[i].year))return false;
    return true;
}
/* A segment is actual continuous mono progression under exactly one mapping.
 * The strict edit boundary is projected into this segment, not copied across
 * wall-time corrections. Latest due mono proves ALL members of a batch. */
static uint8_t scan_segment(at_state *s,int64_t first,int64_t last,uint64_t first_mono,
                            uint64_t last_mono,const af_year *years,size_t n,bool *fallback,uint64_t *latest) {
    uint8_t mask=0;
    if(first>=last||first_mono>last_mono)return 0;
    for(unsigned i=0;i<s->image.config.count;++i) {
        const am_entry *e=&s->image.config.entries[i];int64_t after=first;
        if(s->effective_after[i]>first_mono) {
            if(s->effective_after[i]>=last_mono)continue;
            after+=(int64_t)(s->effective_after[i]-first_mono);
        }
        if(e->mode==AF_ONCE&&s->seen[i].once_revision==s->seen[i].revision)continue;
        af_time reference=wall_time(after);
        af_alarm alarm={e->enabled!=0,true,e->hour,e->minute,(af_mode)e->mode};
        af_candidate candidate=af_next(&alarm,&reference,true,false,years,n);int64_t due;
        if(candidate.status!=AF_OK||!at_wall_ms(&candidate.at,&due)||due>last)continue;
        uint16_t day1=(uint16_t)(due/86400000+1);
        if(day1<=s->seen[i].day1)continue;
        s->seen[i].day1=day1;
        if(e->mode==AF_ONCE)s->seen[i].once_revision=s->seen[i].revision;
        uint64_t occurrence=first_mono+(uint64_t)(due-first);
        if(occurrence>*latest)*latest=occurrence;
        mask|=(uint8_t)(1u<<i);*fallback|=candidate.calendar_fallback;
    }
    return mask;
}
bool at_step(at_state *s,const at_clock *c,const af_year *years,size_t n,aa_state audio,aa_result error) {
    if(!s||!c)return false;
    bool valid_audio=(unsigned)audio<=AA_RECOVERING&&(unsigned)error<=AA_QUIET_ERROR;
    bool valid_inputs=valid_audio&&calendars_valid(years,n);
    int64_t wall=0;bool changed=false;
    uint64_t previous_poll=s->poll_mono;
    bool had_baseline=s->baseline;
    bool clock_ok=clock_guard(s,c,&wall,&changed);
    if(valid_audio)observe_audio(s,audio,error);
    if((s->phase==AT_START_COMMAND||s->phase==AT_STARTING||s->phase==AT_RINGING)&&c->now_mono_ms>=s->deadline_ms)at_stop(s);
    if(s->job_pending&&(c->now_mono_ms<s->job_started_ms||c->now_mono_ms-s->job_started_ms>AT_CHECKPOINT_MAX_MS)) {
        s->batch_valid=false;s->error=AT_CHECKPOINT_EXPIRED;
    }
    if(!valid_inputs)s->error=AT_BAD_INPUT;
    uint8_t due_mask=0;bool fallback=false;
    bool gap=had_baseline&&c->now_mono_ms>=previous_poll&&c->now_mono_ms-previous_poll>AT_POLL_MAX_MS;
    if(clock_ok&&s->loaded) {
        uint64_t latest=0;
        if(valid_inputs&&had_baseline&&!gap) {
            if(!changed&&wall>=s->previous_wall)due_mask=scan_segment(s,s->previous_wall,wall,previous_poll,c->now_mono_ms,years,n,&fallback,&latest);
            else {
                int64_t old_event;
                if(transition(s,c,&old_event)) {
                    due_mask=scan_segment(s,s->previous_wall,old_event,previous_poll,c->mono_anchor_ms,years,n,&fallback,&latest);
                    due_mask|=scan_segment(s,c->wall_anchor_ms,wall,c->mono_anchor_ms,c->now_mono_ms,years,n,&fallback,&latest);
                }
            }
        }
        if(gap) { if(valid_inputs)s->error=AT_POLL_GAP;s->batch_valid=false; }
        if(due_mask&&!s->job_pending)s->batch_due_mono=latest;
        s->mapping=*c;s->previous_wall=wall;s->poll_mono=c->now_mono_ms;s->baseline=true;
    }
    if(due_mask&&s->job_pending) {
        s->skipped_busy+=s->skipped_busy<UINT32_MAX?1u:0u;
        /* No queue: RAM high-water remains dirty for a later silent save. */
        due_mask=0;
    }
    flush_intent(s,c->now_mono_ms,due_mask,fallback);
    return valid_inputs&&(clock_ok||!c->trusted);
}
bool at_take_job(at_state *s,at_job *out) {
    if(!s||!out||!s->job_pending||s->job_taken)return false;
    s->job_taken=true;*out=s->job;return true;
}
static bool batch_current(const at_state *s) {
    for(unsigned i=0;i<s->batch_count;++i) {
        int k=am_find(&s->image.config,s->batch_ids[i]);
        if(k<0||s->image.records[k].revision!=s->batch_revisions[i]||!s->image.config.entries[k].enabled)return false;
    }
    return s->batch_count>0;
}
static void add_session(at_state *s) {
    for(unsigned i=0;i<s->batch_count;++i) {
        bool found=false;
        for(unsigned k=0;k<s->session_count;++k)found|=s->session_ids[k]==s->batch_ids[i];
        if(!found&&s->session_count<AM_CAPACITY)s->session_ids[s->session_count++]=s->batch_ids[i];
    }
    s->session_fallback|=s->batch_fallback;
}
bool at_accept_job(at_state *s,uint32_t token,at_store_result result,const at_image *readback,const at_clock *c) {
    if(!s||!c||(unsigned)result>AT_UNCERTAIN||!s->job_pending||token!=s->job.token||!s->job_taken)return false;
    int64_t wall=0;bool changed=false;
    bool clock_ok=clock_guard(s,c,&wall,&changed);
    bool timely=c->now_mono_ms>=s->job_started_ms&&c->now_mono_ms-s->job_started_ms<=AT_CHECKPOINT_MAX_MS;
    bool poll_fresh=s->baseline&&c->now_mono_ms>=s->poll_mono&&c->now_mono_ms-s->poll_mono<=AT_POLL_MAX_MS;
    if(!poll_fresh) { s->batch_valid=false;s->baseline=false; }
    bool matched=result==AT_COMMITTED&&at_image_equal(readback,&s->job.proposed)&&forward_image(&s->image,readback);
    bool sound=matched&&s->job.kind==AT_MARK&&s->batch_valid&&timely&&poll_fresh&&clock_ok;
    s->job_pending=false;s->job_taken=false;
    if(matched) {
        adopt(s,readback,true);
        s->storage_blocked=false;
    } else if(result==AT_UNCHANGED&&at_image_equal(readback,&s->job.expected)) {
        s->storage_blocked=true;s->error=AT_STORAGE_ERROR;
    } else if(result==AT_DIFFERENT&&at_image_valid(readback)&&forward_image(&s->image,readback)) {
        adopt(s,readback,true);s->storage_blocked=true;s->error=AT_STORAGE_ERROR;
    } else {
        s->storage_blocked=true;s->error=AT_STORAGE_UNCERTAIN;
    }
    if(!timely&&s->error!=AT_CLOCK_CONTRACT)s->error=AT_CHECKPOINT_EXPIRED;
    sound&=batch_current(s);
    if(sound&&(s->phase==AT_STARTING||s->phase==AT_RINGING||s->phase==AT_START_COMMAND))add_session(s);
    else if(sound&&s->phase==AT_SILENT) {
        if(c->now_mono_ms>UINT64_MAX-AT_RING_MS||!audio_command(s,AT_AUDIO_START)) {
            s->error=AT_SEQUENCE_LIMIT;
        } else {
            s->start_ms=c->now_mono_ms;s->deadline_ms=s->start_ms+AT_RING_MS;
            s->session_due_mono=s->batch_due_mono;
            s->phase=AT_START_COMMAND;s->quiet_observed=false;s->session_count=0;s->session_fallback=false;add_session(s);
        }
    }
    s->batch_valid=false;
    return true;
}
bool at_take_audio(at_state *s,at_audio_command *out) {
    if(!s||!out||!s->command_pending||s->command_taken)return false;
    s->command_taken=true;*out=s->audio_command;return true;
}
bool at_accept_audio(at_state *s,at_audio_command command,aa_result result) {
    if(!s||(unsigned)result>AA_QUIET_ERROR||!s->command_pending||!s->command_taken||command.token!=s->audio_command.token||command.kind!=s->audio_command.kind)return false;
    s->command_pending=false;
    if(command.kind==AT_AUDIO_START) {
        if(result==AA_OK)s->phase=AT_STARTING;
        else {
            s->error=AT_AUDIO_ERROR;s->audio_error=result;s->quiet_observed=false;
            if(result==AA_BUSY) {
                /* An unexpected busy owner may still be playing. Request stop
                 * and wait for IDLE/FAILED before closing the once entry. */
                (void)audio_command(s,AT_AUDIO_STOP);s->phase=AT_STOPPING;
            } else s->phase=AT_AUDIO_FAILED;
        }
    }
    return true;
}
