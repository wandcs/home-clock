#define _POSIX_C_SOURCE 200809L
#include "alarm_bsp.h"
#include "aa_host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned scenario;
static pthread_t task;
static void (*entry)(void *);
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool entered, released;
static atomic_uint blocked_once, amp, high_count, reg31, reg32, opened, writes, normal;
static atomic_uint runtime, fail_off;
static audio_codec_data_if_t data_if;
static audio_codec_ctrl_if_t ctrl_if;
static audio_codec_if_t codec_if;
static int handle;

static void nap(void) { struct timespec t = { .tv_nsec=1000000 }; nanosleep(&t,NULL); }
static void wait_for(bool (*predicate)(void))
{
    for (unsigned n=0; n<5000; ++n) { if (predicate()) return; nap(); }
    assert(!"bounded host wait expired");
}
static void block_here(void)
{
    if (atomic_exchange(&blocked_once,1)) return;
    pthread_mutex_lock(&gate); entered=true; pthread_cond_broadcast(&changed);
    while (!released) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
}
static void await_block(void)
{
    pthread_mutex_lock(&gate);
    while (!entered) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
}
static void release_block(void)
{ pthread_mutex_lock(&gate); released=true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate); }
int gpio_config(const gpio_config_t *cfg)
{ assert(cfg->pin_bit_mask==(UINT64_C(1)<<53) && cfg->mode==GPIO_MODE_INPUT_OUTPUT); return 0; }
int gpio_set_level(int pin,int value)
{
    assert(pin==53);
    if (!value && atomic_load(&fail_off)) return -1;
    atomic_store(&amp,(unsigned)value); if (value) atomic_fetch_add(&high_count,1);
    return 0;
}
int gpio_get_level(int pin) { assert(pin==53); return (int)atomic_load(&amp); }
int i2s_new_channel(const i2s_chan_config_t *cfg,void **tx,void **rx)
{
    assert(rx==NULL && cfg->dma_desc_num==4 && cfg->dma_frame_num==256 && cfg->auto_clear);
    if (scenario==2) return -1;
    *tx=&handle; return 0;
}
int i2s_channel_init_std_mode(void *tx,const i2s_std_config_t *cfg)
{
    assert(tx==&handle && cfg->clk_cfg==22050 && cfg->slot_cfg==16);
    assert(cfg->gpio_cfg.mclk==13 && cfg->gpio_cfg.bclk==12 && cfg->gpio_cfg.ws==10);
    assert(cfg->gpio_cfg.dout==9 && cfg->gpio_cfg.din==-1); return 0;
}
int i2s_channel_enable(void *tx) { assert(tx==&handle); return 0; }
int i2s_channel_get_info(void *tx,i2s_chan_info_t *out)
{ assert(tx==&handle); out->total_dma_buf_size=scenario==14?4096:2048; return 0; }
int i2s_channel_write(void *tx,const void *buf,size_t bytes,size_t *accepted,unsigned timeout)
{
    assert(tx==&handle && bytes==AA_BLOCK*2 && timeout==40);
    bool nonzero=false; const int16_t *pcm=buf;
    for (size_t n=0;n<bytes/2;++n) nonzero |= pcm[n]!=0;
    atomic_fetch_add(&writes,1);
    if (!atomic_load(&runtime)) assert(!nonzero && !atomic_load(&amp));
    if (nonzero) {
        atomic_fetch_add(&normal,1);
        if (scenario==10) block_here();
    }
    *accepted=bytes;
    if ((scenario==6 && !atomic_load(&runtime)) || (scenario==9 && nonzero)) { *accepted=bytes-2; return 0; }
    if (scenario==7 && nonzero) { *accepted=200; return -1; }
    if (scenario==8 && nonzero) { *accepted=200; return ESP_ERR_TIMEOUT; }
    nap(); return 0;
}
void *bsp_i2c_get_handle(void) { return &handle; }
const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *cfg)
{ assert(cfg->port==0 && cfg->rx_handle==NULL && cfg->tx_handle==&handle); return &data_if; }
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *cfg)
{ assert(cfg->addr==0x30 && cfg->bus_handle==&handle); return &ctrl_if; }
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *cfg)
{
    assert(cfg->pa_pin==-1 && cfg->codec_mode==ESP_CODEC_DEV_WORK_MODE_DAC && cfg->use_mclk);
    assert(cfg->hw_gain.pa_voltage==5 && cfg->hw_gain.codec_dac_voltage==3.3f);
    if (scenario==12) block_here();
    return &codec_if;
}
void *esp_codec_dev_new(esp_codec_dev_cfg_t *cfg)
{ assert(cfg->dev_type==ESP_CODEC_DEV_TYPE_OUT && cfg->data_if==&data_if); return &handle; }
int esp_codec_dev_read_reg(void *c,int reg,int *value)
{ assert(c==&handle); *value=(int)atomic_load(reg==0x31?&reg31:&reg32); return 0; }
int esp_codec_dev_set_out_mute(void *c,bool value)
{
    assert(c==&handle);
    if (!value && scenario==11) block_here();
    if (scenario!=4) atomic_store(&reg31,value?0x60:0);
    return 0; /* scenario4 simulates the locked driver swallowing write error */
}
int esp_codec_dev_set_out_vol(void *c,int value)
{ assert(c==&handle && value==100); atomic_store(&reg32,scenario==5?199:198); return 0; }
int esp_codec_dev_open(void *c,esp_codec_dev_sample_info_t *fs)
{
    assert(c==&handle && fs->sample_rate==AA_RATE && fs->channel==1 && fs->bits_per_sample==16);
    if (scenario==3) return -1;
    atomic_store(&opened,1);
    atomic_store(&reg31,0); /* codec internally unmutes on enable, never PA */
    return 0;
}
static void *runner(void *unused) { (void)unused; entry(NULL); return NULL; }
int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *out,unsigned caps)
{
    assert(!strcmp(name,"alarm-audio") && stack==4096 && !arg && priority==AA_WORKER_PRIORITY && !out);
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (scenario==1) return 0;
    entry=fn; assert(!pthread_create(&task,NULL,runner,NULL)); return pdPASS;
}
void vTaskDeleteWithCaps(void *task_handle) { assert(!task_handle); pthread_exit(NULL); }
void vTaskDelay(unsigned ticks)
{
    assert(ticks==1);
    /* The sink already blocks for DMA; sleeping in PLAYING causes starvation. */
    assert(aa_status(aa_bsp_player(),NULL)!=AA_PLAYING);
    nap();
}
int64_t esp_timer_get_time(void)
{
    struct timespec now; assert(clock_gettime(CLOCK_MONOTONIC,&now)==0);
    return (int64_t)now.tv_sec*1000000+now.tv_nsec/1000;
}
unsigned uxTaskGetStackHighWaterMark(void *p) { assert(!p && aa_status(aa_bsp_player(),NULL)!=AA_PLAYING); return 1234; /* NOT measured stack */ }
static bool prepared(void) { aa_bsp_diagnostic d; aa_bsp_diagnostics(&d); return d.preparation==AA_PREP_READY || d.preparation==AA_PREP_FAILED; }
static bool idle(void) { return aa_status(aa_bsp_player(),NULL)==AA_IDLE; }
static bool failed(void) { return aa_status(aa_bsp_player(),NULL)==AA_FAILED; }
static bool playing(void) { return atomic_load(&normal)>=3; }
static bool zero_written(void) { return atomic_load(&writes)>=10; }

