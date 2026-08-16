#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <retropark/retropark_abi.h>

namespace rp {
struct IRenderBackend {
    virtual ~IRenderBackend() = default;
    virtual rp_result initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) = 0;
    virtual rp_result allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                        std::vector<rp_surface_desc>& out, std::string& err) = 0;
    // If out_rgba != null, the composited image (w*h*4, RGBA8) is copied there (headless).
    virtual rp_result composite_and_present(uint32_t ready_index, uint64_t sync_value, bool has_frame,
                                            uint8_t* out_rgba, std::string& err) = 0;
    virtual rp_result composite_driven(const void* data, uint32_t width, uint32_t height,
                                       uint32_t pitch, bool dupe, uint8_t* out_rgba,
                                       std::string& err) = 0;

    // External-sync accessors (Vulkan; default no-op for backends without external sync).
    virtual void* present_sync_handle() const { return nullptr; }
    // CONSUME timeline handle: the host-owned one-directional back-pressure channel the presenting core
    // waits on before reusing a shared slot (Vulkan multi-slot). null => single-timeline lock-step.
    virtual void* present_consume_sync_handle() const { return nullptr; }
    virtual void  present_device_uuid(uint8_t out[16]) const { for (int i=0;i<16;++i) out[i]=0; }

    // GL zero-copy (B2). The host's GL context handle to share with (GL backend only), and compositing a core-
    // supplied external GL texture directly. Non-GL backends: no context / unsupported.
    virtual void* gl_context() const { return nullptr; }
    virtual rp_result composite_external_gl(unsigned /*tex*/, uint32_t /*w*/, uint32_t /*h*/,
                                            bool /*bottom_left_origin*/, uint8_t* /*out_rgba*/, std::string& err) {
        err = "backend has no GL external-texture composite"; return RP_ERR_UNSUPPORTED;
    }
};
}
