#include "alarm_audio.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned writes, cutoffs; int16_t block[AA_BLOCK]; } capture;
static aa_result prepare(void *ctx, const aa_output_config *config)
{ (void)ctx; assert(config->codec_volume == 100); return AA_OK; }
static aa_result ok(void *ctx) { (void)ctx; return AA_OK; }
static aa_result cutoff(void *ctx) { ++((capture *)ctx)->cutoffs; return AA_OK; }
static aa_result put(void *ctx, int16_t *data, size_t count, size_t *accepted)
{ capture *c=ctx; assert(count==AA_BLOCK); memcpy(c->block,data,sizeof c->block); ++c->writes; *accepted=count; return AA_OK; }
int main(int argc, char **argv)
{
    assert(argc==2 && aa_reference_count==(uint32_t)strtoul(argv[1],NULL,10));
    aa_source source; aa_source_reset(&source);
    int16_t block[AA_BLOCK];
    uint32_t remaining=aa_reference_count;
    while(remaining) {
        uint32_t n=remaining>AA_BLOCK ? AA_BLOCK : remaining;
        aa_source_render(&source,block,n,false); remaining-=n;
    }
    assert(source.phase==0 && source.rise==(aa_reference_count<30u*AA_RATE?aa_reference_count:30u*AA_RATE));
    source=(aa_source){.phase=aa_reference_count-10u,.rise=30u*AA_RATE};
    aa_source_render(&source,block,20,false);
    assert(source.phase==10u && source.rise==30u*AA_RATE);
    for(unsigned i=0;i<20;++i) {
        uint32_t pos=i<10?aa_reference_count-10u+i:i-10u;
        assert(abs(block[i]-(int)lroundf(aa_reference[pos]*0.5011872336f))<=1);
    }
    capture c={0}; aa_player player;
    aa_init(&player,(aa_sink){.ctx=&c,.prepare=prepare,.enable=ok,.write=put,.quiet=ok,.cutoff=cutoff},(aa_output_config){100});
    assert(aa_set_level(&player,0) && aa_start_preview(&player)==AA_OK);
    assert(aa_pump(&player)==AA_OK);
    for(unsigned i=0;i<8;++i) { assert(aa_pump(&player)==AA_OK); for(unsigned j=0;j<AA_BLOCK;++j) assert(c.block[j]==0); }
    assert(aa_set_level(&player,100));
    for(unsigned i=0;i<8;++i) { assert(aa_pump(&player)==AA_OK); for(unsigned j=0;j<AA_BLOCK;++j) assert(abs(c.block[j])<=16423); }
    assert(player.source.preview && player.source.rise==AA_RATE/10u);
    aa_stop(&player); assert(c.cutoffs==1 && aa_pump(&player)==AA_OK && aa_status(&player,NULL)==AA_IDLE);
    unsigned stopped=c.writes; for(unsigned i=0;i<10;++i) assert(aa_pump(&player)==AA_OK); assert(c.writes==stopped);
    assert(aa_start(&player)==AA_OK && aa_pump(&player)==AA_OK);
    assert(player.source.phase==0 && player.source.rise==0 && !player.source.preview);
    aa_stop(&player); assert(aa_pump(&player)==AA_OK);
    printf("PASS frames=%u loop/rise/preview/levels0-100/stop/restart; player_bytes=%zu\n",aa_reference_count,sizeof(player));
    return 0;
}
