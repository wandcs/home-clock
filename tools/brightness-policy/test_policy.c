#include "brightness_policy.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static unsigned long checks, steps;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static bp_output_t advance(bp_state_t *s, bp_input_t in, uint64_t now) {
    bp_output_t out;
    uint64_t dt = now - s->last_tick_ms;
    if (dt > BP_TICK_CAP_MS) dt = BP_TICK_CAP_MS;
    int32_t previous = s->target_mp;
    CHECK(bp_step(s, in, now, &out));
    CHECK(out.target_millipercent >= BP_MIN_MP && out.target_millipercent <= BP_MAX_MP);
    CHECK(out.demand_millipercent >= BP_MIN_MP && out.demand_millipercent <= BP_MAX_MP);
    CHECK(out.target_percent >= 2 && out.target_percent <= 80);
    CHECK((int64_t)out.target_millipercent - previous <= (int64_t)dt * BP_UP_MP_PER_MS);
    CHECK((int64_t)previous - out.target_millipercent <= (int64_t)dt * BP_DOWN_MP_PER_MS);
    CHECK(!out.low_light_known || out.mode == BP_TRACKING);
    ++steps;
    return out;
}

static bp_input_t fresh(float lux, uint64_t stamp) {
    return (bp_input_t){.lux=lux, .acquired_ms=stamp, .valid=true};
}