int main(int argc,char **argv)
{
    assert(argc==2); scenario=(unsigned)atoi(argv[1]);
    assert(!aa_bsp_ready() && !aa_bsp_player());
    aa_result result=aa_bsp_startup();
    aa_player *p=aa_bsp_player(); assert(p && p->output.codec_volume==100);
    assert(aa_bsp_startup()==AA_BUSY);
    if (scenario==1) {
        assert(result==AA_OPEN_ERROR && !aa_bsp_ready() && !atomic_load(&amp));
    } else {
        assert(result==AA_OK);
        if (scenario==12) {
            await_block(); assert(!aa_bsp_ready()); assert(aa_start(p)==AA_OK);
            aa_stop(p); assert(!atomic_load(&amp)); release_block();
        }
        wait_for(prepared);
        if ((scenario>=2 && scenario<=6) || scenario==14) {
            wait_for(failed); assert(!aa_bsp_ready() && !atomic_load(&normal) && !atomic_load(&high_count));
        } else {
            assert(aa_bsp_ready()); wait_for(idle);
            aa_bsp_diagnostic d; aa_bsp_diagnostics(&d);
            assert(d.mute_readback && d.volume_readback && d.dac_register==198);
            assert(d.dma_data_bytes==2048 && !d.pa_on && !d.normal_blocks);
            assert(!atomic_load(&normal) && !atomic_load(&high_count));
            atomic_store(&runtime,1); if(scenario==15)assert(aa_set_level(p,0)); assert(aa_start(p)==AA_OK);
            if (scenario==15) {
                wait_for(zero_written);assert(!atomic_load(&amp)&&(atomic_load(&reg31)&0x60)==0x60);
                assert(!atomic_load(&normal));aa_stop(p);wait_for(idle);
            } else if (scenario==11) {
                await_block(); aa_stop(p); assert(!atomic_load(&amp)); release_block(); wait_for(idle);
                assert(!atomic_load(&high_count)); /* late unmute never re-enables PA */
            } else if (scenario==10) {
                await_block(); assert(atomic_load(&amp)); aa_stop(p); assert(!atomic_load(&amp));
                release_block(); wait_for(idle); assert(atomic_load(&high_count)==1);
            } else if (scenario>=7 && scenario<=9) {
                wait_for(failed); assert(!atomic_load(&amp) && !aa_bsp_ready());
                aa_result err; aa_status(p,&err); assert(err==AA_WRITE_ERROR || err==AA_TIMEOUT || err==AA_SHORT_WRITE);
            } else {
                wait_for(playing);
                if (scenario==13) atomic_store(&fail_off,1);
                aa_stop(p);
                if (scenario==13) { wait_for(failed); aa_bsp_diagnostics(&d); assert(d.cutoff_failures && !d.ready); atomic_store(&fail_off,0); }
                else { assert(!atomic_load(&amp)); wait_for(idle); }
            }
        }
        aa_bsp_shutdown(); assert(!pthread_join(task,NULL));
    }
    aa_bsp_diagnostic d; aa_bsp_diagnostics(&d);
    assert(!d.ready && !d.worker_active && !atomic_load(&amp));
    printf("PASS adapter contract scenario %u; no board/timing/listening claim\n",scenario);
    return 0;
}
