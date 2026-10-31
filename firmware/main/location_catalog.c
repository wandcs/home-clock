#include "location_catalog.h"
#include <string.h>
#include <limits.h>

static bool scalar(const unsigned char **p,const unsigned char *end,uint32_t *cp) {
    if(*p>=end)return false;
    unsigned c=*(*p)++,n=0,min=0;*cp=c;
    if(c<128)return c!=0;
    if(c>=0xc2&&c<=0xdf){n=1;min=128;*cp=c&31;}
    else if(c>=0xe0&&c<=0xef){n=2;min=2048;*cp=c&15;}
    else if(c>=0xf0&&c<=0xf4){n=3;min=65536;*cp=c&7;}
    else return false;
    if((size_t)(end-*p)<n)return false;
    while(n--){c=*(*p)++;if((c&0xc0)!=0x80)return false;*cp=(*cp<<6)|(c&63);}
    return *cp>=min&&*cp<=0x10ffff&&!(*cp>=0xd800&&*cp<=0xdfff);
}
static bool put(uint32_t cp,char *out,size_t *used,unsigned *count) {
    if(++*count>64)return false;
    unsigned n=cp<128?1:cp<2048?2:cp<65536?3:4;
    if(*used+n>=LOCATION_QUERY_BYTES)return false;
    if(n==1)out[(*used)++]=(char)cp;
    else {
        out[(*used)++]=(char)((n==2?0xc0:n==3?0xe0:0xf0)|(cp>>(6*(n-1))));
        for(unsigned i=n-1;i;i--)out[(*used)++]=(char)(0x80|((cp>>(6*(i-1)))&63));
    }
    return true;
}
bool location_normalize(const char *raw,char out[LOCATION_QUERY_BYTES]) {
    out[0]=0;size_t size=strnlen(raw,LOCATION_QUERY_BYTES),used=0;unsigned count=0;
    if(size==LOCATION_QUERY_BYTES)return false;
    const unsigned char *p=(const unsigned char *)raw,*end=p+size;
    while(p<end) {
        uint32_t cp;if(!scalar(&p,end,&cp))return false;
        /* Match reference casefold().replace("u:","v") BEFORE NFKD. */
        if((cp=='u'||cp=='U')&&p<end&&*p==':'){cp='v';p++;}
        if(cp>=0xac00&&cp<=0xd7a3){
            uint32_t s=cp-0xac00;
            if(!put(0x1100+s/588,out,&used,&count)||!put(0x1161+(s%588)/28,out,&used,&count))return false;
            if(s%28&&!put(0x11a7+s%28,out,&used,&count))return false;
            continue;
        }
        unsigned lo=0,hi=location_norm_map_count;
        while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(location_norm_map[mid].cp<cp)lo=mid+1;else hi=mid;}
        if(lo<location_norm_map_count&&location_norm_map[lo].cp==cp){
            const unsigned char *s=location_norm_pool+location_norm_map[lo].offset,*e=s+strlen((const char*)s);
            while(s<e){uint32_t mapped;if(!scalar(&s,e,&mapped)||!put(mapped,out,&used,&count))return false;}
        }else{
            lo=0;hi=location_norm_range_count;
            while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(location_norm_ranges[mid].last<cp)lo=mid+1;else hi=mid;}
            if(lo<location_norm_range_count&&location_norm_ranges[lo].first<=cp&&!put(cp,out,&used,&count))return false;
        }
    }
    out[used]=0;return true;
}
static const char *meta_string(const location_catalog_t *c,uint32_t offset) {
    if(offset>=c->pool_bytes)return NULL;
    const char *s=(const char *)c->pool+offset;
    return memchr(s,0,c->pool_bytes-offset)?s:NULL;
}
bool location_catalog_open(location_catalog_t *c,const void *data,size_t bytes,const void *meta,size_t meta_bytes) {
    memset(c,0,sizeof *c);
    if(lix_open(&c->index,data,bytes)||meta_bytes<16)return false;
    const uint8_t *p=meta;uint32_t nr=lix_u32(p+4),row=lix_u32(p+8),ns=lix_u32(p+12);
    if(memcmp(p,"LM01",4)||nr!=c->index.nr||row!=24||16ULL+24ULL*nr+ns!=meta_bytes)return false;
    c->metadata=p+16;c->metadata_bytes=24ULL*nr;c->pool=p+16+c->metadata_bytes;c->pool_bytes=ns;
    /* Validate the complete metadata table once, before any worker query. */
    for(uint32_t i=0;i<nr;i++)for(unsigned k=1;k<6;k++)
        if(!meta_string(c,lix_u32(c->metadata+i*24+k*4)))return false;
    return true;
}
static bool copy(char *out,size_t bytes,const char *source) {
    if(!source||strlen(source)>=bytes)return false;
    memcpy(out,source,strlen(source)+1);return true;
}
bool location_catalog_record(const location_catalog_t *c,uint32_t i,location_record_t *out) {
    memset(out,0,sizeof *out);if(i>=c->index.nr)return false;
    const uint8_t *r=lix_record(&c->index,i),*meta=c->metadata+i*24;
    if(!(lix_u32(r+28)&1))return false;
    out->version=2;out->id=i+1;out->scope=lix_u32(meta);
    out->latitude=(int32_t)lix_u32(r+16)/1e5;out->longitude=(int32_t)lix_u32(r+20)/1e5;
    if(!copy(out->name,sizeof out->name,lix_string(&c->index,lix_u32(r+4)))||
       !copy(out->admin[0],sizeof out->admin[0],lix_string(&c->index,lix_u32(r+8)))||
       !copy(out->timezone,sizeof out->timezone,lix_string(&c->index,lix_u32(r+12)))||
       !copy(out->source_id,sizeof out->source_id,lix_string(&c->index,lix_u32(r)))||
       !copy(out->source_region,sizeof out->source_region,meta_string(c,lix_u32(meta+4)))||
       !copy(out->source_timezone,sizeof out->source_timezone,meta_string(c,lix_u32(meta+8)))||
       !copy(out->source,sizeof out->source,meta_string(c,lix_u32(meta+12)))||
       !copy(out->source_version,sizeof out->source_version,meta_string(c,lix_u32(meta+16)))||
       !copy(out->point_purpose,sizeof out->point_purpose,meta_string(c,lix_u32(meta+20)))||
       !copy(out->catalog_version,sizeof out->catalog_version,location_catalog_version))return false;
    if(out->scope==1||out->scope==2){strcpy(out->country,"中国");strcpy(out->country_code,"CN");}
    else {
        const char *end=strstr(out->admin[0]," · ");size_t n=end?(size_t)(end-out->admin[0]):strlen(out->admin[0]);
        if(n>=sizeof out->country)return false;
        memcpy(out->country,out->admin[0],n);out->country[n]=0;strcpy(out->country_code,out->source_region);
    }
    return location_valid(out);
}
bool location_catalog_query(const location_catalog_t *c,const char *raw,void *work,size_t bytes,location_results_t *out,lix_result *stats) {
    char normalized[LOCATION_QUERY_BYTES];lix_hit hits[LOCATION_RESULTS];
    memset(out,0,sizeof *out);
    if(!location_normalize(raw,normalized)||lix_query(&c->index,normalized,strlen(normalized),1,work,bytes,hits,LOCATION_RESULTS,stats))return false;
    for(unsigned i=0;i<stats->count;i++){
        if(location_catalog_record(c,hits[i].record,&out->items[out->count]))out->count++;
        else out->rejected++;
    }
    return !out->rejected;
}
