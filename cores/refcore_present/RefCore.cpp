#include <retropark/retropark_abi.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <vector>
using Microsoft::WRL::ComPtr;

#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {
struct Slot {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<IDXGIKeyedMutex> keyed;
    ComPtr<ID3D11RenderTargetView> rtv;
    uint32_t index = 0;
    uint64_t generation = 0;
};

struct RefCore {
    rp_host_iface host{};
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11DeviceContext> ctx;
    std::vector<Slot> slots;
    std::thread th;
    std::atomic<bool> running{false};
    uint32_t cursor = 0;
    uint32_t frame = 0;

    void loop() {
        while (running.load()) {
            if (!slots.empty()) {
                Slot& s = slots[cursor];
                cursor = (cursor + 1) % (uint32_t)slots.size();
                if (SUCCEEDED(s.keyed->AcquireSync(0, 100))) {
                    float t = (float)((frame++ % 120)) / 120.0f;   // animate
                    const float col[4] = { 0.0f, 1.0f, t, 1.0f };  // green with rising blue
                    ctx->ClearRenderTargetView(s.rtv.Get(), col);
                    ctx->Flush();
                    s.keyed->ReleaseSync(1);
                    host.submit_frame(host.host, s.index, s.generation);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
};

void ref_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_PRESENTING;
    out->graphics_api = RP_GFX_D3D11;
    out->id = "refcore_present";
}

rp_core* ref_create(const rp_host_iface* host) {
    auto* c = new RefCore();
    c->host = *host;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &c->dev, &fl, &c->ctx))) { delete c; return nullptr; }
    if (FAILED(c->dev.As(&c->dev1))) { delete c; return nullptr; }
    return reinterpret_cast<rp_core*>(c);
}

void ref_destroy(rp_core* core) {
    auto* c = reinterpret_cast<RefCore*>(core);
    if (c->running.load()) { c->running = false; if (c->th.joinable()) c->th.join(); }
    delete c;
}

rp_result ref_set_surfaces(rp_core* core, const rp_surface_desc* descs, uint32_t count) {
    auto* c = reinterpret_cast<RefCore*>(core);
    c->slots.clear();
    for (uint32_t i = 0; i < count; ++i) {
        Slot s; s.index = descs[i].index; s.generation = descs[i].generation;
        if (FAILED(c->dev1->OpenSharedResource1((HANDLE)descs[i].shared_handle, IID_PPV_ARGS(&s.tex))))
            return RP_ERR_DEVICE;
        if (FAILED(s.tex.As(&s.keyed))) return RP_ERR_DEVICE;
        if (FAILED(c->dev->CreateRenderTargetView(s.tex.Get(), nullptr, &s.rtv))) return RP_ERR_DEVICE;
        c->slots.push_back(std::move(s));
    }
    c->cursor = 0;
    return RP_OK;
}

rp_result ref_start(rp_core* core) {
    auto* c = reinterpret_cast<RefCore*>(core);
    if (c->running.load()) return RP_OK;
    c->running = true;
    c->th = std::thread([c]{ c->loop(); });
    return RP_OK;
}

rp_result ref_stop(rp_core* core) {
    auto* c = reinterpret_cast<RefCore*>(core);
    c->running = false;
    if (c->th.joinable()) c->th.join();
    return RP_OK;
}

const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, ref_get_info, ref_create, ref_destroy,
    ref_set_surfaces, ref_start, ref_stop
};
}

RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
