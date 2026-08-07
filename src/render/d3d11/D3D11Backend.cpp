#include "render/d3d11/D3D11Backend.h"
#include <cstring>
using Microsoft::WRL::ComPtr;

namespace rp {

static rp_result make_device(bool warp, ComPtr<ID3D11Device>& dev,
                             ComPtr<ID3D11DeviceContext>& ctx, std::string& err) {
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr,
        warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { err = "D3D11CreateDevice failed"; return RP_ERR_DEVICE; }
    return RP_OK;
}

bool D3D11Backend::probe_shared_keyed_mutex() {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; std::string e;
    if (make_device(true, dev, ctx, e) != RP_OK) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 4; d.Height = 4; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) return false;
    ComPtr<IDXGIResource1> res; if (FAILED(tex.As(&res))) return false;
    HANDLE h = nullptr;
    if (FAILED(res->CreateSharedHandle(nullptr,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h))) return false;
    if (h) ::CloseHandle(h);
    return true;
}

rp_result D3D11Backend::initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) {
    width_ = w; height_ = h;
    const bool headless = (native_window == nullptr);
    // The reference core (refcore_present) always creates a WARP device and shares its
    // surfaces with the host via an NT handle + keyed mutex; that interop only succeeds
    // when both devices sit on the SAME adapter. So the windowed host device is ALSO WARP
    // here, matching the core, rather than D3D_DRIVER_TYPE_HARDWARE. Moving the host to a
    // hardware adapter is a deliberate later-slice optimization that requires solving
    // cross-adapter (or same-adapter-hardware) sharing first — don't "optimize" this to
    // hardware without addressing that.
    rp_result r = make_device(/*warp=*/true, device_, ctx_, err);
    if (r != RP_OK) return r;
    if (FAILED(device_.As(&device1_))) { err = "no ID3D11Device1"; return RP_ERR_DEVICE; }

    swapchain_.Reset();
    backbuffer_rtv_.Reset();
    if (!headless) {
        ComPtr<IDXGIDevice> dxgiDev;
        if (FAILED(device_.As(&dxgiDev))) { err = "no IDXGIDevice"; return RP_ERR_DEVICE; }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDev->GetAdapter(&adapter))) { err = "no IDXGIAdapter"; return RP_ERR_DEVICE; }
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) { err = "no IDXGIFactory2"; return RP_ERR_DEVICE; }

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width = w; sc.Height = h; sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sc.SampleDesc.Count = 1; sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = 2; sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), static_cast<HWND>(native_window),
                                                   &sc, nullptr, nullptr, &swapchain_))) {
            err = "swapchain"; return RP_ERR_DEVICE;
        }
        ComPtr<ID3D11Texture2D> bb;
        if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb)))) { err = "swapchain buffer"; return RP_ERR_DEVICE; }
        if (FAILED(device_->CreateRenderTargetView(bb.Get(), nullptr, &backbuffer_rtv_))) {
            err = "backbuffer RTV"; return RP_ERR_DEVICE;
        }
    }
    return RP_OK;
}

rp_result D3D11Backend::allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                          std::vector<rp_surface_desc>& out, std::string& err) {
    surfaces_.clear(); out.clear();
    width_ = w; height_ = h;
    for (uint32_t i = 0; i < count; ++i) {
        Surface s;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &s.tex))) {
            err = "CreateTexture2D(shared) failed"; return RP_ERR_DEVICE;
        }
        if (FAILED(s.tex.As(&s.keyed))) { err = "no keyed mutex"; return RP_ERR_DEVICE; }
        if (FAILED(device_->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv))) {
            err = "CreateSRV failed"; return RP_ERR_DEVICE;
        }
        ComPtr<IDXGIResource1> res;
        if (FAILED(s.tex.As(&res))) { err = "no IDXGIResource1"; return RP_ERR_DEVICE; }
        HANDLE handle = nullptr;
        if (FAILED(res->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
            err = "CreateSharedHandle failed"; return RP_ERR_DEVICE;
        }
        s.handle = handle;

        rp_surface_desc desc{};
        desc.index = i; desc.width = w; desc.height = h;
        desc.format = RP_FMT_R8G8B8A8_UNORM;
        desc.shared_handle = handle;
        desc.generation = 0;   // Runtime overwrites with the ring generation.
        surfaces_.push_back(std::move(s));
        out.push_back(desc);
    }
    return RP_OK;
}

