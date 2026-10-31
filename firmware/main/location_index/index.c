#include "index.h"
#include <limits.h>
#include <string.h>

uint32_t lix_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
           (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint32_t u16(const uint8_t *p) { return p[0] | (uint32_t)p[1]<<8; }
const char *lix_string(const lix_index *x, uint32_t off) {
    return (const char *)x->pool + off;
}
const uint8_t *lix_record(const lix_index *x, uint32_t i) { return x->records+36*i; }
const char *lix_key(const lix_index *x, uint32_t i) {
    return lix_string(x, lix_u32(x->keys+i*(x->compact?8:12)));
}
/* Strict UTF-8 scalar count; rejects overlong, surrogates, embedded NUL. */
static int scalars(const char *s, size_t n) {
    size_t i=0; int count=0;
    while (i<n) {
        uint32_t c=(uint8_t)s[i++], cp, min; unsigned tail;
        if (!c) return -1;
        if (c<128) { count++; continue; }
        if (c>=0xc2 && c<=0xdf) { cp=c&31; tail=1; min=128; }
        else if (c>=0xe0 && c<=0xef) { cp=c&15; tail=2; min=2048; }
        else if (c>=0xf0 && c<=0xf4) { cp=c&7; tail=3; min=65536; }
        else return -1;
        if (tail>n-i) return -1;
        while(tail--) {
            c=(uint8_t)s[i++]; if ((c&0xc0)!=0x80) return -1;
            cp=(cp<<6)|(c&63);
        }
        if (cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return -1;
        count++;
    }
    return count;
}
static int valid_string(const lix_index *x, uint32_t off) {
    if (off>=x->ns) return 0;
    const char *s=lix_string(x,off);
    const char *end=memchr(s,0,x->ns-off);
    return end && scalars(s,(size_t)(end-s))>=0;
}
static uint32_t post_start(const lix_index *x, uint32_t k) {
    return lix_u32(x->keys+k*(x->compact?8:12)+4);
}
static uint32_t post_end(const lix_index *x, uint32_t k) {
    uint32_t start=post_start(x,k);
    return x->compact ? post_start(x,k+1) : start+lix_u32(x->keys+k*12+8);
}
int lix_open(lix_index *x, const void *data, size_t length) {
    lix_index v={0}; const uint8_t *p=data;
    if (!x) return -1;
    memset(x,0,sizeof(*x));
    if (!p || length<24 || length>UINT32_MAX) return -1;
    v.compact=!memcmp(p,"LCO2",4);
    if (v.compact) {
        if(length<48 || lix_u32(p+4)!=1 || lix_u32(p+44)!=length) return -1;
        v.nr=lix_u32(p+8); v.nk=lix_u32(p+12);
        v.np=lix_u32(p+16); v.ns=lix_u32(p+20);
    } else {
        if(memcmp(p,"LCS1",4) || lix_u32(p+20)!=1) return -1;
        v.nr=lix_u32(p+4); v.nk=lix_u32(p+8);
        v.np=lix_u32(p+12); v.ns=lix_u32(p+16);
    }
    if (!v.nr || v.nr>65535 || !v.nk || !v.ns) return -1;
    uint64_t rec=v.compact?48:24, rank=rec+(uint64_t)v.nr*36;
    uint64_t key=rank+(v.compact?(uint64_t)v.nr*2:0);
    uint64_t post=key+((uint64_t)v.nk+(v.compact?1u:0u))*(v.compact?8:12);
    uint64_t dir=post+(uint64_t)v.np*(v.compact?3:8);
    uint64_t pool=dir+(v.compact?702*8:0);
    if (pool+v.ns!=length) return -1;
    if(v.compact && (lix_u32(p+24)!=key || lix_u32(p+28)!=post ||
       lix_u32(p+32)!=pool || lix_u32(p+36)!=rank || lix_u32(p+40)!=dir)) return -1;
    v.data=p; v.records=p+rec; v.keys=p+key; v.posts=p+post; v.pool=p+pool;
    v.ranks=v.compact?p+rank:NULL; v.dirs=v.compact?p+dir:NULL; v.accelerate=1;
    /* Rank permutation: a fixed 8KiB validation bitmap, never per-query. */
    uint8_t used[8192]={0};
    for(uint32_t i=0;i<v.nr;i++) {
        const uint8_t *r=lix_record(&v,i);
        for(unsigned j=0;j<4;j++) if(!valid_string(&v,lix_u32(r+4*j))) return -1;
        uint32_t flags=lix_u32(r+28);
        if ((flags&~511u) || ((flags>>4)&15)>6) return -1;
        if(v.compact) {
            uint32_t n=u16(v.ranks+2*i);
            if(n>=v.nr || (used[n/8]&(1u<<(n%8)))) return -1;
            used[n/8]|=(uint8_t)(1u<<(n%8));
        }
    }
    uint32_t previous=0;
    for(uint32_t i=0;i<v.nk;i++) {
        if(!valid_string(&v,lix_u32(v.keys+i*(v.compact?8:12)))) return -1;
        const char *s=lix_key(&v,i);
        int n=scalars(s,strlen(s));
        if(n<1 || n>64 || (i && strcmp(lix_key(&v,i-1),s)>=0)) return -1;
        uint32_t a=post_start(&v,i), b=post_end(&v,i);
        if(a!=previous || a>=b || b>v.np) return -1;
        previous=b;
        uint32_t last=0;
        for(uint32_t j=a;j<b;j++) {
            const uint8_t *r=v.posts+j*(v.compact?3:8);
            uint32_t ri=v.compact?u16(r):lix_u32(r);
            unsigned kind=r[v.compact?2:4]; uint32_t order=ri*4+kind;
            if(ri>=v.nr || kind>3 || (j>a && order<=last) || !(lix_u32(lix_record(&v,ri)+28)&1)) return -1;
            last=order;
        }
    }
    if(previous!=v.np) return -1;
    if(v.compact) {
        if(post_start(&v,v.nk)!=v.np || lix_u32(v.keys+v.nk*8)) return -1;
        /* Verify exact directory ranges, not just that offsets fit. */
        for(uint32_t d=0;d<702;d++) {
            char q[3]={(char)('a'+d/27),(char)('a'+d%27),0};
            if(d%27==26) q[1]=0;
            uint32_t lo=0,hi=v.nk;
            while(lo<hi) {uint32_t m=lo+(hi-lo)/2; if(strcmp(lix_key(&v,m),q)<0) lo=m+1;else hi=m;}
            uint32_t a=lo; size_t n=strlen(q);
            while(lo<v.nk && !strncmp(lix_key(&v,lo),q,n)) lo++;
            if(lix_u32(v.dirs+d*8)!=a || lix_u32(v.dirs+d*8+4)!=lo) return -1;
        }
    }
    *x=v; return 0;
}
size_t lix_workspace_size(const lix_index *x) { return x->nr+(size_t)LIX_VARIANTS*26; }
static int rank_cmp(const lix_index *x,uint32_t a,uint32_t b) {
    if(x->compact && x->accelerate) return (int)u16(x->ranks+a*2)-(int)u16(x->ranks+b*2);
    const uint8_t *ra=lix_record(x,a),*rb=lix_record(x,b);
    uint32_t fa=lix_u32(ra+28),fb=lix_u32(rb+28);
    if((fa&2)!=(fb&2)) return (fa&2)?-1:1;
    if(((fa>>2)&3)!=((fb>>2)&3)) return (int)((fa>>2)&3)-(int)((fb>>2)&3);
    uint32_t pa=lix_u32(ra+32),pb=lix_u32(rb+32);
    if(pa!=pb) return pa>pb?-1:1;
    const char *sa=lix_string(x,lix_u32(ra+4)),*sb=lix_string(x,lix_u32(rb+4));
    int na=scalars(sa,strlen(sa)),nb=scalars(sb,strlen(sb));
    if(na!=nb) return na-nb;
    return strcmp(lix_string(x,lix_u32(ra)),lix_string(x,lix_u32(rb)));
}
static int hit_cmp(const lix_index *x,lix_hit a,lix_hit b) {
    return a.tier!=b.tier?(int)a.tier-b.tier:rank_cmp(x,a.record,b.record);
}
static void offer(const lix_index *x,lix_hit h,uint8_t *tiers,lix_hit *out,uint32_t cap,lix_result *r) {
    uint8_t old=tiers[h.record];
    if(h.tier>old) return;
    if(old==255) r->total++;
    if(h.tier<old) {tiers[h.record]=h.tier; r->improvements++;}
    uint32_t at=0;
    while(at<r->count && out[at].record!=h.record) at++;
    if(at<r->count) {
        if(h.tier==old && (h.key>out[at].key || (h.key==out[at].key && h.kind>=out[at].kind))) return;
        for(uint32_t j=at+1;j<r->count;j++) out[j-1]=out[j];
        r->count--;
    } else if(h.tier==old) return; /* Equal-score discarded items cannot re-enter. */
    if(r->count==cap && hit_cmp(x,h,out[r->count-1])>=0) return;
    at=r->count<cap?r->count++:cap-1;
    while(at && hit_cmp(x,h,out[at-1])<0) {out[at]=out[at-1];at--;}
    out[at]=h;
}
static uint32_t lower(const lix_index *x,const char *q,lix_result *r) {
    uint32_t lo=0,hi=x->nk;
    if(x->compact && x->accelerate && q[0]>='a' && q[0]<='z') {
        unsigned second=q[1]>='a'&&q[1]<='z'?(unsigned)(q[1]-'a'):26;
        unsigned d=(unsigned)(q[0]-'a')*27+second;
        lo=lix_u32(x->dirs+d*8);hi=lix_u32(x->dirs+d*8+4);
    }
    while(lo<hi) {uint32_t m=lo+(hi-lo)/2; r->comparisons++;if(strcmp(lix_key(x,m),q)<0)lo=m+1;else hi=m;}
    return lo;
}
static void collect(const lix_index *x,uint32_t k,int edit,const char *q,int qlen,
                    uint8_t *tiers,lix_hit *out,uint32_t cap,lix_result *r) {
    int exact=!strcmp(lix_key(x,k),q); r->keys_scanned++;
    for(uint32_t j=post_start(x,k),end=post_end(x,k);j<end;j++) {
        const uint8_t *p=x->posts+j*(x->compact?3:8);r->posts_scanned++;
        uint8_t kind=p[x->compact?2:4]; if(edit && kind)continue;
        uint8_t tier=edit?5:kind==0?(exact?0:qlen<=2?3:1):kind==1?(exact?2:4):kind==2||exact?3:4;
        lix_hit h={x->compact?u16(p):lix_u32(p),k,tier,kind};
        offer(x,h,tiers,out,cap,r);
    }
}
/* Shell sort: bounded 1400 raw variants, no libc allocator or recursion. */
static void sort_variants(char (*v)[26],uint32_t n) {
    for(uint32_t gap=n/2;gap;gap/=2) {
        for(uint32_t i=gap;i<n;i++) {
            char temp[26];memcpy(temp,v[i],26);uint32_t j=i;
            while(j>=gap && strcmp(v[j-gap],temp)>0) {memcpy(v[j],v[j-gap],26);j-=gap;}
            memcpy(v[j],temp,26);
        }
    }
}
static uint32_t variants(const char *q,size_t n,char (*v)[26]) {
    uint32_t count=0;
    for(size_t i=0;i<n;i++) {memcpy(v[count],q,i);memcpy(v[count]+i,q+i+1,n-i);count++;}
    for(size_t i=0;i+1<n;i++) {memcpy(v[count],q,n+1);v[count][i]=q[i+1];v[count][i+1]=q[i];count++;}
    for(size_t i=0;i<=n;i++) for(char c='a';c<='z';c++) {
        memcpy(v[count],q,i);v[count][i]=c;memcpy(v[count]+i+1,q+i,n-i+1);count++;
        if(i<n) {memcpy(v[count],q,n+1);v[count++][i]=c;}
    }
    sort_variants(v,count);uint32_t unique=0;
    for(uint32_t i=0;i<count;i++) if(strcmp(v[i],q) && (!unique || strcmp(v[i],v[unique-1]))) {
        if(i!=unique)memcpy(v[unique],v[i],26);
        unique++;
    }
    return unique;
}
int lix_query(const lix_index *x,const char *query,size_t n,int fuzzy,
              void *workspace,size_t bytes,lix_hit *out,uint32_t cap,lix_result *r) {
    if(!r)return -1;
    memset(r,0,sizeof(*r));
    if(!x || !x->data || !query || !workspace || !out || !cap || cap>x->nr ||
       bytes<lix_workspace_size(x) || n>256) return -1;
    int qlen=scalars(query,n);
    if(qlen<0 || qlen>64)return -1;
    if(!n)return 0;
    char q[257];memcpy(q,query,n);q[n]=0;
    /* Canonical ASCII contains only lowercase letters/digits. Non-ASCII
     * normalization is the caller's responsibility, preserving R2 norm(). */
    int ascii=1;
    for(size_t i=0;i<n;i++) {
        unsigned c=(uint8_t)q[i];
        if(c>=128)ascii=0;
        else if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')))return -1;
    }
    uint8_t *tiers=workspace;memset(tiers,255,x->nr);
    uint32_t k=lower(x,q,r);
    for(;k<x->nk && !strncmp(lix_key(x,k),q,n);k++) collect(x,k,0,q,qlen,tiers,out,cap,r);
    int alpha=ascii;
    for(size_t i=0;i<n;i++)if(q[i]<'a'||q[i]>'z')alpha=0;
    if(fuzzy && alpha && n>=5 && n<=24 && r->total<cap && (!r->count || out[0].tier!=0)) {
        char (*v)[26]=(char (*)[26])(tiers+x->nr);
        r->variants=variants(q,n,v);
        for(uint32_t i=0;i<r->variants;i++) {
            k=lower(x,v[i],r);
            if(k<x->nk && !strcmp(lix_key(x,k),v[i])) collect(x,k,1,q,qlen,tiers,out,cap,r);
        }
    }
    return 0;
}
