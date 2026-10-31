#include "alarm_store.h"
#include <string.h>

static uint16_t get16(const uint8_t *p) {return (uint16_t)((unsigned)p[0]|((unsigned)p[1]<<8));}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put16(uint8_t *p,unsigned n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8u*i));}
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i) {
        c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^((c&1u)?UINT32_C(0xedb88320):0u);
    }
    return ~c;
}
size_t as_encode(const at_image *image,uint8_t *out,size_t cap) {
    if(!out||!at_image_valid(image))return 0;
    size_t n=20u+20u*image->config.count;
    if(n>cap)return 0;
    memset(out,0,n);memcpy(out,"ALM1",4);put16(out+4,2);put16(out+6,image->config.count);put16(out+8,(unsigned)n);put32(out+12,image->config.next_id);
    for(unsigned i=0;i<image->config.count;++i) {
        uint8_t *p=out+16u+20u*i;
        const am_entry *e=&image->config.entries[i];const at_record *r=&image->records[i];
        put32(p,e->id);p[4]=e->hour;p[5]=e->minute;p[6]=e->mode;p[7]=e->enabled;
        put32(p+8,r->revision);put32(p+12,r->once_revision);put16(p+16,r->day1);
    }
    put32(out+n-4u,crc(out,n-4u));return n;
}
as_decode_status as_decode(const uint8_t *p,size_t n,at_image *out) {
    if(!p||!out||n<6u||memcmp(p,"ALM1",4))return AS_DECODE_BAD;
    unsigned version=get16(p+4);
    if(version==1u) {
        am_config config;am_decode_status status=am_decode(p,n,&config);
        if(status==AM_DECODE_FOREIGN)return AS_DECODE_FOREIGN;
        if(status!=AM_DECODE_OK)return AS_DECODE_BAD;
        at_image image;if(!at_image_first(&config,&image))return AS_DECODE_BAD;
        *out=image;return AS_DECODE_LEGACY;
    }
    if(version!=2u)return AS_DECODE_FOREIGN;
    if(n>AS_WIRE_MAX)return AS_DECODE_FOREIGN;
    if(n<20u)return AS_DECODE_BAD;
    unsigned count=get16(p+6);
    if(count>AM_CAPACITY)return AS_DECODE_FOREIGN;
    if(n!=20u+20u*count||get16(p+8)!=n||get16(p+10)||get32(p+n-4u)!=crc(p,n-4u))return AS_DECODE_BAD;
    at_image image={0};image.config.count=(uint8_t)count;image.config.next_id=get32(p+12);
    for(unsigned i=0;i<count;++i) {
        const uint8_t *r=p+16u+20u*i;
        if(get16(r+18))return AS_DECODE_BAD;
        image.config.entries[i]=(am_entry){get32(r),r[4],r[5],r[6],r[7]};
        image.records[i]=(at_record){get32(r+8),get32(r+12),get16(r+16)};
    }
    if(!at_image_valid(&image))return AS_DECODE_BAD;
    *out=image;return AS_DECODE_CURRENT;
}
