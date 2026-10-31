#include "comfort.h"
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, cases, random_steps;
static uint64_t clock_ms = UINT64_C(100000);
static const char *const states[] = {"pending", "comfortable", "hot", "cold", "dry", "wet"};
static const char *const reasons[] = {"none", "hot", "cold", "dry", "wet"};
static const char *const validities[] = {"ok", "unavailable", "nonfinite", "range", "future", "stale"};

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static comfort_sample_t sample(float value, bool valid, uint64_t acquired) {
    return (comfort_sample_t){value, valid, acquired};
}

static void invariants(comfort_result_t result) {
    CHECK(result.state >= COMFORT_PENDING && result.state <= COMFORT_WET);
    CHECK(result.reason_count <= 2);
    CHECK(result.temperature_validity >= COMFORT_VALUE_OK &&
          result.temperature_validity <= COMFORT_STALE);
    CHECK(result.humidity_validity >= COMFORT_VALUE_OK &&
          result.humidity_validity <= COMFORT_STALE);
    if (result.temperature_validity != COMFORT_VALUE_OK ||
        result.humidity_validity != COMFORT_VALUE_OK)
        CHECK(result.state == COMFORT_PENDING);
    else CHECK(result.state != COMFORT_PENDING);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(result.reasons[i] >= COMFORT_REASON_NONE &&
              result.reasons[i] <= COMFORT_REASON_WET);
        if (i >= result.reason_count) CHECK(result.reasons[i] == COMFORT_REASON_NONE);
        else {
            CHECK(result.reasons[i] != COMFORT_REASON_NONE);
            if (result.reasons[i] == COMFORT_REASON_HOT ||
                result.reasons[i] == COMFORT_REASON_COLD)
                CHECK(result.temperature_validity == COMFORT_VALUE_OK);
            else CHECK(result.humidity_validity == COMFORT_VALUE_OK);
        }
    }
    if (result.reason_count == 2) {
        CHECK(result.reasons[0] == COMFORT_REASON_HOT ||
              result.reasons[0] == COMFORT_REASON_COLD);
        CHECK(result.reasons[1] == COMFORT_REASON_DRY ||
              result.reasons[1] == COMFORT_REASON_WET);
    }
    if (result.state == COMFORT_OK) CHECK(result.reason_count == 0);
}

static void json_float(float value) {
    if (isfinite(value)) printf("%.9g", (double)value);
    else fputs("null", stdout);
}

static void expect(const char *name, comfort_context_t *context,
                   comfort_sample_t t, comfort_sample_t h, uint64_t now,
                   comfort_state_t state, comfort_reason_t first,
                   comfort_reason_t second, comfort_validity_t tv,
                   comfort_validity_t hv) {
    comfort_result_t result = comfort_step(context, t, h, now);
    invariants(result);
    if (result.state != state) fprintf(stderr, "case=%s expected=%s actual=%s\n",
        name, states[state], states[result.state]);
    CHECK(result.state == state);
    CHECK(result.reasons[0] == first);
    CHECK(result.reasons[1] == second);
    CHECK(result.reason_count == (unsigned)(first != COMFORT_REASON_NONE) +
                                  (unsigned)(second != COMFORT_REASON_NONE));
    CHECK(result.temperature_validity == tv);
    CHECK(result.humidity_validity == hv);
    ++cases;
    printf("{\"case\":\"%s\",\"now_ms\":%" PRIu64 ",\"t\":", name, now);
    json_float(t.value);
    fputs(",\"rh\":", stdout); json_float(h.value);
    printf(",\"source_valid\":[%s,%s],\"acquired_ms\":[%" PRIu64 ",%" PRIu64
           "],\"state\":\"%s\",\"reasons\":[\"%s\",\"%s\"],"
           "\"validity\":[\"%s\",\"%s\"],\"pass\":true}\n",
           t.source_valid ? "true" : "false", h.source_valid ? "true" : "false",
           t.acquired_ms, h.acquired_ms, states[result.state],
           reasons[result.reasons[0]], reasons[result.reasons[1]],
           validities[tv], validities[hv]);
}

