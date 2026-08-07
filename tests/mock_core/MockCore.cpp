#include <retropark/retropark_abi.h>

#if defined(_WIN32)
  #define MOCK_EXPORT extern "C" __declspec(dllexport)
#else
  #define MOCK_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
struct MockCore { int started = 0; uint32_t surfaces = 0; };

void mock_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_PRESENTING;
    out->graphics_api = RP_GFX_D3D11;
    out->id = "mock_core";
}
rp_core* mock_create(const rp_host_iface*) { return reinterpret_cast<rp_core*>(new MockCore()); }
void mock_destroy(rp_core* c) { delete reinterpret_cast<MockCore*>(c); }
rp_result mock_set_surfaces(rp_core* c, const rp_surface_set* set) {
    reinterpret_cast<MockCore*>(c)->surfaces = set->count; return RP_OK;
}
rp_result mock_start(rp_core* c) { reinterpret_cast<MockCore*>(c)->started = 1; return RP_OK; }
rp_result mock_stop(rp_core* c) { reinterpret_cast<MockCore*>(c)->started = 0; return RP_OK; }

const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, mock_get_info, mock_create, mock_destroy,
    mock_set_surfaces, mock_start, mock_stop,
    nullptr, nullptr, nullptr, nullptr, nullptr
};
}

MOCK_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
