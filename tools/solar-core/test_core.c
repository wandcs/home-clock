#include "solar_core.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static solar_civil_day_t civil(int y,int m,int d,int a,int b) {
    return (solar_civil_day_t){{y,m,d},a,b};
}
static solar_input_t input(solar_civil_day_t c, double lat, double lon) {
    int64_t start,end; assert(solar_civil_bounds(c,&start,&end));
    return (solar_input_t){.latitude=lat,.longitude=lon,.civil=c,.now_utc_ms=start+43200000,.trusted=true};
}
static void tests(void) {
    solar_day_t d;
    solar_civil_day_t sh = civil(2026,10,18,480,480), c;
    int64_t a,b;
    assert(solar_civil_bounds(sh,&a,&b) && b-a == 86400000);
    assert(solar_fixed_civil(a,480,&c) && c.date.day == 18);
    assert(solar_fixed_civil(a-1,480,&c) && c.date.day == 17);
    assert(solar_fixed_civil(-1,0,&c) && c.date.year == 1969 && c.date.month == 12 && c.date.day == 31);
    assert(solar_fixed_civil(-2177452800000LL,0,&c) && c.date.year == 1901 && c.date.day == 1);
    assert(solar_fixed_civil(4102444799999LL,0,&c) && c.date.year == 2099 && c.date.day == 31);
    assert(!solar_fixed_civil(INT64_MAX,840,&c) && !solar_fixed_civil(INT64_MIN,-840,&c));
    assert(!solar_fixed_civil(4102444800000LL,0,&c));
    assert(!solar_fixed_civil(-2177452800001LL,0,&c));
    assert(!solar_fixed_civil(0,841,&c) && !solar_fixed_civil(0,0,NULL));
    for (int y=1901; y<=2099; ++y) for(int m=1;m<=12;++m) for(int day=1;day<=31;++day) {
        c=civil(y,m,day,480,480);
        if(solar_civil_bounds(c,&a,&b)) {
            solar_civil_day_t back;
            assert(solar_fixed_civil(a,480,&back));
            assert(back.date.year==y && back.date.month==m && back.date.day==day);
        }
    }
    assert(solar_civil_bounds(civil(2026,3,8,-300,-240),&a,&b) && b-a==23*3600000LL);
    assert(solar_civil_bounds(civil(2026,11,1,-240,-300),&a,&b) && b-a==25*3600000LL);
    assert(solar_civil_bounds(civil(2026,10,4,630,660),&a,&b) && b-a==235*360000LL);
    assert(!solar_civil_bounds(civil(2011,12,30,-600,840),&a,&b)); /* skipped Apia */
    assert(!solar_civil_bounds(civil(2026,1,1,840,-840),&a,&b));
    assert(!solar_civil_bounds(civil(2026,2,29,0,0),&a,&b));
    assert(!solar_civil_bounds(civil(2100,1,1,0,0),&a,&b));
    assert(!solar_civil_bounds(civil(1900,1,1,0,0),&a,&b));
    assert(!solar_civil_bounds(civil(2026,0,1,0,0),&a,&b));
    assert(!solar_civil_bounds(civil(2026,1,0,0,0),&a,&b));
    assert(!solar_civil_bounds(civil(2026,1,1,-841,0),&a,&b));
    assert(solar_calculate(31.2304,121.4737,sh,&d) && d.count==2);
    for(unsigned i=0;i<d.count;++i) {
        solar_phase_t before=d.events[i].type==SOLAR_SUNRISE?SOLAR_NIGHT:SOLAR_DAY;
        solar_phase_t after=before==SOLAR_DAY?SOLAR_NIGHT:SOLAR_DAY;
        int64_t t=d.events[i].utc_ms;
        assert(solar_evaluate(&d,t-1,true).phase==before);
        assert(solar_evaluate(&d,t,true).phase==after);
        assert(solar_evaluate(&d,t+1,true).phase==after);
        assert(solar_evaluate(&d,t-1,true).next_check_utc_ms==t);
    }
    assert(solar_evaluate(&d,d.end_utc_ms,true).reason==SOLAR_STALE_WINDOW);
    assert(solar_evaluate(&d,d.start_utc_ms-1,true).phase==SOLAR_UNKNOWN);
    assert(solar_evaluate(&d,d.start_utc_ms,false).reason==SOLAR_UNTRUSTED_TIME);
    assert(solar_evaluate(NULL,0,true).reason==SOLAR_BAD_INPUT);
    assert(!solar_calculate(NAN,0,sh,&d) && d.reason==SOLAR_BAD_INPUT);
    assert(!solar_calculate(0,INFINITY,sh,&d));
    assert(!solar_calculate(-91,0,sh,&d));
    assert(!solar_calculate(0,181,sh,&d));
    assert(!solar_calculate(0,0,sh,NULL));
    assert(solar_calculate(69.6492,18.9553,civil(2026,6,21,120,120),&d));
    assert(d.status==SOLAR_CONTINUOUS_DAY && d.count==0);
    assert(solar_evaluate(&d,d.start_utc_ms,true).next_check_utc_ms==d.end_utc_ms);
    assert(solar_calculate(69.6492,18.9553,civil(2026,12,21,60,60),&d));
    assert(d.status==SOLAR_CONTINUOUS_NIGHT && d.count==0);

    solar_model_t model={0}; solar_input_t in=input(sh,31.2304,121.4737);
    solar_state_t s=solar_update(&model,&in); assert(s.phase==SOLAR_DAY && model.calculations==1);
    ++in.now_utc_ms; solar_update(&model,&in); assert(model.calculations==1);
    int64_t sunset=model.day.events[1].utc_ms;
    in.now_utc_ms=sunset; s=solar_update(&model,&in);
    assert(s.phase==SOLAR_NIGHT && !s.has_next_event && s.next_check_utc_ms==model.day.end_utc_ms);
    in.now_utc_ms=model.day.end_utc_ms; s=solar_update(&model,&in);
    assert(s.reason==SOLAR_STALE_WINDOW && !s.has_next_check && !model.cached);
    assert(solar_fixed_civil(in.now_utc_ms,480,&in.civil));
    s=solar_update(&model,&in); assert(s.phase==SOLAR_NIGHT && model.calculations==2);
    assert(s.next_event.type==SOLAR_SUNRISE);
    in.now_utc_ms=s.next_event.utc_ms; assert(solar_update(&model,&in).phase==SOLAR_DAY);
    assert(model.calculations==2);
    /* Small forwards sync, backwards jump, large forwards jump and resume. */
    ++in.clock_revision; ++in.now_utc_ms; solar_update(&model,&in); assert(model.calculations==3);
    in.now_utc_ms-=3600000; solar_update(&model,&in); assert(model.calculations==4);
    in.now_utc_ms=model.day.events[1].utc_ms+60000;
    s=solar_update(&model,&in); assert(s.phase==SOLAR_NIGHT);
    assert(model.calculations==4); /* skipped boundary evaluates present phase */
    in.trusted=false; s=solar_update(&model,&in); assert(s.reason==SOLAR_UNTRUSTED_TIME && !model.cached);
    in.trusted=true; solar_update(&model,&in); assert(model.calculations==5);
    ++in.location_revision; solar_update(&model,&in); assert(model.calculations==6);
    in.longitude=-74.006; in.latitude=40.7128;
    solar_update(&model,&in); assert(model.calculations==7);
    in.civil.end_offset_minutes=540; solar_update(&model,&in); assert(model.calculations==8);
    in.latitude=NAN; s=solar_update(&model,&in); assert(s.reason==SOLAR_BAD_INPUT && !model.cached);
    assert(solar_update(&model,NULL).reason==SOLAR_BAD_INPUT);
    assert(solar_update(NULL,&in).reason==SOLAR_BAD_INPUT);
    /* Midnight/year/leap transitions selected locally, no weather refresh. */
    const solar_civil_day_t dates[]={{{2026,12,31},480,480},{{2024,2,28},480,480},{{2024,2,29},480,480}};
    const int months[]={1,2,3}, days[]={1,29,1};
    for(unsigned i=0;i<3;++i) {
        in=input(dates[i],31.2304,121.4737);
        solar_update(&model,&in); in.now_utc_ms=model.day.end_utc_ms;
        assert(solar_fixed_civil(in.now_utc_ms,480,&in.civil));
        s=solar_update(&model,&in); assert(s.phase==SOLAR_NIGHT);
        assert(in.civil.date.month==months[i] && in.civil.date.day==days[i]);
        if(i==0) assert(in.civil.date.year==2027);
    }
    /* Same UTC, distant location including civil date change. */
    in=input(sh,31.2304,121.4737); in.now_utc_ms=1792288800000LL; /* 2026-10-18 02Z */
    assert(solar_update(&model,&in).phase==SOLAR_DAY);
    assert(solar_fixed_civil(in.now_utc_ms,-240,&in.civil));
    in.latitude=40.7128; in.longitude=-74.006;
    assert(solar_update(&model,&in).phase==SOLAR_NIGHT && in.civil.date.day==17);
    in.now_utc_ms+=3*86400000LL;
    assert(solar_update(&model,&in).reason==SOLAR_STALE_WINDOW);
    assert(solar_fixed_civil(in.now_utc_ms,-240,&in.civil));
    assert(solar_update(&model,&in).phase==SOLAR_NIGHT);
    in.now_utc_ms=INT64_MAX;
    assert(solar_update(&model,&in).reason==SOLAR_STALE_WINDOW);
    printf("native scenario assertions passed; sizeof(day)=%zu sizeof(model)=%zu sizeof(state)=%zu\n",
           sizeof(solar_day_t),sizeof(solar_model_t),sizeof(solar_state_t));
}
int main(int argc,char **argv) {
    if(argc==2 && strcmp(argv[1],"--batch")==0) {
        double lat,lon; int y,m,d,a,b;
        while(scanf("%lf %lf %d %d %d %d %d",&lat,&lon,&y,&m,&d,&a,&b)==7) {
            solar_day_t day; bool ok=solar_calculate(lat,lon,civil(y,m,d,a,b),&day);
            if(ok) for(unsigned i=0;i<day.count;++i) {
                int64_t t=day.events[i].utc_ms;
                solar_phase_t after=day.events[i].type==SOLAR_SUNRISE?SOLAR_DAY:SOLAR_NIGHT;
                solar_phase_t before=after==SOLAR_DAY?SOLAR_NIGHT:SOLAR_DAY;
                assert(solar_evaluate(&day,t-1,true).phase==before);
                assert(solar_evaluate(&day,t,true).phase==after);
                if(t+1<day.end_utc_ms) assert(solar_evaluate(&day,t+1,true).phase==after);
                assert(solar_evaluate(&day,t-1,true).next_check_utc_ms==t);
            }
            printf("{\"valid\":%s,\"status\":%d,\"reason\":%d,\"start\":%" PRId64 ",\"end\":%" PRId64 ",\"startPhase\":%d,\"events\":[",
                   ok?"true":"false",day.status,day.reason,day.start_utc_ms,day.end_utc_ms,day.start_phase);
            for(unsigned i=0;i<day.count;++i) printf("%s{\"type\":\"%s\",\"utcMs\":%" PRId64 "}",i?",":"",
                day.events[i].type==SOLAR_SUNRISE?"sunrise":"sunset",day.events[i].utc_ms);
            puts("]}");
        }
        return ferror(stdin)?1:0;
    }
    tests(); return 0;
}