static void fresh(const char *name, comfort_context_t *context, float t, float rh,
                  comfort_state_t state, comfort_reason_t first, comfort_reason_t second) {
    clock_ms += 2000;
    expect(name, context, sample(t, true, clock_ms), sample(rh, true, clock_ms),
           clock_ms, state, first, second, COMFORT_VALUE_OK, COMFORT_VALUE_OK);
}

static void combinations(void) {
    /* Nine explicit product scenarios; expectations are independent fixtures. */
    const struct {float t, rh; comfort_state_t state; comfort_reason_t a, b;} rows[] = {
        {23,45,COMFORT_OK,COMFORT_REASON_NONE,COMFORT_REASON_NONE},
        {28,45,COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_NONE},
        {18,45,COMFORT_COLD,COMFORT_REASON_COLD,COMFORT_REASON_NONE},
        {23,25,COMFORT_DRY,COMFORT_REASON_DRY,COMFORT_REASON_NONE},
        {23,75,COMFORT_WET,COMFORT_REASON_WET,COMFORT_REASON_NONE},
        {28,25,COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_DRY},
        {28,75,COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_WET},
        {18,25,COMFORT_COLD,COMFORT_REASON_COLD,COMFORT_REASON_DRY},
        {18,75,COMFORT_COLD,COMFORT_REASON_COLD,COMFORT_REASON_WET}
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        comfort_context_t c = {0};
        fresh("combination", &c, rows[i].t, rows[i].rh, rows[i].state, rows[i].a, rows[i].b);
    }
}

static void boundaries(void) {
    const float t[] = {nextafterf(20,-INFINITY),20,nextafterf(20,INFINITY),
                       nextafterf(26,-INFINITY),26,nextafterf(26,INFINITY)};
    const comfort_state_t ts[] = {COMFORT_COLD,COMFORT_OK,COMFORT_OK,
                                  COMFORT_OK,COMFORT_OK,COMFORT_HOT};
    const comfort_reason_t tr[] = {COMFORT_REASON_COLD,0,0,0,0,COMFORT_REASON_HOT};
    const float h[] = {nextafterf(30,-INFINITY),30,nextafterf(30,INFINITY),
                       nextafterf(60,-INFINITY),60,nextafterf(60,INFINITY)};
    const comfort_state_t hs[] = {COMFORT_DRY,COMFORT_OK,COMFORT_OK,
                                  COMFORT_OK,COMFORT_OK,COMFORT_WET};
    const comfort_reason_t hr[] = {COMFORT_REASON_DRY,0,0,0,0,COMFORT_REASON_WET};
    for (unsigned i = 0; i < 6; ++i) {
        comfort_context_t c = {0};
        fresh("temperature-entry-ulp", &c, t[i], 45, ts[i], tr[i], 0);
        comfort_reset(&c);
        fresh("humidity-entry-ulp", &c, 23, h[i], hs[i], hr[i], 0);
    }
    comfort_context_t c = {0};
    fresh("lower-corner-equality", &c, 20,30,COMFORT_OK,0,0);
    comfort_reset(&c);
    fresh("upper-corner-equality", &c, 26,60,COMFORT_OK,0,0);
    comfort_reset(&c);
    fresh("sensor-min-equality", &c, -40,0,COMFORT_COLD,COMFORT_REASON_COLD,COMFORT_REASON_DRY);
    fresh("sensor-max-equality", &c, 125,100,COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_WET);
}

