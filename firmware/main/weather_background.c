/* R7 light, R6 cloud/precipitation, R8 static moon; original project equations.
 * See assets/weather-background/NOTICE.md and docs/weather-background.md. */
#include "weather_background.h"
#include "../../assets/weather-background/fields.h"
#include <math.h>
#include <string.h>

#define GW 256
#define GH 150
#define TAU 6.2831853071795864769
static int clampi(int n,int lo,int hi) {return n<lo?lo:n>hi?hi:n;}
static float clampf(float n,float lo,float hi) {return n<lo?lo:n>hi?hi:n;}
static int lerp(int a,int b,int fraction,int scale) {return (a*(scale-fraction)+b*fraction+scale/2)/scale;}
static unsigned wrap(int n,unsigned size) {int v=n%(int)size;return (unsigned)(v<0?v+(int)size:v);}
static int sample(const uint8_t *p,int w,int h,int channels,int channel,int xq,int yq,bool repeat_x,bool repeat_y) {
    /* Explicit floor avoids negative-shift or truncation seams. */
    int x=xq/256,y=yq/256;
    if(xq<0&&xq%256)x--;
    if(yq<0&&yq%256)y--;
    int fx=xq-x*256,fy=yq-y*256;
    int x0=repeat_x?(int)wrap(x,(unsigned)w):clampi(x,0,w-1),x1=repeat_x?(int)wrap(x+1,(unsigned)w):clampi(x+1,0,w-1);
    int y0=repeat_y?(int)wrap(y,(unsigned)h):clampi(y,0,h-1),y1=repeat_y?(int)wrap(y+1,(unsigned)h):clampi(y+1,0,h-1);
    int a=lerp(p[(y0*w+x0)*channels+channel],p[(y0*w+x1)*channels+channel],fx,256);
    int b=lerp(p[(y1*w+x0)*channels+channel],p[(y1*w+x1)*channels+channel],fx,256);
    return lerp(a,b,fy,256);
}
static wb_weather_t base_weather(wb_weather_t w) {return w==WB_FOG?WB_OVERCAST:w==WB_THUNDER?WB_RAIN:w;}
void wb_init(wb_state_t *s,uint64_t now,bool low) {
    memset(s,0,sizeof *s);s->last_ms=now;s->low_light=low;s->frame.scene=low?WB_BLACK:WB_NEUTRAL;
}
bool wb_step(wb_state_t *s,wb_input_t in,uint64_t now,wb_frame_t *out) {
    if(!s||!out||(unsigned)in.weather>WB_THUNDER||(unsigned)in.outdoor>WB_NIGHT||now<s->last_ms)return false;
    bool low=in.low_light_known?in.low_light:s->low_light;
    wb_weather_t w=base_weather(in.weather);
    bool valid=in.current_valid&&w!=WB_UNKNOWN&&in.outdoor!=WB_OUTDOOR_UNKNOWN;
    bool moving=valid&&!low&&in.outdoor==WB_DAY&&in.visible&&in.motion_enabled;
    uint64_t phase=s->frame.animation_ms;
    if(valid&&w!=s->phase_weather)phase=0;
    else if(s->frame.moving&&moving)phase+=now-s->last_ms;
    wb_scene_t scene=WB_NEUTRAL;
    if(valid) {
        static const wb_scene_t day[]={WB_NEUTRAL,WB_SUN,WB_CLOUD,WB_GREY,WB_WET,WB_WHITE};
        scene=day[w];
        if(in.outdoor==WB_NIGHT&&(w==WB_SUNNY||w==WB_CLOUDY))scene=w==WB_SUNNY?WB_MOON:WB_CLOUD_MOON;
    }
    if(low)scene=WB_BLACK;
    wb_frame_t f={scene,phase,moving,!s->initialized||scene!=s->frame.scene||(moving&&phase!=s->frame.animation_ms)||(in.visible&&!s->visible)};
    if(valid)s->phase_weather=w;
    s->frame=f;s->last_ms=now;s->low_light=low;s->initialized=true;s->visible=in.visible;*out=f;return true;
}
static void sun(uint8_t *grid,uint64_t ms) {
    memcpy(grid,wb_fields+WBF_SUN,GW*GH*3u);
    double seconds=(double)ms/1000;
    unsigned spin=(unsigned)(fmod(seconds*.014/TAU,1.)*65536.);
    int weights[6];for(int i=0;i<6;i++)weights[i]=(int)lround((.64+.22*sin(fmod(seconds*(.105+i*.006)+i*1.73,TAU)))*256);
    int airx=(int)(fmod(seconds*.006,1.)*128*256),airy=(int)(fmod(seconds*.002,1.)*128*256);
    for(int y=0;y<GH;y++)for(int x=0;x<GW;x++) {
        int i=y*GW+x;const uint8_t *p=wb_fields+WBF_POLAR+i*4;
        unsigned a=(unsigned)p[0]+((unsigned)p[1]<<8),r=(unsigned)p[2]+((unsigned)p[3]<<8);
        int air=sample(wb_fields+WBF_AIR,128,128,1,0,x*128+airx,y*218+airy,true,true);
        a=(a-spin+(unsigned)((air-128)*3))&65535u;
        int sum=0;
        for(int j=0;j<6;j++)sum+=sample(wb_fields+WBF_LOBES+j*512*64,512,64,1,0,(int)a*2,(int)r,true,false)*weights[j];
        int strength=(sum*wb_fields[WBF_SOLAR_GAIN+i]+32640)/(256*255);
        static const int color[]={419,373,267};
        for(int c=0;c<3;c++)grid[i*3+c]=(uint8_t)clampi(grid[i*3+c]+(strength*color[c]+512)/1024,0,255);
    }
}
static void cloud_layer(uint8_t *g,int which,double seconds) {
    static const float starts[]={.48f,.91f,.06f,1.13f},speeds[]={.0010f,.0018f,.0024f,.0020f};
    static const float ys[]={-.09f,.10f,.115f,.48f},sizes[]={.31f,.46f,.40f,.33f};
    static const int opacity[]={168,232,222,166};
    float size=sizes[which],cx=(float)fmod(starts[which]+seconds*speeds[which]+.6,2.4)-.6f;
    int x0=clampi((int)floorf((cx-.72f*size)*GW)-1,0,GW),x1=clampi((int)ceilf((cx+.72f*size)*GW)+1,0,GW);
    int y0=clampi((int)floorf((ys[which]-.5f*size)*GH)-1,0,GH),y1=clampi((int)ceilf((ys[which]+.5f*size)*GH)+1,0,GH);
    const uint8_t *texture=wb_fields+WBF_BANKS+which*128*80*4;
    for(int y=y0;y<y1;y++) {
        int v=(int)lroundf(((((y+.5f)/GH-ys[which])/size+.5f)*80-.5f)*256);
        for(int x=x0;x<x1;x++) {
            int u=(int)lroundf(((((x+.5f)/GW-cx)/size*1.25f+.9f)/1.8f*128-.5f)*256);
            int alpha=sample(texture,128,80,4,3,u,v,false,false)*opacity[which]/255;
            for(int c=0;c<3;c++) {
                int value=sample(texture,128,80,4,c,u,v,false,false),i=(y*GW+x)*3+c;
                g[i]=(uint8_t)lerp(g[i],value,alpha,255);
            }
        }
    }
}
static void cloudy(uint8_t *g,uint64_t ms) {
    memcpy(g,wb_fields+WBF_CLOUD_SKY,GW*GH*3u);
    for(int j=0;j<4;j++)cloud_layer(g,j,(double)ms/1000);
    static const int veil[]={31,59,82};
    for(int i=0;i<GW*GH;i++)for(int c=0;c<3;c++)g[i*3+c]=(uint8_t)(lerp(g[i*3+c],veil[c],92,255)*wb_fields[WBF_EXPOSURE+i]/255);
}
static void grey(uint8_t *g,uint64_t ms,wb_scene_t scene) {
    double t=(double)ms/1000;
    int ox=(int)(fmod(t*.006/2.8,1.)*128*256),oy=(int)(fmod(t*.003/3.5,1.)*128*256);
    static const int low[]={31,46,59},high[]={82,99,112},snow[]={71,94,110},tint[]={212,227,240};
    for(int y=0;y<GH;y++)for(int x=0;x<GW;x++) {
        int u=x*128-ox,v=y*218+oy;
        int n=sample(wb_fields+WBF_CLOUD_FIELD,128,128,3,0,u,v,true,true);
        int folds=sample(wb_fields+WBF_CLOUD_FIELD,128,128,3,1,u,v,true,true);
        int far=sample(wb_fields+WBF_CLOUD_FIELD,128,128,3,2,u+oy,v-ox,true,true);
        int amount=clampi(14+(141*n+115*folds)/255,0,255),i=y*GW+x;
        for(int c=0;c<3;c++) {
            int value=lerp(low[c],high[c],amount,255)+(far-102)*(c==0?10:11)/255;
            value=value*(255-y*71/GH)/255;
            if(scene==WB_WET)value=value*tint[c]/255+(c==0?5:c==1?7:9)*clampi((y*255/GH-76)*255/179,0,255)/255;
            if(scene==WB_WHITE)value=lerp(value,snow[c],56,255);
            g[i*3+c]=(uint8_t)(clampi(value,0,255)*wb_fields[WBF_EXPOSURE+i]/255);
        }
    }
}
static uint32_t random32(uint32_t n) {n^=n>>16;n*=0x7feb352du;n^=n>>15;n*=0x846ca68bu;return n^(n>>16);}
static void blend(uint8_t *g,int x,int y,float alpha,int r,int green,int b) {
    if(x<0||x>=GW||y<0||y>=GH)return;
    int i=y*GW+x,a=clampi((int)lroundf(alpha*wb_fields[WBF_PARTICLE_QUIET+i]),0,255);
    const int color[]={r,green,b};for(int c=0;c<3;c++)g[i*3+c]=(uint8_t)lerp(g[i*3+c],color[c],a,255);
}
static void precipitation(uint8_t *g,uint64_t ms,bool snow) {
    double t=(double)ms/1000;int count=snow?78:126,offset=snow?126:0;
    for(int i=0;i<count;i++) {
        const float *p=wb_particles[offset+i];double travel=p[1]+t*p[3],turn=floor(travel/760.);
        float y=(float)fmod(travel,760.)-80;
        float seed=(float)(random32((uint32_t)i+1u+(uint32_t)fmod(turn,4294967296.)*43u)&65535u)/65535*90-45;
        float wind=snow?sinf((float)fmod(t*(.10+p[2]*.10)+p[4],TAU))*p[8]:sinf((float)fmod(t*.16+p[4],TAU))*4;
        float x=fmodf(p[0]+seed+wind-(snow?0:y*(.055f+p[2]*.055f))+2240,1120)-48;
        float fade=fminf(1,fminf(fmaxf(0,(y+35)/65),fmaxf(0,(660-y)/70)));
        float alpha=p[7]*fade*(.80f+.20f*sinf((float)fmod(t*(snow?.15:.5)+p[4],TAU)));
        if(alpha<=0)continue;
        float px=x/4,py=y/4;
        if(snow) {
            float radius=p[5]*3.8f/4;
            int x0=(int)floorf(px-radius),x1=(int)ceilf(px+radius),y0=(int)floorf(py-radius),y1=(int)ceilf(py+radius);
            for(int yy=y0;yy<=y1;yy++)for(int xx=x0;xx<=x1;xx++) {
                float d=sqrtf((xx+.5f-px)*(xx+.5f-px)+(yy+.5f-py)*(yy+.5f-py))/radius;
                float a=d<.27f?.9f-(.2f/.27f)*d:d<.66f?.7f-(.58f/.39f)*(d-.27f):d<1?.12f*(1-d)/.34f:0;
                blend(g,xx,yy,alpha*a,228,239,244);
            }
        } else {
            float dx=-p[6]*(.07f+p[2]*.07f)/4,dy=p[6]/4,length=dx*dx+dy*dy;
            int x0=(int)floorf(px+dx-1),x1=(int)ceilf(px+1),y0=(int)floorf(py-1),y1=(int)ceilf(py+dy+1);
            for(int yy=y0;yy<=y1;yy++)for(int xx=x0;xx<=x1;xx++) {
                float v=clampf(((xx+.5f-px)*dx+(yy+.5f-py)*dy)/length,0,1);
                float ax=xx+.5f-px-v*dx,ay=yy+.5f-py-v*dy;
                float a=clampf(p[5]/8+.5f-sqrtf(ax*ax+ay*ay),0,1);
                blend(g,xx,yy,alpha*a,196,220,232);
            }
        }
    }
}
static const uint8_t bayer[64]={0,48,12,60,3,51,15,63,32,16,44,28,35,19,47,31,8,56,4,52,11,59,7,55,40,24,36,20,43,27,39,23,2,50,14,62,1,49,13,61,34,18,46,30,33,17,45,29,10,58,6,54,9,57,5,53,42,26,38,22,41,25,37,21};
static void expand(const uint8_t *g,uint8_t *out,wb_scene_t scene) {
    for(int y=0;y<600;y++) {
      int iy=(y+2)/4-1,fy=(y*64+160)&255;
      const uint8_t *row0=g+clampi(iy,0,GH-1)*GW*3,*row1=g+clampi(iy+1,0,GH-1)*GW*3;
      for(int x=0;x<1024;x++) {
        int ix=(x+2)/4-1,fx=(x*64+160)&255,x0=clampi(ix,0,GW-1)*3,x1=clampi(ix+1,0,GW-1)*3;
        int rgb[3];
        for(int c=0;c<3;c++)rgb[c]=lerp(lerp(row0[x0+c],row0[x1+c],fx,256),lerp(row1[x0+c],row1[x1+c],fx,256),fy,256);
        if((scene==WB_MOON||scene==WB_CLOUD_MOON)&&x>=795&&x<875&&y>=63&&y<143) {
            int j=(y-63)*80+x-795,a=wb_fields[WBF_MOON_PATCH_ALPHA+j];
            const uint8_t *patch=wb_fields+WBF_MOON_PATCH+(scene==WB_CLOUD_MOON?80*80*3:0)+j*3;
            for(int c=0;c<3;c++)rgb[c]=lerp(rgb[c],patch[c],a,255);
        }
        int threshold=2*bayer[(y&7)*8+(x&7)]+1;
        int r=clampi((rgb[0]*31*128+threshold*255)/(255*128),0,31);
        int green=clampi((rgb[1]*63*128+threshold*255)/(255*128),0,63);
        int b=clampi((rgb[2]*31*128+threshold*255)/(255*128),0,31);
        unsigned value=(unsigned)((r<<11)|(green<<5)|b),i=(unsigned)(y*1024+x)*2;
        out[i]=(uint8_t)value;out[i+1]=(uint8_t)(value>>8);
      }
    }
}
bool wb_render(const wb_frame_t *f,wb_workspace_t *work,size_t n,uint8_t *out,size_t bytes) {
    if(!f||!work||!out||n<sizeof *work||bytes<WB_OUTPUT_BYTES||(unsigned)f->scene>WB_CLOUD_MOON)return false;
    uint8_t *g=work->rgb;
    switch(f->scene) {
    case WB_BLACK:memset(out,0,WB_OUTPUT_BYTES);return true;
    case WB_NEUTRAL:
        for(int i=0;i<GW*GH;i++){g[i*3]=15;g[i*3+1]=26;g[i*3+2]=36;}break;
    case WB_SUN:sun(g,f->animation_ms);break;
    case WB_CLOUD:cloudy(g,f->animation_ms);break;
    case WB_GREY:case WB_WET:case WB_WHITE:
        grey(g,f->animation_ms,f->scene);
        if(f->scene!=WB_GREY)precipitation(g,f->animation_ms,f->scene==WB_WHITE);
        break;
    case WB_MOON:case WB_CLOUD_MOON:memcpy(g,wb_fields+(f->scene==WB_MOON?WBF_MOON_CLEAR:WBF_MOON_CLOUD),GW*GH*3u);break;
    }
    expand(g,out,f->scene);return true;
}
