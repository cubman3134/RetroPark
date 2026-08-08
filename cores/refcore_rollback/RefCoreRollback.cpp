#include <retropark/retropark_abi.h>
#include <vector>
#include <cstring>
#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {
struct RollbackCore {
    rp_host_iface host{};
    std::vector<uint8_t> fb;
    uint32_t acc = 0;
    static const uint32_t W = 64, H = 64;
};

void rc_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_DRIVEN;
    out->graphics_api = RP_GFX_NONE;
    out->id = "refcore_rollback";
}
void rc_get_av_info(rp_core*, rp_av_info* out) {
    out->fps = 60.0; out->sample_rate = 0.0;
    out->base_width = RollbackCore::W; out->base_height = RollbackCore::H;
    out->max_width = RollbackCore::W; out->max_height = RollbackCore::H;
    out->pixel_format = RP_FMT_R8G8B8A8_UNORM;
}
rp_core* rc_create(const rp_host_iface* host) {
    auto* c = new RollbackCore();
    c->host = *host;
    c->fb.assign((size_t)RollbackCore::W * RollbackCore::H * 4, 0);
    return reinterpret_cast<rp_core*>(c);
}
void rc_destroy(rp_core* core) { delete reinterpret_cast<RollbackCore*>(core); }
void rc_run_frame(rp_core* core) {
    auto* c = reinterpret_cast<RollbackCore*>(core);
    rp_input_state in{};
    c->host.input_state(c->host.host, 0, &in);          // v5: port 0
    c->acc += in.keys['X'] ? 2u : 1u;                   // state depends on input
    uint8_t v = (uint8_t)(c->acc & 0xFFu);
    for (uint32_t i = 0; i < RollbackCore::W * RollbackCore::H; ++i) {
        uint8_t* p = c->fb.data() + (size_t)i * 4;
        p[0] = v; p[1] = 0; p[2] = 0; p[3] = 255;       // red = acc low byte (diverged state is visible)
    }
    c->host.video_refresh(c->host.host, c->fb.data(), RollbackCore::W, RollbackCore::H, RollbackCore::W * 4);
}
size_t rc_serialize_size(rp_core*) { return sizeof(uint32_t); }
rp_result rc_serialize(rp_core* core, void* data, size_t size) {
    if (!data || size < sizeof(uint32_t)) return RP_ERR_BAD_ARG;
    std::memcpy(data, &reinterpret_cast<RollbackCore*>(core)->acc, sizeof(uint32_t));
    return RP_OK;
}
rp_result rc_unserialize(rp_core* core, const void* data, size_t size) {
    if (!data || size < sizeof(uint32_t)) return RP_ERR_BAD_ARG;
    std::memcpy(&reinterpret_cast<RollbackCore*>(core)->acc, data, sizeof(uint32_t));
    return RP_OK;
}
const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, rc_get_info, rc_create, rc_destroy,
    /*set_surfaces*/nullptr, /*start*/nullptr, /*stop*/nullptr,
    rc_get_av_info, rc_run_frame,
    rc_serialize_size, rc_serialize, rc_unserialize
};
}
RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
