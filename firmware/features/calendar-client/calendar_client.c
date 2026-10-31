#include "calendar_client.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
typedef struct {
    const uint8_t *p, *end;
}
reader;
static void ws(reader *r) {
    while (r->p<r->end && (*r->p==' '||*r->p=='\n'||*r->p=='\r'||*r->p=='\t')) ++r->p;
}
static bool ch(reader *r, uint8_t c) {
    ws(r);
    if(r->p==r->end||*r->p!=c)return false;
    ++r->p;
    return true;
}
static int hex(uint8_t c) {
    if(c>='0'&&c<='9')return (int)c-'0';
    if(c>='a'&&c<='f')return (int)c-'a'+10;
    if(c>='A'&&c<='F')return (int)c-'A'+10;
    return -1;
}
static bool u16(reader *r, uint32_t *v) {
    if((size_t)(r->end-r->p)<4)return false;
    uint32_t x=0;
    for(unsigned i=0;i<4;++i) {
        int h=hex(*r->p++);
        if(h<0)return false;
        x=x*16u+(unsigned)h;
    }
    *v=x;
    return true;
}
static bool append(char *out,size_t cap,size_t *n,uint32_t x) {
    uint8_t b[4];
    size_t k=0;
    if(x<0x80) {
        b[0]=(uint8_t)x;
        k=1;
    }
    else if(x<0x800) {
        b[0]=(uint8_t)(0xc0u|(x>>6));
        b[1]=(uint8_t)(0x80u|(x&63u));
        k=2;
    }
    else if(x<0x10000) {
        b[0]=(uint8_t)(0xe0u|(x>>12));
        b[1]=(uint8_t)(0x80u|((x>>6)&63u));
        b[2]=(uint8_t)(0x80u|(x&63u));
        k=3;
    }
    else {
        b[0]=(uint8_t)(0xf0u|(x>>18));
        b[1]=(uint8_t)(0x80u|((x>>12)&63u));
        b[2]=(uint8_t)(0x80u|((x>>6)&63u));
        b[3]=(uint8_t)(0x80u|(x&63u));
        k=4;
    }
    if(x==0||*n+k>=cap)return false;
    memcpy(out+*n,b,k);
    *n+=k;
    return true;
}
/* Decode escapes before key comparison; reject invalid UTF-8/surrogates/NUL. */
static bool string(reader *r,char *out,size_t cap) {
    if(!ch(r,'"'))return false;
    size_t n=0;
    while(r->p<r->end) {
        uint32_t x=*r->p++;
        if(x=='"') {
            out[n]=0;
            return true;
        }
        if(x<32)return false;
        if(x=='\\') {
            if(r->p==r->end)return false;
            x=*r->p++;
            if(x=='u') {
                if(!u16(r,&x))return false;
                if(x>=0xd800&&x<=0xdbff) {
                    uint32_t low;
                    if((size_t)(r->end-r->p)<2||*r->p++!='\\'||*r->p++!='u'||!u16(r,&low)||low<0xdc00||low>0xdfff)return false;
                    x=0x10000u+((x-0xd800u)<<10)+(low-0xdc00u);
                }
                else if(x>=0xdc00&&x<=0xdfff)return false;
            }
            else if(x=='b')x=8;
            else if(x=='f')x=12;
            else if(x=='n')x=10;
            else if(x=='r')x=13;
            else if(x=='t')x=9;
            else if(x!='"'&&x!='\\'&&x!='/')return false;
        }
        else if(x>=128) {
            unsigned k;
            uint32_t min;
            if(x>=0xc2&&x<=0xdf) {
                k=1;
                min=0x80;
                x&=31u;
            }
            else if(x>=0xe0&&x<=0xef) {
                k=2;
                min=0x800;
                x&=15u;
            }
            else if(x>=0xf0&&x<=0xf4) {
                k=3;
                min=0x10000;
                x&=7u;
            }
            else return false;
            for(unsigned i=0;i<k;++i) {
                if(r->p==r->end||(*r->p&0xc0u)!=0x80u)return false;
                x=(x<<6)|(*r->p++&63u);
            }
            if(x<min||x>0x10ffff||(x>=0xd800&&x<=0xdfff))return false;
        }
        if(!append(out,cap,&n,x))return false;
    }
    return false;
}
static bool number(reader *r,uint16_t *v) {
    ws(r);
    if(r->p==r->end||*r->p<'1'||*r->p>'9')return false;
    unsigned x=0,k=0;
    while(r->p<r->end&&*r->p>='0'&&*r->p<='9') {
        if(++k>4)return false;
        x=x*10u+(*r->p++-'0');
    }
    if(x<2000||x>2099)return false;
    *v=(uint16_t)x;
    return true;
}
static bool boolean(reader *r,bool *v) {
    ws(r);
    size_t n=(size_t)(r->end-r->p);
    if(n>=4&&!memcmp(r->p,"true",4)) {
        r->p+=4;
        *v=true;
        return true;
    }
    if(n>=5&&!memcmp(r->p,"false",5)) {
        r->p+=5;
        *v=false;
        return true;
    }
    return false;
}
static bool official(const char *s) {
    static const char prefix[]="https://www.gov.cn/";
    if(strncmp(s,prefix,sizeof prefix-1))return false;
    const char *p=s+sizeof prefix-1;
    if(!*p)return false;
    for(;*p;++p)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='/'||*p=='_'||*p=='-'||*p=='.'))return false;
    return strstr(s,"..") == NULL;
}
static uint8_t family(const char *s) {
    static const char *const names[]= {
        "元旦","春节","清明节","劳动节","端午节","中秋节","国庆节"
    };
    uint8_t bits=0;
    for(unsigned i=0;i<7;++i)if(strstr(s,names[i]))bits|=(uint8_t)(1u<<i);
    return bits;
}
static bool date(const char *s,af_time *t) {
    if(strlen(s)!=10||s[4]!='-'||s[7]!='-')return false;
    unsigned v[3]= {
        0
    };
    unsigned g=0;
    for(unsigned i=0;i<10;++i) {
        if(i==4||i==7) {
            ++g;
            continue;
        }
        if(s[i]<'0'||s[i]>'9')return false;
        v[g]=v[g]*10u+(unsigned)(s[i]-'0');
    }
    *t=(af_time) {
        .year=(uint16_t)v[0],.month=(uint8_t)v[1],.day=(uint8_t)v[2]
    };
    return af_time_valid(t);
}
static cc_result papers(reader *r,cc_candidate *c) {
    if(!ch(r,'['))return CC_JSON;
    ws(r);
    if(ch(r,']'))return CC_OK;
    do {
        if(c->paper_count==CC_PAPERS_LIMIT)return CC_SIZE;
        char *s=c->papers[c->paper_count];
        if(!string(r,s,CC_PAPER_BYTES)||!official(s))return CC_SCHEMA;
        for(unsigned i=0;i<c->paper_count;++i)if(!strcmp(s,c->papers[i]))return CC_DUPLICATE;
        ++c->paper_count;
        if(ch(r,']'))return CC_OK;
    }
    while(ch(r,','));
    return CC_JSON;
}
static cc_result row(reader *r,cc_candidate *c) {
    char key[24],name[64]= {
        0
    },day[16]= {
        0
    };
    bool off=false;
    unsigned fields=0;
    if(!ch(r,'{'))return CC_JSON;
    do {
        if(!string(r,key,sizeof key)||!ch(r,':'))return CC_JSON;
        unsigned bit;
        if(!strcmp(key,"name")) {
            bit=1;
            if(!string(r,name,sizeof name)||!name[0])return CC_SCHEMA;
        }
        else if(!strcmp(key,"date")) {
            bit=2;
            if(!string(r,day,sizeof day))return CC_SCHEMA;
        }
        else if(!strcmp(key,"isOffDay")) {
            bit=4;
            if(!boolean(r,&off))return CC_SCHEMA;
        }
        else return CC_SCHEMA;
        if(fields&bit)return CC_DUPLICATE;
        fields|=bit;
        if(ch(r,'}'))break;
        if(!ch(r,','))return CC_JSON;
    }
    while(true);
    if(fields!=7)return CC_SCHEMA;
    af_time d;
    if(!date(day,&d)||d.year>c->title_year||(unsigned)d.year+1u<c->title_year||(d.year<c->title_year&&d.month!=12))return CC_DATE;
    unsigned slot=d.year==c->title_year?1u:0u;
    uint16_t index;
    if(!af_day_index(&d,&index))return CC_DATE;
    uint8_t mask=(uint8_t)(1u<<(index%8u));
    unsigned b=index/8u;
    af_year *y=&c->years[slot];
    if((y->off[b]|y->work[b])&mask)return CC_DUPLICATE;
    if(off)y->off[b]|=mask;
    else y->work[b]|=mask;
    if(slot==1&&off)c->families|=family(name);
    return CC_OK;
}
static cc_result days(reader *r,cc_candidate *c) {
    if(!ch(r,'['))return CC_JSON;
    if(ch(r,']'))return CC_OK;
    do {
        if(c->day_count==CC_DAYS_LIMIT)return CC_SIZE;
        cc_result e=row(r,c);
        if(e!=CC_OK)return e;
        ++c->day_count;
        if(ch(r,']'))return CC_OK;
    }
    while(ch(r,','));
    return CC_JSON;
}
cc_result cc_parse(const uint8_t *raw,size_t n,uint16_t title,cc_candidate *out) {
    if(!raw||!out||title<2000||title>2099)return CC_ARGUMENT;
    if(!n||n>CC_BODY_LIMIT)return CC_SIZE;
    reader r= {
        raw,raw+n
    };
    cc_candidate c= {
        .title_year=title
    };
    c.years[0].year=(uint16_t)(title-1u);
    c.years[1].year=title;
    if(!ch(&r,'{'))return CC_JSON;
    unsigned fields=0;
    do {
        char key[24],ignored[CC_PAPER_BYTES];
        if(!string(&r,key,sizeof key)||!ch(&r,':'))return CC_JSON;
        unsigned bit;
        cc_result e=CC_OK;
        if(!strcmp(key,"year")) {
            bit=1;
            uint16_t y;
            if(!number(&r,&y)||y!=title)return CC_SCHEMA;
        }
        else if(!strcmp(key,"papers")) {
            bit=2;
            e=papers(&r,&c);
        }
        else if(!strcmp(key,"days")) {
            bit=4;
            e=days(&r,&c);
        }
        else if(!strcmp(key,"$schema")) {
            bit=8;
            if(!string(&r,ignored,sizeof ignored))return CC_SCHEMA;
        }
        else if(!strcmp(key,"$id")) {
            bit=16;
            if(!string(&r,ignored,sizeof ignored))return CC_SCHEMA;
        }
        else return CC_SCHEMA;
        if(e!=CC_OK)return e;
        if(fields&bit)return CC_DUPLICATE;
        fields|=bit;
        if(ch(&r,'}'))break;
        if(!ch(&r,','))return CC_JSON;
    }
    while(true);
    ws(&r);
    if(r.p!=r.end)return CC_JSON;
    if((fields&7u)!=7u)return CC_SCHEMA;
    if(!c.day_count&&!c.paper_count)return CC_EMPTY;
    if(!c.day_count||!c.paper_count)return CC_INCOMPLETE;
    *out=c;
    return CC_OK;
}
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i) {
        c^=p[i];
        for(unsigned j=0;j<8;++j)c=(c>>1)^((c&1u)?0xedb88320u:0u);
    }
    return c^UINT32_MAX;
}
static void put32(uint8_t *p,uint32_t x) {
    for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(i*8u));
}
static bool approved(cc_approval a) {
    return a==CC_INDEPENDENT_REVIEW||a==CC_TRUSTED_UPSTREAM;
}
static cc_result eligible(const cc_candidate *c) {
    if(!c||c->title_year<2000||c->title_year>2099||!c->paper_count||c->paper_count>CC_PAPERS_LIMIT||!c->day_count||c->day_count>CC_DAYS_LIMIT||c->years[1].year!=c->title_year||!af_year_valid(&c->years[1]))return CC_SCHEMA;
    return c->families==CC_ALL_FAMILIES?CC_OK:CC_COVERAGE;
}
cc_result cc_prepare(const cc_candidate *base,const cc_candidate *next,cc_result next_status,cc_approval approval,const uint8_t digest[32],uint8_t out[AF_YEAR_BYTES]) {
    if(!base||!digest||!out)return CC_ARGUMENT;
    if(!approved(approval))return CC_UNAPPROVED;
    cc_result gate=eligible(base);
    if(gate!=CC_OK)return gate;
    if(next_status!=CC_OK&&next_status!=CC_EMPTY&&next_status!=CC_NOT_FOUND)return next_status;
    if((next_status==CC_OK&&!next)||(next_status!=CC_OK&&next))return CC_ARGUMENT;
    af_year y=base->years[1];
    if(next) {
        gate=eligible(next);
        if(gate!=CC_OK)return gate;
        if(next->title_year!=(unsigned)base->title_year+1u||next->years[0].year!=y.year||!af_year_valid(&next->years[0]))return CC_SCHEMA;
        for(unsigned i=0;i<AF_BITMAP_BYTES;++i) {
            if((y.off[i]&next->years[0].work[i])||(y.work[i]&next->years[0].off[i]))return CC_CONFLICT;
            y.off[i]|=next->years[0].off[i];
            y.work[i]|=next->years[0].work[i];
        }
    }
    uint8_t b[AF_YEAR_BYTES]= {
        0
    };
    memcpy(b,"AFY1",4);
    b[4]=1;
    b[6]=(uint8_t)y.year;
    b[7]=(uint8_t)(y.year>>8);
    memcpy(b+8,y.off,46);
    memcpy(b+54,y.work,46);
    memcpy(b+100,digest,32);
    put32(b+132,crc(b,132));
    af_year checked;
    if(!af_year_decode(b,sizeof b,&checked))return CC_SCHEMA;
    memcpy(out,b,sizeof b);
    return CC_OK;
}
cc_result cc_cache_load(cc_cache *c,const cc_store *s,uint16_t y,cc_approval approval) {
    if(!c||!s||!s->get||y<2000||y>2099)return CC_ARGUMENT;
    if(!approved(approval))return CC_UNAPPROVED;
    uint8_t b[AF_YEAR_BYTES];
    size_t n=0;
    af_year v;
    if(!s->get(s->ctx,y,b,sizeof b,&n)||!af_year_decode(b,n,&v)||v.year!=y)return CC_STORAGE;
    c->valid=true;
    c->uncertain=false;
    c->year=v;
    memcpy(c->bytes,b,sizeof b);
    return CC_OK;
}
cc_result cc_cache_update(cc_cache *c,const cc_store *s,const uint8_t *b,size_t n,cc_approval approval) {
    if(!c||!s||!s->get||!s->put||!b)return CC_ARGUMENT;
    if(!approved(approval))return CC_UNAPPROVED;
    if(c->uncertain)return CC_UNCERTAIN;
    af_year y;
    if(!af_year_decode(b,n,&y))return CC_SCHEMA;
    if(c->valid&&c->year.year!=y.year)return CC_ARGUMENT;
    if(c->valid&&!memcmp(c->bytes,b,AF_YEAR_BYTES))return CC_OK;
    bool wrote=s->put(s->ctx,y.year,b,n);
    uint8_t readback[AF_YEAR_BYTES];
    size_t used=0;
    af_year v;
    if(s->get(s->ctx,y.year,readback,sizeof readback,&used)&&af_year_decode(readback,used,&v)&&v.year==y.year) {
        if(!memcmp(readback,b,AF_YEAR_BYTES)) {
            c->valid=true;
            c->uncertain=false;
            c->year=v;
            memcpy(c->bytes,b,AF_YEAR_BYTES);
            return CC_OK;
        }
        if(c->valid&&!memcmp(readback,c->bytes,AF_YEAR_BYTES))return CC_STORAGE;
    } (void)wrote;
    c->uncertain=true;
    return CC_UNCERTAIN;
}
static uint64_t after(uint64_t n,uint64_t d) {
    return n>UINT64_MAX-d?UINT64_MAX:n+d;
}
void cc_schedule_init(cc_schedule *s,uint64_t now) {
    if(s)*s=(cc_schedule) {
        .due_ms=now
    };
}
bool cc_schedule_take(cc_schedule *s,uint64_t now,bool online,bool clock) {
    if(!s||!online||!clock||s->in_flight||now<s->due_ms)return false;
    s->in_flight=true;
    return true;
}
void cc_schedule_finish(cc_schedule *s,uint64_t now,bool success) {
    if(!s||!s->in_flight)return;
    s->in_flight=false;
    static const uint64_t retry[]= {
        3600000,21600000,86400000,259200000
    };
    if(success) {
        s->failures=0;
        s->due_ms=after(now,604800000);
    }
    else {
        if(s->failures<4)++s->failures;
        s->due_ms=after(now,retry[s->failures-1u]);
    }
}
bool cc_url(uint16_t year,char *out,size_t cap) {
    if(!out||year<2000||year>2099)return false;
    char b[112];
    int n=snprintf(b,sizeof b,"https://raw.githubusercontent.com/NateScarlet/holiday-cn/master/%u.json",(unsigned)year);
    if(n<0||(size_t)n>=cap)return false;
    memcpy(out,b,(size_t)n+1u);
    return true;
}