rp_result D3D11Backend::readback_surface_pixel(uint32_t index, uint32_t x, uint32_t y,
                                               uint8_t rgba_out[4], std::string& err) {
    if (index >= surfaces_.size()) { err = "bad index"; return RP_ERR_BAD_ARG; }
    Surface& s = surfaces_[index];
    HRESULT acq_hr = s.keyed->AcquireSync(1, 100);
    if (acq_hr != S_OK) { err = "acquire timeout"; return RP_ERR_TIMEOUT; }

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = width_; sd.Height = height_; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging))) {
        s.keyed->ReleaseSync(0); err = "staging create failed"; return RP_ERR_DEVICE;
    }
    ctx_->CopyResource(staging.Get(), s.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        s.keyed->ReleaseSync(0); err = "map failed"; return RP_ERR_DEVICE;
    }
    const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
    const uint8_t* px = row + x * 4;
    rgba_out[0]=px[0]; rgba_out[1]=px[1]; rgba_out[2]=px[2]; rgba_out[3]=px[3];
    ctx_->Unmap(staging.Get(), 0);
    s.keyed->ReleaseSync(0);
    return RP_OK;
}

rp_result D3D11Backend::composite_and_present(uint32_t ready_index, uint64_t sync_value, bool has_frame,
                                              uint8_t* out_rgba, std::string& err) {
    (void)sync_value;
    // Windowed readback (swapchain present + CPU pixel readback in the same call) is not
    // implemented: the render target in that path is the back buffer, but the readback below
    // copies from the offscreen texture, which is never drawn into when a swapchain exists.
    // That would silently hand the caller uninitialized/stale pixels. Reject it explicitly
    // rather than special-case it now; full windowed readback is a later-slice feature.
    if (swapchain_ && out_rgba) {
        err = "windowed readback (swapchain + out_rgba) is not supported in Slice A";
        return RP_ERR_UNSUPPORTED;
    }
    if (!compositor_ready_) {
        rp_result r = compositor_.initialize(device_.Get(), err);
        if (r != RP_OK) return r;
        compositor_ready_ = true;
    }
    // Ensure an offscreen RTV of the current size. Needed whenever there's no swapchain
    // (headless target) or a readback was requested even while windowed.
    if (!offscreen_ && (!swapchain_ || out_rgba)) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width=width_; d.Height=height_; d.MipLevels=1; d.ArraySize=1;
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1;
        d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &offscreen_))) { err="offscreen"; return RP_ERR_DEVICE; }
        if (FAILED(device_->CreateRenderTargetView(offscreen_.Get(), nullptr, &offscreen_rtv_))) { err="offRTV"; return RP_ERR_DEVICE; }
    }

    ID3D11ShaderResourceView* core_srv = nullptr;
    bool acquired = false;
    if (has_frame && ready_index < surfaces_.size()) {
        HRESULT acq_hr = surfaces_[ready_index].keyed->AcquireSync(1, 100);
        if (acq_hr != S_OK) { err="acquire timeout"; return RP_ERR_TIMEOUT; }
        acquired = true;
        core_srv = surfaces_[ready_index].srv.Get();
    }

    ID3D11RenderTargetView* target = swapchain_ ? backbuffer_rtv_.Get() : offscreen_rtv_.Get();
    rp_result r = compositor_.render(ctx_.Get(), target, core_srv, width_, height_, err);

    if (acquired) surfaces_[ready_index].keyed->ReleaseSync(0);
    if (r != RP_OK) return r;

    if (swapchain_) swapchain_->Present(1, 0);

    if (out_rgba) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width=width_; sd.Height=height_; sd.MipLevels=1; sd.ArraySize=1;
        sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count=1;
        sd.Usage=D3D11_USAGE_STAGING; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging))) { err="composite staging"; return RP_ERR_DEVICE; }
        ctx_->CopyResource(staging.Get(), offscreen_.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { err="composite map"; return RP_ERR_DEVICE; }
        for (uint32_t y=0; y<height_; ++y)
            memcpy(out_rgba + y*width_*4, (const uint8_t*)m.pData + y*m.RowPitch, width_*4);
        ctx_->Unmap(staging.Get(), 0);
    }
    return RP_OK;
}

rp_result D3D11Backend::composite_driven(const void*, uint32_t, uint32_t, uint32_t, bool, uint8_t*, std::string& err) {
    err = "composite_driven not implemented yet";
    return RP_ERR_UNSUPPORTED;
}
}