static void boundaries(void) {
    CHECK(bp_curve(0) == 2000 && bp_curve(3) == 4000);
    CHECK(bp_curve(10) == 8000 && bp_curve(50) == 20000);
    CHECK(bp_curve(200) == 40000 && bp_curve(500) == 65000);
    CHECK(bp_curve(1000) == 80000 && bp_curve(FLT_MAX) == 80000);
    CHECK(bp_curve(-1) == -1 && bp_curve(NAN) == -1);
    CHECK(bp_curve(INFINITY) == -1 && bp_curve(-INFINITY) == -1);
    int32_t last = bp_curve(0);
    for (unsigned i=1; i<=100000; ++i) {
        int32_t next = bp_curve((float)i / 100.0f);
        CHECK(next >= last && next <= BP_MAX_MP);
        last = next;
    }
    bp_state_t s;
    bp_init(&s, 0, 0); CHECK(s.target_mp == BP_MIN_MP);
    bp_init(&s, 0, ~0u); CHECK(s.target_mp == BP_MAX_MP);

    /* Polling one cached dark sample cannot advance either confirmation. */
    bp_init(&s, 0, 50);
    bp_output_t out = {0};
    for (uint64_t t=0;t<=6000;t+=100) out=advance(&s, fresh(0,0),t);
    CHECK(out.target_percent == 50 && !out.low_light && !out.low_light_known);
    CHECK(out.mode == BP_TRACKING);
    out=advance(&s,fresh(0,0),6001); CHECK(out.mode == BP_HOLD);
    CHECK(out.target_percent == 50);

    /* Future, reversed, negative and nonfinite values do not look like dark. */
    bp_init(&s,100,50);
    out=advance(&s,fresh(0,101),100); CHECK(out.mode == BP_WAITING);
    out=advance(&s,fresh(-1,100),100); CHECK(out.mode == BP_WAITING);
    out=advance(&s,fresh(NAN,100),100); CHECK(out.mode == BP_WAITING);
    out=advance(&s,fresh(INFINITY,100),100); CHECK(out.mode == BP_WAITING);
    out=advance(&s,fresh(200,100),100); CHECK(out.mode == BP_TRACKING);
    out=advance(&s,fresh(0,99),101); CHECK(out.mode == BP_HOLD);

    /* Clock rollback refuses the update, including output writes. */
    bp_state_t before=s;
    bp_output_t unchanged=out;
    CHECK(!bp_step(&s,fresh(0,0),0,&out));
    CHECK(memcmp(&s,&before,sizeof s)==0 && memcmp(&out,&unchanged,sizeof out)==0);
    /* No uint64 rollover/subtraction hazard near the end of the domain. */
    bp_init(&s,UINT64_MAX-100,20);
    out=advance(&s,fresh(0,UINT64_MAX-100),UINT64_MAX-100);
    out=advance(&s,fresh(0,UINT64_MAX-1),UINT64_MAX);
    CHECK(!out.low_light);

    /* An unobserved >6s sample gap breaks continuity, no catch-up jump. */
    bp_init(&s,0,50);
    (void)advance(&s,fresh(0,0),0);
    (void)advance(&s,fresh(0,2000),2000);
    (void)advance(&s,fresh(0,4000),4000);
    out=advance(&s,fresh(0,12000),12000);
    CHECK(out.target_percent==50 && !out.low_light);
    out=advance(&s,fresh(0,14000),14000); CHECK(out.target_percent==50);

    /* Exact dwell boundaries use acquisition time, not polling time. */
    bp_init(&s,0,50);
    (void)advance(&s,fresh(3,0),0);
    (void)advance(&s,fresh(3,2000),2000);
    (void)advance(&s,fresh(3,4000),4000);
    out=advance(&s,fresh(3,5999),5999); CHECK(out.demand_millipercent==50000);
    out=advance(&s,fresh(3,6000),6000); CHECK(out.demand_millipercent==4000);
    out=advance(&s,fresh(3,7999),7999); CHECK(!out.low_light);
    out=advance(&s,fresh(3,8000),8000); CHECK(out.low_light && out.low_light_known);
    (void)advance(&s,fresh(8,10000),10000);
    out=advance(&s,fresh(8,13999),13999); CHECK(out.low_light);
    out=advance(&s,fresh(8,14000),14000); CHECK(!out.low_light && out.low_light_known);

    /* Error interruption resets both dwell timers; recovery needs new samples. */
    bp_init(&s,0,50);
    (void)advance(&s,fresh(0,0),0);
    (void)advance(&s,fresh(0,4000),4000);
    out=advance(&s,(bp_input_t){0},5000); CHECK(out.mode==BP_HOLD);
    (void)advance(&s,fresh(0,6000),6000);
    out=advance(&s,fresh(0,10000),10000); CHECK(!out.low_light && out.target_percent==50);
    out=advance(&s,fresh(0,12000),12000); CHECK(out.demand_millipercent==2000);
    out=advance(&s,fresh(0,14000),14000); CHECK(out.low_light);

    /* Startup timeout exact boundary, no upward fallback at an already low output. */
    bp_init(&s,100,50);
    out=advance(&s,(bp_input_t){0},30099); CHECK(out.mode==BP_WAITING);
    out=advance(&s,(bp_input_t){0},30100); CHECK(out.mode==BP_FALLBACK);
    CHECK(out.demand_millipercent==BP_FALLBACK_MP && !out.low_light_known);
    bp_init(&s,0,2);
    out=advance(&s,(bp_input_t){0},1000000);
    CHECK(out.mode==BP_FALLBACK && out.target_percent==2);

    /* Error freezes the current ramp position, not the old accepted demand. */
    bp_init(&s,0,20);
    (void)advance(&s,fresh(1000,0),0);
    out=advance(&s,fresh(1000,2000),2000);
    CHECK(out.demand_millipercent==80000 && out.target_millipercent==22000);
    out=advance(&s,(bp_input_t){0},2100);
    CHECK(out.target_millipercent==22000 && out.demand_millipercent==22000);

    /* Small steady noise stays in the brightness deadband indefinitely. */
    bp_init(&s,0,65);
    for(uint64_t t=0;t<=60000;t+=2000) {
        out=advance(&s,fresh(t%4000 ? 498.0f : 502.0f,t),t);
        CHECK(out.target_percent==65 && out.demand_millipercent==65000);
    }
    /* Long failure while dark never brightens or fabricates a normal scene.
     * A recovery in the hysteresis band stays unknown until a qualified exit. */
    bp_init(&s,0,2);
    for(uint64_t t=0;t<=8000;t+=2000) out=advance(&s,fresh(0,t),t);
    CHECK(out.low_light && out.low_light_known);
    out=advance(&s,(bp_input_t){0},40000);
    CHECK(out.mode==BP_FALLBACK && out.target_percent==2 && out.low_light && !out.low_light_known);
    for(uint64_t t=42000;t<=52000;t+=2000) {
        out=advance(&s,fresh(5,t),t);
        CHECK(out.low_light && !out.low_light_known);
    }
    (void)advance(&s,fresh(8,54000),54000);
    (void)advance(&s,fresh(8,56000),56000);
    out=advance(&s,fresh(8,58000),58000);
    CHECK(!out.low_light && out.low_light_known);
}

static float scenario_lux(unsigned scene,uint64_t t) {
    switch(scene) {
    case 0: return t<20000 || t>=65000 ? 500 : 0;
    case 1: return t<20000 ? 0 : 1000;
    case 2:
        if(t<20000) return 20;
        if(t<100000) return ((t/2000)%2) ? 3.5f : 2.5f;
        if(t<120000) return 0;
        if(t<200000) return ((t/2000)%2) ? 7.5f : 8.5f;
        return 10;
    case 3: return (t>=20000 && t<24000) || (t>=50000 && t<54000) || t>=80000 ? 0 : 500;
    case 4: return t<70000 ? 200 : 0;
    default: return 200;
    }
}