static void hysteresis(void) {
    comfort_context_t c = {0};
    fresh("hot-start", &c,28,45,COMFORT_HOT,COMFORT_REASON_HOT,0);
    fresh("hot-at-entry", &c,26,45,COMFORT_HOT,COMFORT_REASON_HOT,0);
    fresh("hot-before-release", &c,nextafterf(25.5f,INFINITY),45,COMFORT_HOT,COMFORT_REASON_HOT,0);
    fresh("hot-release-equal", &c,25.5f,45,COMFORT_OK,0,0);
    fresh("hot-after-release", &c,nextafterf(25.5f,-INFINITY),45,COMFORT_OK,0,0);
    fresh("cold-start", &c,18,45,COMFORT_COLD,COMFORT_REASON_COLD,0);
    fresh("cold-at-entry", &c,20,45,COMFORT_COLD,COMFORT_REASON_COLD,0);
    fresh("cold-before-release", &c,nextafterf(20.5f,-INFINITY),45,COMFORT_COLD,COMFORT_REASON_COLD,0);
    fresh("cold-release-equal", &c,20.5f,45,COMFORT_OK,0,0);
    fresh("cold-after-release", &c,nextafterf(20.5f,INFINITY),45,COMFORT_OK,0,0);
    fresh("dry-start", &c,23,25,COMFORT_DRY,COMFORT_REASON_DRY,0);
    fresh("dry-at-entry", &c,23,30,COMFORT_DRY,COMFORT_REASON_DRY,0);
    fresh("dry-before-release", &c,23,nextafterf(33,-INFINITY),COMFORT_DRY,COMFORT_REASON_DRY,0);
    fresh("dry-release-equal", &c,23,33,COMFORT_OK,0,0);
    fresh("dry-after-release", &c,23,nextafterf(33,INFINITY),COMFORT_OK,0,0);
    fresh("wet-start", &c,23,75,COMFORT_WET,COMFORT_REASON_WET,0);
    fresh("wet-at-entry", &c,23,60,COMFORT_WET,COMFORT_REASON_WET,0);
    fresh("wet-before-release", &c,23,nextafterf(57,INFINITY),COMFORT_WET,COMFORT_REASON_WET,0);
    fresh("wet-release-equal", &c,23,57,COMFORT_OK,0,0);
    fresh("wet-after-release", &c,23,nextafterf(57,-INFINITY),COMFORT_OK,0,0);
    fresh("both-cold-dry", &c,18,25,COMFORT_COLD,COMFORT_REASON_COLD,COMFORT_REASON_DRY);
    fresh("jump-both-sides", &c,28,75,COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_WET);
    fresh("jump-back-both-sides", &c,18,25,COMFORT_COLD,COMFORT_REASON_COLD,COMFORT_REASON_DRY);
    fresh("both-clear", &c,23,45,COMFORT_OK,0,0);
    fresh("humidity-reason-continues", &c,28,25,COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_DRY);
    fresh("temperature-clear-humidity-primary", &c,23,31,COMFORT_DRY,COMFORT_REASON_DRY,0);
}

static void jitter(void) {
    const comfort_state_t expected[] = {COMFORT_HOT,COMFORT_COLD,COMFORT_DRY,COMFORT_WET};
    for (unsigned axis = 0; axis < 4; ++axis) {
        comfort_context_t c = {0};
        fresh("jitter-baseline", &c,23,45,COMFORT_OK,0,0);
        comfort_state_t previous = COMFORT_OK;
        unsigned transitions = 0;
        for (unsigned i = 0; i < 120; ++i) {
            const float t = axis == 0 ? (i%2 ? 25.9f : 26.1f) :
                            axis == 1 ? (i%2 ? 20.1f : 19.9f) : 23;
            const float h = axis == 2 ? (i%2 ? 30.5f : 29.5f) :
                            axis == 3 ? (i%2 ? 59.5f : 60.5f) : 45;
            clock_ms += 2000;
            comfort_result_t r = comfort_step(&c,sample(t,true,clock_ms),
                                               sample(h,true,clock_ms),clock_ms);
            invariants(r);
            CHECK(r.state == expected[axis]);
            if (previous != r.state) ++transitions;
            previous = r.state;
        }
        CHECK(transitions == 1);
        printf("{\"jitter_axis\":%u,\"samples\":120,\"duration_ms\":238000,"
               "\"transitions_from_comfortable\":%u,\"pass\":true}\n",axis,transitions);
        fresh("jitter-final-release", &c,23,45,COMFORT_OK,0,0);
    }
}

