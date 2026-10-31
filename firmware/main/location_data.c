#include "location_data.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
/* Reject malformed UTF-8/control characters and oversized identities rather
 * than persisting truncated names. Missing optional metadata stays unknown. */
static bool utf8(const char *s,size_t capacity,bool required) {
    size_t n=strnlen(s,capacity); if(n==capacity || (required && !n)) return false;
    const unsigned char *p=(const unsigned char*)s,*end=p+n;
    while(p<end) {
        unsigned c=*p++,cp=c,min=0,k=0;
        if(c<128) { if(c<32 || c==127) return false; continue; }
        if(c>=0xc2 && c<=0xdf) {k=1;cp=c&31;min=128;}
        else if(c>=0xe0 && c<=0xef) {k=2;cp=c&15;min=2048;}
        else if(c>=0xf0 && c<=0xf4) {k=3;cp=c&7;min=65536;}
        else return false;
        while(k--) {if(p==end || (*p&0xc0)!=0x80) return false;cp=(cp<<6)|(*p++&63);}
        if(cp<min || cp>0x10ffff || (cp>=0xd800&&cp<=0xdfff)) return false;
    }
    return true;
}
bool location_valid(const location_record_t *r) {
    if((r->version!=1&&r->version!=2) || !r->id || !isfinite(r->latitude)||!isfinite(r->longitude)
       || fabs(r->latitude)>90 || fabs(r->longitude)>180 || !utf8(r->name,sizeof r->name,true)
       || !utf8(r->country,sizeof r->country,false)) return false;
    for(unsigned i=0;i<4;i++) if(!utf8(r->admin[i],sizeof r->admin[i],false)) return false;
    size_t n=strnlen(r->timezone,sizeof r->timezone);
    if(!n || n==sizeof r->timezone) return false;
    for(size_t i=0;i<n;i++) { char c=r->timezone[i]; if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='/'||c=='_'||c=='-'||c=='+')) return false; }
    if(r->version==2) {
        if(r->scope<1||r->scope>3||!utf8(r->source_id,sizeof r->source_id,true)||
           !utf8(r->source_timezone,sizeof r->source_timezone,false)||!utf8(r->source,sizeof r->source,true)||
           !utf8(r->source_version,sizeof r->source_version,true)||!utf8(r->catalog_version,sizeof r->catalog_version,true)||
           !utf8(r->point_purpose,sizeof r->point_purpose,true)||strnlen(r->source_region,sizeof r->source_region)!=2)return false;
        if(r->scope==1&&strcmp(r->timezone,"Asia/Shanghai"))return false;
    }
    return r->country_code[2]==0 && ((r->country_code[0]==0 && r->country_code[1]==0) ||
        (r->country_code[0]>='A'&&r->country_code[0]<='Z'&&r->country_code[1]>='A'&&r->country_code[1]<='Z'));
}
bool location_equal(const location_record_t *a,const location_record_t *b) {
    if(a->version!=b->version)return false;
    size_t size=a->version==1?offsetof(location_record_t,source_id):sizeof *a;
    return memcmp(a,b,size)==0;
}
bool location_query_valid(const char *q) {
    if(!utf8(q,LOCATION_QUERY_BYTES,true)) return false;
    unsigned count=0; for(const unsigned char *p=(const unsigned char*)q;*p;p++) if((*p&0xc0)!=0x80 && *p!=' ') count++;
    return count>=1&&count<=64;
}
static bool field(const cJSON *o,const char *key,char *dest,size_t cap,bool required) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    if(!v) return !required;
    if(!cJSON_IsString(v) || !v->valuestring || !utf8(v->valuestring,cap,required)) return false;
    memcpy(dest,v->valuestring,strlen(v->valuestring)+1); return true;
}
bool location_parse(const char *body,size_t bytes,location_results_t *out) {
    memset(out,0,sizeof *out);
    if(!bytes || bytes>LOCATION_BODY_LIMIT || memchr(body,0,bytes)) return false;
    /* Bound recursive cJSON before allocating: depth <=8, <=1600 structures. */
    unsigned depth=0,tokens=0; bool quoted=false,escape=false;
    for(size_t i=0;i<bytes;i++) {
        char c=body[i]; if(quoted) {if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quoted=false;continue;}
        if(c=='"')quoted=true;
        else if(c=='{'||c=='[') {if(++depth>8||++tokens>1600)return false;}
        else if(c=='}'||c==']') {if(!depth)return false;depth--;}
        else if(c==','||c==':') {if(++tokens>1600)return false;}
    }
    if(depth||quoted) return false;
    const char *end=NULL; cJSON *root=cJSON_ParseWithLengthOpts(body,bytes,&end,false);
    if(!root) return false;
    while(end<body+bytes && (*end==' '||*end=='\r'||*end=='\n'||*end=='\t'))end++;
    bool ok=cJSON_IsObject(root) && end==body+bytes && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"error"));
    cJSON *array=cJSON_GetObjectItemCaseSensitive(root,"results");
    if(array && !cJSON_IsArray(array))ok=false;
    if(ok && array) {
        cJSON *item; cJSON_ArrayForEach(item,array) {
            location_record_t r={.version=1};
            cJSON *id=cJSON_GetObjectItemCaseSensitive(item,"id"),*lat=cJSON_GetObjectItemCaseSensitive(item,"latitude"),*lon=cJSON_GetObjectItemCaseSensitive(item,"longitude");
            bool valid=cJSON_IsNumber(id)&&id->valuedouble>=1&&id->valuedouble<=UINT32_MAX&&floor(id->valuedouble)==id->valuedouble&&cJSON_IsNumber(lat)&&cJSON_IsNumber(lon);
            if(valid) {r.id=id->valuedouble;r.latitude=lat->valuedouble;r.longitude=lon->valuedouble;}
            valid=valid&&field(item,"name",r.name,sizeof r.name,true)&&field(item,"country",r.country,sizeof r.country,false)&&field(item,"country_code",r.country_code,sizeof r.country_code,false)&&field(item,"timezone",r.timezone,sizeof r.timezone,true);
            for(unsigned i=0;i<4&&valid;i++) {char key[]="admin1";key[5]+=(char)i;valid=field(item,key,r.admin[i],sizeof r.admin[i],false);}
            if(valid && location_valid(&r) && out->count<LOCATION_RESULTS) out->items[out->count++]=r;
            else out->rejected++;
        }
    }
    cJSON_Delete(root); return ok;
}
bool location_url(const char *q,char *out,size_t size) {
    if(!location_query_valid(q))return false;
    char encoded[LOCATION_QUERY_BYTES*3];size_t used=0; const char *hex="0123456789ABCDEF";
    for(const unsigned char *p=(const unsigned char*)q;*p;p++) {
        unsigned c=*p;
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')encoded[used++]=c;
        else {encoded[used++]='%';encoded[used++]=hex[c>>4];encoded[used++]=hex[c&15];}
    }
    encoded[used]=0;
    int n=snprintf(out,size,"https://geocoding-api.open-meteo.com/v1/search?name=%s&count=8&language=zh&format=json",encoded);
    return n>0 && (size_t)n<size;
}
static bool context_token(const location_record_t *r,const char *s,size_t n,char *out,size_t size) {
    static const char separator[]=" · ";
    if(!n||(strlen(r->name)==n&&!memcmp(r->name,s,n)))return true;
    /* Compare full emitted tokens before appending, never truncated display text. */
    for(const char *p=out;*p;){const char *end=strstr(p,separator);size_t bytes=end?(size_t)(end-p):strlen(p);
        if(bytes==n&&!memcmp(p,s,n))return true;
        if(!end)break;
        p=end+sizeof separator-1;
    }
    size_t used=strlen(out),gap=used?sizeof separator-1:0;
    if(n>=size||used+gap>=size-n)return false;
    if(gap)memcpy(out+used,separator,gap);
    memcpy(out+used+gap,s,n);out[used+gap+n]=0;return true;
}
bool location_display_context(const location_record_t *r,char *out,size_t size) {
    if(!out||!size)return false;
    out[0]=0;
    if(!r||!memchr(r->name,0,sizeof r->name)||!memchr(r->country,0,sizeof r->country))return false;
    for(unsigned i=0;i<4;i++)if(!memchr(r->admin[i],0,sizeof r->admin[i]))return false;
    if(r->version!=2&&!context_token(r,r->country,strlen(r->country),out,size))goto fail;
    for(unsigned i=0;i<4;i++){
        const char *p=r->admin[i];
        do{const char *end=r->version==2?strstr(p," · "):NULL;size_t n=end?(size_t)(end-p):strlen(p);
            if(!context_token(r,p,n,out,size))goto fail;
            if(!end)break;
            p=end+strlen(" · ");
        }while(*p);
    }
    if(!out[0]&&!context_token(r,r->country,strlen(r->country),out,size))goto fail;
    return true;
fail:
    out[0]=0;return false;
}
const char *location_weather_date_basis(const char *timezone) {
    if(!timezone||!timezone[0])return NULL;
    return !strcmp(timezone,"Asia/Shanghai")?"北京时间":"地点当地时间";
}
