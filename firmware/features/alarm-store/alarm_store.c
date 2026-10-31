#include "alarm_store.h"
#include <string.h>

static bool known(const as_result *r) {return r->origin!=AS_ORIGIN_NONE;}
static as_result read_image(const as_store *s) {
    as_result r={.status=AS_READ_ERROR};
    if(!s||!s->port.read)return r;
    uint8_t bytes[AS_WIRE_MAX];size_t n=0;
    as_raw_status raw=s->port.read(s->port.ctx,bytes,sizeof bytes,&n,&r.read_error);
    if(raw==AS_RAW_MISSING) {
        am_config empty;am_config_empty(&empty);(void)at_image_first(&empty,&r.image);
        r.status=AS_MISSING;r.origin=AS_ORIGIN_MISSING;return r;
    }
    if(raw==AS_RAW_OVERSIZE) {r.status=AS_FOREIGN;return r;}
    if(raw!=AS_RAW_OK)return r;
    if(n>sizeof bytes) {r.status=AS_READ_ERROR;return r;}
    as_decode_status decoded=as_decode(bytes,n,&r.image);
    if(decoded==AS_DECODE_CURRENT) {r.status=AS_CURRENT;r.origin=AS_ORIGIN_CURRENT;}
    else if(decoded==AS_DECODE_LEGACY) {r.status=AS_LEGACY;r.origin=AS_ORIGIN_LEGACY;}
    else r.status=decoded==AS_DECODE_FOREIGN?AS_FOREIGN:AS_BAD;
    return r;
}
void as_init(as_store *s,as_port port) {
    if(s)*s=(as_store){.port=port,.blocked=true};
}
as_result as_reload(as_store *s) {
    as_result r=read_image(s);
    if(s)s->blocked=!known(&r);
    return r;
}
static as_result invalid(void) {return (as_result){.status=AS_INVALID};}
static as_result before_write(as_store *s) {
    if(!s||!s->port.read||!s->port.replace)return invalid();
    if(s->blocked)return (as_result){.status=AS_BLOCKED};
    as_result r=read_image(s);
    if(!known(&r))s->blocked=true;
    return r;
}
static as_result replace_image(as_store *s,const as_result *before,const at_image *target) {
    if(before->origin==AS_ORIGIN_CURRENT&&at_image_equal(&before->image,target)) {
        as_result r=*before;r.status=AS_UNCHANGED;return r;
    }
    uint8_t bytes[AS_WIRE_MAX];size_t n=as_encode(target,bytes,sizeof bytes);
    if(!n)return invalid();
    int write_error=s->port.replace(s->port.ctx,bytes,n);
    as_result after=read_image(s);after.write_error=write_error;
    if(after.origin==AS_ORIGIN_CURRENT&&at_image_equal(&after.image,target))after.status=AS_SAVED;
    else if(known(&after)) {
        if(after.origin==before->origin&&at_image_equal(&after.image,&before->image))after.status=AS_OLD_VALUE;
        else after.status=AS_DIFFERENT;
    } else {after.status=AS_UNCERTAIN;s->blocked=true;}
    return after;
}
as_result as_save_ui(as_store *s,const am_config *expected,const am_config *proposed) {
    if(!am_config_valid(expected)||!am_config_valid(proposed))return invalid();
    as_result before=before_write(s);
    if(!known(&before))return before;
    if(!am_config_equal(expected,&before.image.config)) {before.status=AS_DIFFERENT;return before;}
    at_image target;
    if(!at_image_reconfigure(&before.image,proposed,&target))return invalid();
    return replace_image(s,&before,&target);
}
static bool trigger_change(const at_image *before,const at_image *after) {
    if(!at_image_valid(before)||!at_image_valid(after)||before->config.next_id!=after->config.next_id||before->config.count!=after->config.count)return false;
    for(unsigned i=0;i<before->config.count;++i) {
        const am_entry *a=&before->config.entries[i],*b=&after->config.entries[i];
        const at_record *x=&before->records[i],*y=&after->records[i];
        if(a->id!=b->id||a->hour!=b->hour||a->minute!=b->minute||a->mode!=b->mode||y->day1<x->day1)return false;
        if(a->enabled==b->enabled) {
            if(x->revision!=y->revision)return false;
            if(x->once_revision==x->revision&&x->once_revision&&y->once_revision!=x->once_revision)return false;
        } else {
            /* A failed mark may leave only the core's RAM consumption fact.
             * Its later silent close includes a newly advanced day checkpoint
             * in this same atomic image; no earlier durable pending is needed. */
            bool consumed=x->once_revision==x->revision||y->day1>x->day1;
            if(a->mode!=AF_ONCE||!a->enabled||b->enabled||x->revision==UINT32_MAX||!consumed||y->revision!=x->revision+1u||y->once_revision)return false;
        }
    }
    return true;
}
as_result as_save_trigger(as_store *s,const at_image *expected,const at_image *proposed) {
    if(!trigger_change(expected,proposed))return invalid();
    as_result before=before_write(s);
    if(!known(&before))return before;
    if(!at_image_equal(expected,&before.image)) {before.status=AS_DIFFERENT;return before;}
    return replace_image(s,&before,proposed);
}