static void invalid_and_recovery(void) {
    const struct {float value; bool valid; int64_t age; comfort_validity_t status;} bad_t[] = {
        {28,false,0,COMFORT_UNAVAILABLE},
        {NAN,true,0,COMFORT_NONFINITE}, {INFINITY,true,0,COMFORT_NONFINITE},
        {-INFINITY,true,0,COMFORT_NONFINITE}, {FLT_MAX,true,0,COMFORT_OUT_OF_RANGE},
        {nextafterf(-40,-INFINITY),true,0,COMFORT_OUT_OF_RANGE},
        {nextafterf(125,INFINITY),true,0,COMFORT_OUT_OF_RANGE},
        {23,true,-1,COMFORT_FUTURE}, {23,true,6001,COMFORT_STALE}
    };
    const struct {float value; bool valid; int64_t age; comfort_validity_t status;} bad_h[] = {
        {75,false,0,COMFORT_UNAVAILABLE},
        {NAN,true,0,COMFORT_NONFINITE}, {INFINITY,true,0,COMFORT_NONFINITE},
        {-INFINITY,true,0,COMFORT_NONFINITE}, {-FLT_MAX,true,0,COMFORT_OUT_OF_RANGE},
        {nextafterf(0,-INFINITY),true,0,COMFORT_OUT_OF_RANGE},
        {nextafterf(100,INFINITY),true,0,COMFORT_OUT_OF_RANGE},
        {45,true,-1,COMFORT_FUTURE}, {45,true,6001,COMFORT_STALE}
    };
    for (unsigned i = 0; i < 9; ++i) {
        comfort_context_t c = {0};
        fresh("before-temperature-fault", &c,28,75,COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_WET);
        clock_ms += 2000;
        expect("temperature-fault-keep-wet", &c,
            sample(bad_t[i].value,bad_t[i].valid,(uint64_t)((int64_t)clock_ms-bad_t[i].age)),
            sample(59,true,clock_ms),clock_ms,COMFORT_PENDING,COMFORT_REASON_WET,0,
            bad_t[i].status,COMFORT_VALUE_OK);
        fresh("temperature-recovery-resets-only-t", &c,26,59,COMFORT_WET,COMFORT_REASON_WET,0);
        fresh("before-humidity-fault", &c,28,25,COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_DRY);
        clock_ms += 2000;
        expect("humidity-fault-keep-hot", &c,sample(25.9f,true,clock_ms),
            sample(bad_h[i].value,bad_h[i].valid,(uint64_t)((int64_t)clock_ms-bad_h[i].age)),
            clock_ms,COMFORT_PENDING,COMFORT_REASON_HOT,0,COMFORT_VALUE_OK,bad_h[i].status);
        fresh("humidity-recovery-resets-only-rh", &c,25.9f,30,COMFORT_HOT,COMFORT_REASON_HOT,0);
    }
    comfort_context_t c = {0};
    clock_ms += 2000;
    expect("both-unavailable", &c,sample(0,false,0),sample(0,false,0),clock_ms,
           COMFORT_PENDING,0,0,COMFORT_UNAVAILABLE,COMFORT_UNAVAILABLE);
    expect("valid-normal-not-overall-comfort", &c,sample(23,true,clock_ms),
           sample(0,false,0),clock_ms,COMFORT_PENDING,0,0,COMFORT_VALUE_OK,COMFORT_UNAVAILABLE);
    expect("valid-rh-not-overall-comfort", &c,sample(0,false,0),
           sample(45,true,clock_ms),clock_ms,COMFORT_PENDING,0,0,COMFORT_UNAVAILABLE,COMFORT_VALUE_OK);
    fresh("both-recover", &c,23,45,COMFORT_OK,0,0);
    fresh("shared-sht-before-failure", &c,28,75,COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_WET);
    clock_ms += 2000;
    expect("shared-sht-error", &c,sample(28,false,clock_ms-2000),
        sample(75,false,clock_ms-2000),clock_ms,COMFORT_PENDING,0,0,
        COMFORT_UNAVAILABLE,COMFORT_UNAVAILABLE);
    fresh("shared-sht-recovery-both-clear", &c,26,60,COMFORT_OK,0,0);
}

