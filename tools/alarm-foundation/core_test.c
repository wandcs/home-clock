#include "alarm_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static af_year calendar;
static af_time t(unsigned y,unsigned m,unsigned d,unsigned h,unsigned min,unsigned s)
{ return (af_time){(uint16_t)y,(uint8_t)m,(uint8_t)d,(uint8_t)h,(uint8_t)min,(uint8_t)s}; }
static void self_test(const uint8_t *raw)
{
    assert(sizeof(af_year) == 94u);
    assert(af_year_decode(raw,AF_YEAR_BYTES,&calendar));
    af_year original = calendar, out = calendar;
    for (size_t n=0;n<AF_YEAR_BYTES;n++) {
        assert(!af_year_decode(raw,n,&out));
        assert(memcmp(&out,&original,sizeof out)==0);
    }
    assert(!af_year_decode(raw,AF_YEAR_BYTES+1u,&out));
    for (unsigned i=0;i<AF_YEAR_BYTES;i++) {
        uint8_t broken[AF_YEAR_BYTES]; memcpy(broken,raw,sizeof broken); broken[i]^=1;
        assert(!af_year_decode(broken,sizeof broken,&out));
        assert(memcmp(&out,&original,sizeof out)==0);
    }
    assert(!af_year_decode(NULL,0,&out));
    assert(!af_year_decode(raw,AF_YEAR_BYTES,NULL));
    af_alarm a={true,true,7,0,AF_WORKDAY};
    af_time r=t(2026,2,14,6,59,59);
    af_candidate c=af_next(&a,&r,true,true,&calendar,1);
    assert(c.status==AF_OK && c.at.day==14 && !c.calendar_fallback);
    r=t(2026,2,15,0,0,0); c=af_next(&a,&r,true,true,&calendar,1);
    assert(c.status==AF_OK && c.at.day==24);
    r=t(2026,1,1,0,0,0); c=af_next(&a,&r,true,true,NULL,0);
    assert(c.status==AF_OK && c.at.day==1 && c.calendar_fallback);
    c=af_next(&a,&r,true,true,&calendar,1);
    assert(c.status==AF_OK && c.at.day==4 && !c.calendar_fallback);
    c=af_next(&a,NULL,false,true,NULL,0); assert(c.status==AF_WAIT_TIME);
    a.enabled=false; c=af_next(&a,NULL,false,true,NULL,0); assert(c.status==AF_DISABLED);
    a.enabled=true; a.mode=AF_ONCE; a.once_pending=false;
    c=af_next(&a,&r,true,true,NULL,0); assert(c.status==AF_CONSUMED);
    a.once_pending=true; a.mode=AF_DAILY;
    r=t(2026,10,20,7,0,0); c=af_next(&a,&r,true,true,NULL,0);
    assert(c.status==AF_OK && c.at.day==20);
    c=af_next(&a,&r,true,false,NULL,0); assert(c.status==AF_OK && c.at.day==21);
    r.second=1; c=af_next(&a,&r,true,true,NULL,0); assert(c.status==AF_OK && c.at.day==21);
    r=t(2099,12,31,23,59,59); c=af_next(&a,&r,true,true,NULL,0); assert(c.status==AF_RANGE_END);
    r=t(2000,2,29,6,0,0); assert(af_time_valid(&r));
    r.year=2001; assert(!af_time_valid(&r));
    r.year=2100; assert(!af_time_valid(&r));
    assert(!af_time_valid(NULL));
    af_time bad[]={t(1999,1,1,0,0,0),t(2000,0,1,0,0,0),t(2000,13,1,0,0,0),
        t(2000,2,30,0,0,0),t(2000,1,0,0,0,0),t(2000,1,1,24,0,0),
        t(2000,1,1,0,60,0),t(2000,1,1,0,0,60)};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) assert(!af_time_valid(&bad[i]));
    af_year broken=calendar; broken.off[45]|=128u; assert(!af_year_valid(&broken));
    broken=calendar; broken.work[0]|=1u; assert(!af_year_valid(&broken));
    r=t(2026,10,20,0,0,0); a.mode=AF_WORKDAY;
    assert(af_next(&a,&r,true,true,&broken,1).status==AF_INVALID);
    af_year dup[]={calendar,calendar};
    assert(af_next(&a,&r,true,true,dup,2).status==AF_INVALID);
    assert(af_next(&a,&r,true,true,NULL,1).status==AF_INVALID);
    assert(af_next(&a,&r,true,true,&calendar,101).status==AF_INVALID);
    a.mode=AF_DAILY; assert(af_next(&a,&r,true,true,&broken,1).status==AF_OK);
    a.mode=(af_mode)3; assert(af_next(&a,&r,true,true,NULL,0).status==AF_INVALID);
    a.mode=AF_DAILY; a.hour=24; assert(af_next(&a,&r,true,true,NULL,0).status==AF_INVALID);
    a.hour=7; a.minute=60; assert(af_next(&a,&r,true,true,NULL,0).status==AF_INVALID);
    a.minute=0; assert(af_next(NULL,&r,true,true,NULL,0).status==AF_INVALID);
    /* Valid shape can be all-off: bounded search must terminate, never hang. */
    af_year off[2]={{.year=2024},{.year=2025}};
    for(unsigned k=0;k<2;k++) for(unsigned d=0;d<(k?365u:366u);d++)
        off[k].off[d/8u]|=(uint8_t)(1u<<(d%8u));
    a.mode=AF_WORKDAY; r=t(2024,1,1,0,0,0);
    assert(af_next(&a,&r,true,true,off,2).status==AF_NO_CANDIDATE);
    bool work=true, fallback=false;
    assert(!af_is_workday(NULL,NULL,0,&work,&fallback)); assert(work && !fallback);
    fprintf(stderr,"core checks OK; year=%zu time=%zu alarm=%zu candidate=%zu bytes\n",
            sizeof(af_year),sizeof(af_time),sizeof(af_alarm),sizeof(af_candidate));
}
int main(int argc,char **argv)
{
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    uint8_t raw[AF_YEAR_BYTES+1u]; size_t size=fread(raw,1,sizeof raw,f); fclose(f);
    assert(size==AF_YEAR_BYTES); self_test(raw);
    /* Line protocol for independent Python reference comparison. No device input. */
    unsigned y,m,d,h,min,s,ah,am,mode,inclusive,known;
    while(scanf("%u %u %u %u %u %u %u %u %u %u %u",&y,&m,&d,&h,&min,&s,
                &ah,&am,&mode,&inclusive,&known)==11) {
        af_time r=t(y,m,d,h,min,s);
        af_alarm a={true,true,(uint8_t)ah,(uint8_t)am,(af_mode)mode};
        af_candidate c=af_next(&a,&r,true,inclusive!=0,known?&calendar:NULL,known?1u:0u);
        bool work=false,fallback=false;
        assert(af_is_workday(&r,known?&calendar:NULL,known?1u:0u,&work,&fallback));
        printf("%u %u %u %u %u %u %u %u %u %u\n",(unsigned)c.status,c.at.year,c.at.month,
               c.at.day,c.at.hour,c.at.minute,c.at.second,(unsigned)c.calendar_fallback,
               (unsigned)work,(unsigned)fallback);
    }
    return 0;
}
