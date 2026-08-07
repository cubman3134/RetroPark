#include "PixelConvert.h"
#include "libretro.h"
namespace rp {
static inline uint8_t e5(uint32_t v){ return (uint8_t)((v<<3)|(v>>2)); }
static inline uint8_t e6(uint32_t v){ return (uint8_t)((v<<2)|(v>>4)); }
void convert_to_rgba8(const void* src, uint32_t w, uint32_t h, uint32_t src_pitch,
                      unsigned fmt, uint8_t* dst) {
    for (uint32_t y=0; y<h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(src) + (size_t)y*src_pitch;
        uint8_t* out = dst + (size_t)y*w*4;
        for (uint32_t x=0; x<w; ++x) {
            uint8_t r=0,g=0,b=0;
            if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p = reinterpret_cast<const uint32_t*>(row)[x];
                r=(p>>16)&0xFF; g=(p>>8)&0xFF; b=p&0xFF;
            } else if (fmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p = reinterpret_cast<const uint16_t*>(row)[x];
                r=e5((p>>11)&0x1F); g=e6((p>>5)&0x3F); b=e5(p&0x1F);
            } else if (fmt == RETRO_PIXEL_FORMAT_0RGB1555) {
                uint16_t p = reinterpret_cast<const uint16_t*>(row)[x];
                r=e5((p>>10)&0x1F); g=e5((p>>5)&0x1F); b=e5(p&0x1F);
            }
            out[x*4+0]=r; out[x*4+1]=g; out[x*4+2]=b; out[x*4+3]=255;
        }
    }
}
}