static void time_edges(void) {
    comfort_context_t c = {0};
    expect("sample-age-equals-limit",&c,sample(23,true,0),sample(45,true,0),6000,
        COMFORT_OK,0,0,COMFORT_VALUE_OK,COMFORT_VALUE_OK);
    expect("sample-age-past-limit",&c,sample(23,true,0),sample(45,true,0),6001,
        COMFORT_PENDING,0,0,COMFORT_STALE,COMFORT_STALE);
    comfort_reset(&c);
    expect("gap-start",&c,sample(28,true,10000),sample(45,true,10000),10000,
        COMFORT_HOT,COMFORT_REASON_HOT,0,0,0);
    expect("gap-exact-limit-holds",&c,sample(26,true,16000),sample(45,true,16000),16000,
        COMFORT_HOT,COMFORT_REASON_HOT,0,0,0);
    expect("gap-past-limit-resets",&c,sample(26,true,22001),sample(45,true,22001),22001,
        COMFORT_OK,0,0,0,0);
    expect("before-clock-reset",&c,sample(28,true,24001),sample(75,true,24001),24001,
        COMFORT_HOT,COMFORT_REASON_HOT,COMFORT_REASON_WET,0,0);
    expect("clock-reset-new-samples",&c,sample(26,true,0),sample(60,true,0),0,
        COMFORT_OK,0,0,0,0);
    expect("clock-reset-old-future-samples",&c,sample(28,true,24001),sample(75,true,24001),1,
        COMFORT_PENDING,0,0,COMFORT_FUTURE,COMFORT_FUTURE);
    expect("uint64-age-no-wrap",&c,sample(23,true,0),sample(45,true,0),UINT64_MAX,
        COMFORT_PENDING,0,0,COMFORT_STALE,COMFORT_STALE);
    expect("uint64-fresh-equality",&c,sample(23,true,UINT64_MAX-6000),
        sample(45,true,UINT64_MAX-6000),UINT64_MAX,COMFORT_OK,0,0,0,0);
    expect("null-context-fails-closed",NULL,sample(23,true,0),sample(45,true,0),0,
        COMFORT_PENDING,0,0,COMFORT_UNAVAILABLE,COMFORT_UNAVAILABLE);
    comfort_reset(NULL);
    comfort_context_t second = {0};
    fresh("context-one-hot",&c,28,45,COMFORT_HOT,COMFORT_REASON_HOT,0);
    fresh("context-two-independent",&second,26,45,COMFORT_OK,0,0);
    fresh("context-one-still-hot",&c,26,45,COMFORT_HOT,COMFORT_REASON_HOT,0);
    comfort_reset(&c);
    fresh("explicit-reset",&c,26,45,COMFORT_OK,0,0);
}

static uint32_t rng = UINT32_C(20261020);
static uint32_t next_random(void) {
    rng = rng * UINT32_C(1664525) + UINT32_C(1013904223);
    return rng;
}

static void generated_invariants(void) {
    comfort_context_t c = {0};
    for (unsigned i = 0; i < 20000; ++i) {
        clock_ms += next_random()%8001;
        const float t = (float)(next_random()%18001)/100 - 45;
        const float h = (float)(next_random()%12001)/100 - 10;
        const bool tv = next_random()%7 != 0, hv = next_random()%7 != 0;
        const uint64_t ta = next_random()%9001, ha = next_random()%9001;
        comfort_result_t r = comfort_step(&c,sample(t,tv,clock_ms-ta),
                                           sample(h,hv,clock_ms-ha),clock_ms);
        invariants(r);
        if (r.state == COMFORT_OK) {
            CHECK(t >= 20 && t <= 26);
            CHECK(h >= 30 && h <= 60);
        }
        if (r.temperature_validity == COMFORT_VALUE_OK &&
            r.humidity_validity == COMFORT_VALUE_OK) {
            if (t > 26) CHECK(r.state == COMFORT_HOT);
            if (t < 20) CHECK(r.state == COMFORT_COLD);
            if (t >= 20.5f && t <= 25.5f && h >= 33 && h <= 57)
                CHECK(r.state == COMFORT_OK);
        }
        ++random_steps;
    }
}

int main(void) {
    combinations(); boundaries(); hysteresis(); jitter();
    invalid_and_recovery(); time_edges(); generated_invariants();
    printf("{\"summary\":true,\"cases\":%u,\"jitter_samples\":480,"
           "\"random_steps\":%u,\"seed\":20261020,\"checks\":%u,"
           "\"context_bytes\":%zu,\"result_bytes\":%zu,\"pass\":true}\n",
           cases,random_steps,checks,sizeof(comfort_context_t),sizeof(comfort_result_t));
    return EXIT_SUCCESS;
}
