#include <retropark/retropark_abi.h>
#include <vector>
#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {
struct DrivenCore {
    rp_host_iface host{};
    std::vector<uint8_t> fb;   // 64*64*4
    uint32_t frame = 0;
    static const uint32_t W = 64, H = 64;
};

void dc_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_DRIVEN;
    out->graphics_api = RP_GFX_NONE;
    out->id = "refcore_driven";
}
void dc_get_av_info(rp_core*, rp_av_info* out) {
    out->fps = 60.0; out->sample_rate = 0.0;
    out->base_width = DrivenCore::W; out->base_height = DrivenCore::H;
    out->max_width = DrivenCore::W; out->max_height = DrivenCore::H;
    out->pixel_format = RP_FMT_R8G8B8A8_UNORM;
}
rp_core* dc_create(const rp_host_iface* host) {
    auto* c = new DrivenCore();
    c->host = *host;
    c->fb.assign((size_t)DrivenCore::W * DrivenCore::H * 4, 0);
    return reinterpret_cast<rp_core*>(c);
}
void dc_destroy(rp_core* core) { delete reinterpret_cast<DrivenCore*>(core); }
void dc_run_frame(rp_core* core) {
    auto* c = reinterpret_cast<DrivenCore*>(core);
    uint8_t t = (uint8_t)((c->frame++ % 120) * 255 / 120);   // rising blue
    for (uint32_t i = 0; i < DrivenCore::W * DrivenCore::H; ++i) {
        uint8_t* p = c->fb.data() + (size_t)i * 4;
        p[0] = 0; p[1] = 255; p[2] = t; p[3] = 255;          // green with rising blue
    }
    c->host.video_refresh(c->host.host, c->fb.data(), DrivenCore::W, DrivenCore::H, DrivenCore::W * 4);
}

const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, dc_get_info, dc_create, dc_destroy,
    /*set_surfaces*/nullptr, /*start*/nullptr, /*stop*/nullptr,
    dc_get_av_info, dc_run_frame,
    /*serialize_size*/nullptr, /*serialize*/nullptr, /*unserialize*/nullptr
};
}
RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
