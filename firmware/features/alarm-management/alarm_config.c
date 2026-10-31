#include "alarm_config.h"
#include <string.h>
_Static_assert(AM_CAPACITY <= UINT8_MAX, "count field");
static uint16_t u16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] | (unsigned)p[1]<<8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void put16(uint8_t *p, unsigned v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) { for(unsigned i=0;i<4u;i++)p[i]=(uint8_t)(v>>(8u*i)); }
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;i++){c^=p[i];for(unsigned j=0;j<8u;j++)c=(c>>1)^((c&1u)?UINT32_C(0xedb88320):0u);}
    return ~c;
}
void am_config_empty(am_config *c) { memset(c,0,sizeof *c);c->next_id=1; }
int am_find(const am_config *c,uint32_t id) {
    if(!c || c->count>AM_CAPACITY)return -1;
    for(unsigned i=0;i<c->count;i++)if(c->entries[i].id==id)return (int)i;
    return -1;
}
bool am_config_valid(const am_config *c) {
    if(!c || !c->next_id || c->count>AM_CAPACITY)return false;
    for(unsigned i=0;i<c->count;i++) {
        const am_entry *e=&c->entries[i];
        if(!e->id || e->id>=c->next_id || e->hour>23u || e->minute>59u || e->mode>AF_DAILY || e->enabled>1u)return false;
        for(unsigned j=0;j<i;j++)if(c->entries[j].id==e->id)return false;
    }
    return true;
}
bool am_config_equal(const am_config *a,const am_config *b) {
    if(!am_config_valid(a)||!am_config_valid(b)||a->count!=b->count||a->next_id!=b->next_id)return false;
    for(unsigned i=0;i<a->count;i++) {
        const am_entry *x=&a->entries[i],*y=&b->entries[i];
        if(x->id!=y->id||x->hour!=y->hour||x->minute!=y->minute||x->mode!=y->mode||x->enabled!=y->enabled)return false;
    }
    return true;
}
size_t am_encode(const am_config *c,uint8_t *p,size_t capacity) {
    if(!p||!am_config_valid(c))return 0;
    size_t n=20u+8u*c->count;if(capacity<n)return 0;
    memset(p,0,n);memcpy(p,"ALM1",4);put16(p+4,1);put16(p+6,c->count);put16(p+8,(unsigned)n);put32(p+12,c->next_id);
    for(unsigned i=0;i<c->count;i++){
        const am_entry *e=&c->entries[i];uint8_t *r=p+16u+8u*i;
        put32(r,e->id);r[4]=e->hour;r[5]=e->minute;r[6]=e->mode;r[7]=e->enabled;
    }
    put32(p+n-4u,crc(p,n-4u));return n;
}
am_decode_status am_decode(const uint8_t *p,size_t n,am_config *out) {
    if(!p||!out||n<6u||memcmp(p,"ALM1",4))return AM_DECODE_BAD;
    if(u16(p+4)!=1u)return AM_DECODE_FOREIGN;
    if(n<20u)return AM_DECODE_BAD;
    unsigned count=u16(p+6);
    if(count>AM_CAPACITY || n>AM_WIRE_MAX)return AM_DECODE_FOREIGN;
    if(n!=20u+8u*count||u16(p+8)!=n||u16(p+10)||crc(p,n-4u)!=u32(p+n-4u))return AM_DECODE_BAD;
    am_config c;am_config_empty(&c);c.next_id=u32(p+12);c.count=(uint8_t)count;
    for(unsigned i=0;i<count;i++){
        const uint8_t *r=p+16u+8u*i;c.entries[i]=(am_entry){u32(r),r[4],r[5],r[6],r[7]};
    }
    if(!am_config_valid(&c))return AM_DECODE_BAD;
    *out=c;return AM_DECODE_OK;
}