static void scenarios(FILE *trace) {
    const char *names[]={"bright-dark-bright","dark-bright","threshold-noise",
                         "short-occlusion","fault-recovery","cold-start"};
    const unsigned seeds[]={65,2,14,65,40,50};
    const uint64_t ends[]={95000,45000,220000,120000,140000,80000};
    fprintf(trace,"scenario,now_ms,lux,valid,acquired_ms,target_mp,demand_mp,low,known,mode\n");
    for(unsigned scene=0;scene<6;++scene) {
        bp_state_t s;
        bp_init(&s,0,seeds[scene]);
        bp_input_t in={0};
        bp_output_t out={0};
        unsigned changes=0;
        bool previous_low=false;
        for(uint64_t t=0;t<=ends[scene];t+=100) {
            if(t%2000==0) {
                bool error=scene==4 && ((t>=10000 && t<20000) || (t>=100000 && t<110000));
                bool stale=scene==4 && t>=30000 && t<70000;
                bool missing=scene==5 && t<50000;
                if(error || missing) in.valid=false;
                else if(!stale) in=fresh(scenario_lux(scene,t),t);
            }
            out=advance(&s,in,t);
            if(out.low_light!=previous_low) {++changes;previous_low=out.low_light;}
            fprintf(trace,"%s,%"PRIu64",%.2f,%d,%"PRIu64",%d,%d,%d,%d,%d\n",
                names[scene],t,(double)in.lux,in.valid,in.acquired_ms,out.target_millipercent,
                out.demand_millipercent,out.low_light,out.low_light_known,out.mode);
            if(scene==0 && t==28000) CHECK(out.low_light && out.low_light_known);
            if(scene==0 && t==70000) CHECK(!out.low_light && out.low_light_known);
            if(scene==1 && t==8000) CHECK(out.low_light && out.low_light_known);
            if(scene==1 && t==24000) CHECK(!out.low_light && out.low_light_known);
            if(scene==2 && t<108000) CHECK(!out.low_light);
            if(scene==2 && t>=108000 && t<204000) CHECK(out.low_light);
            if(scene==3 && t<80000) CHECK(out.target_percent==65 && !out.low_light);
            if(scene==4 && t>=10000 && t<20000) CHECK(out.target_percent==40 && !out.low_light_known);
            if(scene==4 && t==58000) CHECK(out.mode==BP_FALLBACK);
            if(scene==4 && t>=100000 && t<110000) CHECK(out.low_light && out.target_percent==2);
            if(scene==5 && t<50000) CHECK(!out.low_light && !out.low_light_known);
            if(scene==5 && t==45000) CHECK(out.target_percent==20);
        }
        CHECK(out.low_light_known);
        CHECK(changes==(scene==0 || scene==1 || scene==2 ? 2u : scene==3 || scene==4 ? 1u : 0u));
        printf("PASS %s: target=%u%% low=%d transitions=%u\n",names[scene],out.target_percent,out.low_light,changes);
    }
}

static void generated(void) {
    uint32_t rng=1907;
    uint64_t t=0;
    bp_state_t s; bp_init(&s,t,50);
    bp_input_t in={0};
    for(unsigned i=0;i<200000;++i) {
        rng=rng*1664525u+1013904223u;
        t+=rng%53==0 ? 10000 : rng%7==0 ? 0 : 100;
        if(i%20==0) {
            in=fresh((float)(rng%20000)/10.0f,t);
            switch((rng>>16)%12) {
            case 0: in.valid=false; break;
            case 1: in.lux=NAN; break;
            case 2: in.lux=INFINITY; break;
            case 3: in.lux=-1; break;
            case 4: in.acquired_ms=t+1; break;
            case 5: in.acquired_ms=t>7000 ? t-7000 : 0; break;
            default: break;
            }
        }
        (void)advance(&s,in,t);
    }
    puts("PASS generated: 200000 bounded updates, malformed inputs and scheduler gaps");
}

int main(int argc,char **argv) {
    CHECK(argc==2);
    FILE *trace=fopen(argv[1],"w"); CHECK(trace!=NULL);
    boundaries(); scenarios(trace); generated();
    CHECK(fclose(trace)==0);
    printf("PASS checks=%lu steps=%lu state_bytes=%zu output_bytes=%zu input_bytes=%zu\n",
        checks,steps,sizeof(bp_state_t),sizeof(bp_output_t),sizeof(bp_input_t));
    return 0;
}
