#include "calendar_runtime_core.h"
#include <string.h>
typedef enum { DISK_MISSING,DISK_GOOD,DISK_BAD,DISK_FOREIGN,DISK_ERROR } disk_kind;
typedef struct {disk_kind kind;size_t size;uint8_t raw[AF_YEAR_BYTES];af_year year;int error;} disk_value;
static disk_value read_slot(const cr_core *s,unsigned slot) {
    disk_value d={.kind=DISK_ERROR};
    if(!s->store.read)return d;
    cr_read_result r=s->store.read(s->store.ctx,slot,d.raw,sizeof d.raw,&d.size,&d.error);
    if(r==CR_READ_MISSING){d.kind=DISK_MISSING;d.error=0;return d;}
    if(r==CR_READ_OVERSIZE){d.kind=DISK_FOREIGN;return d;}
    if(r!=CR_READ_OK||d.size>sizeof d.raw)return d;
    if(af_year_decode(d.raw,d.size,&d.year)) {
        d.kind=(d.year.year&1u)==slot?DISK_GOOD:DISK_FOREIGN;return d;
    }
    /* Only recognized version1 derived data may be repaired. Unknown/future
     * layouts, including a missing/truncated magic/version, are preserved. */
    d.kind=d.size>=6u&&!memcmp(d.raw,"AFY1",4)&&d.raw[4]==1u&&d.raw[5]==0?DISK_BAD:DISK_FOREIGN;
    return d;
}
static bool publish(cr_core *s) {
    af_year years[2]={0};uint8_t n=0,blocked=0;
    for(unsigned i=0;i<2;++i) {
        if(s->slots[i].valid)years[n++]=s->slots[i].year;
        if(s->slots[i].blocked)blocked|=(uint8_t)(1u<<i);
    }
    if(n==2&&years[0].year>years[1].year){af_year swap=years[0];years[0]=years[1];years[1]=swap;}
    bool changed=n!=s->view.count||memcmp(years,s->view.years,sizeof years)!=0;
    if(changed&&s->view.revision==UINT32_MAX)return false;
    if(changed){memcpy(s->view.years,years,sizeof years);s->view.count=n;++s->view.revision;}
    s->view.blocked_mask=blocked;
    for(unsigned i=0;i<2;++i)s->view.next_due_ms[i]=s->targets[i]?s->schedule[i].due_ms:0;
    return true;
}
void cr_core_init(cr_core *s,cr_store store) {
    if(s){memset(s,0,sizeof *s);s->store=store;s->view.revision=1;}
}
void cr_core_reload(cr_core *s) {
    if(!s)return;
    for(unsigned i=0;i<2;++i) {
        disk_value d=read_slot(s,i);
        s->slots[i].blocked=d.kind==DISK_FOREIGN||d.kind==DISK_ERROR;
        if(d.kind==DISK_GOOD&&s->view.revision!=UINT32_MAX) {
            s->slots[i].valid=true;s->slots[i].year=d.year;
            memcpy(s->slots[i].raw,d.raw,sizeof d.raw);
        }
        if(d.kind==DISK_ERROR)s->view.storage_error=d.error;
        /* Missing/bad/unknown reads never destroy already approved RAM. */
    }
    (void)publish(s);
}
void cr_core_input(cr_core *s,const cr_input *in) {
    if(!s||!in)return;
    s->view.online=in->online;s->view.trusted=in->trusted;
    /* Keep an in-flight job's schedule identity stable. The single worker
     * applies any new calendar year on its next idle input after completion. */
    if(s->view.busy||!in->trusted||in->year<2000u||in->year>2099u)return;
    s->view.current_year=in->year;
    uint16_t targets[2]={in->year,in->year<2099u?(uint16_t)(in->year+1u):0};
    for(unsigned i=0;i<2;++i)if(s->targets[i]!=targets[i]) {
        s->targets[i]=targets[i];cc_schedule_init(&s->schedule[i],in->now_ms);
    }
    (void)publish(s);
}
bool cr_core_take(cr_core *s,uint64_t now,uint16_t *year) {
    if(!s||!year||s->view.busy)return false;
    for(unsigned i=0;i<2;++i)if(s->targets[i]&&cc_schedule_take(&s->schedule[i],now,s->view.online,s->view.trusted)) {
        s->view.busy=true;s->view.last_year=s->targets[i];*year=s->targets[i];
        if(s->view.attempts<UINT32_MAX)++s->view.attempts;
        return true;
    }
    return false;
}
static cc_result persist(cr_core *s,uint16_t year,const uint8_t *raw) {
    af_year decoded;if(!raw||!af_year_decode(raw,AF_YEAR_BYTES,&decoded)||decoded.year!=year)return CC_SCHEMA;
    unsigned slot=year&1u;cr_slot *ram=&s->slots[slot];
    if(ram->blocked)return CC_UNCERTAIN;
    if(s->view.revision==UINT32_MAX)return CC_SIZE;
    disk_value before=read_slot(s,slot);
    if(before.kind==DISK_FOREIGN||before.kind==DISK_ERROR) {
        ram->blocked=true;s->view.storage_error=before.error;return CC_UNCERTAIN;
    }
    if(before.kind!=DISK_GOOD||memcmp(before.raw,raw,AF_YEAR_BYTES)) {
        if(!s->store.write)return CC_STORAGE;
        int write_error=s->store.write(s->store.ctx,slot,raw,AF_YEAR_BYTES);
        disk_value after=read_slot(s,slot);s->view.storage_error=write_error?write_error:after.error;
        if(after.kind!=DISK_GOOD||memcmp(after.raw,raw,AF_YEAR_BYTES)) {
            bool old=after.kind==before.kind&&after.size==before.size&&
                (before.kind==DISK_MISSING||((before.kind==DISK_GOOD||before.kind==DISK_BAD)&&!memcmp(after.raw,before.raw,before.size)));
            if(old)return CC_STORAGE;
            ram->blocked=true;return CC_UNCERTAIN;
        }
    }
    s->view.storage_error=0;
    ram->valid=true;ram->blocked=false;ram->year=decoded;memcpy(ram->raw,raw,AF_YEAR_BYTES);
    return CC_OK;
}
cc_result cr_core_finish(cr_core *s,uint16_t year,cc_result prepared,cc_result adjacent,const uint8_t *raw,uint64_t now) {
    if(!s||!s->view.busy||year!=s->view.last_year)return CC_ARGUMENT;
    int index=s->targets[0]==year?0:s->targets[1]==year?1:-1;
    if(index<0)return CC_ARGUMENT;
    cc_result result=prepared==CC_OK?persist(s,year,raw):prepared;
    cc_schedule_finish(&s->schedule[index],now,result==CC_OK);
    s->view.busy=false;s->view.last_result=result;s->view.adjacent_status=adjacent;
    if(result==CC_OK&&s->view.successes<UINT32_MAX)++s->view.successes;
    (void)publish(s);return result;
}
bool cr_input_from_utc(bool online,bool trusted,int64_t utc_us,uint64_t now,cr_input *out) {
    if(!out)return false;
    cr_input v={.online=online,.trusted=trusted,.now_ms=now};
    if(trusted) {
        const int64_t first=INT64_C(946656000000000);
        if(utc_us<first)return false;
        uint64_t day=(uint64_t)(utc_us-first)/UINT64_C(86400000000);unsigned year=2000;
        while(year<=2099u&&day>=(year%4u==0?366u:365u)){day-=year%4u==0?366u:365u;++year;}
        if(year>2099u)return false;
        v.year=(uint16_t)year;
    }
    *out=v;return true;
}
