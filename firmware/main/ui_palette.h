#pragma once
#include <stdint.h>
/* Existing scene roles shared by browse pages and settings surfaces. */
typedef struct { uint32_t bg,ink,sub,line,edge,off,on,deep,button; } ui_palette_t;
#define UI_PALETTE_NEUTRAL {0x0f1a23,0xcad7e0,0x9eb3c3,0x3e5261,0x9eb3c3,0x8fa8ba,0xb1c5d3,0x132939,0x1c2d3a}
