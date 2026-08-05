#pragma once
#include "render/IRenderBackend.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>

namespace rp {
class D3D11Backend : public IRenderBackend {
public:
    rp_result initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) override;
    rp_result allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                std::vector<rp_surface_desc>& out, std::string& err) override;
    rp_result composite_and_present(uint32_t ready_index, bool has_frame,
                                    uint8_t* out_rgba, std::string& err) override;

    // Test helpers.
    static bool probe_shared_keyed_mutex();
    rp_result readback_surface_pixel(uint32_t index, uint32_t x, uint32_t y,
                                     uint8_t rgba_out[4], std::string& err);
    ID3D11Device* device() const { return device_.Get(); }

protected:
    struct Surface {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        void* handle = nullptr;
    };
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Device1> device1_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;   // null when headless
    std::vector<Surface> surfaces_;
    uint32_t width_ = 0, height_ = 0;
};
}
