#include <doctest/doctest.h>
#include "render/d3d11/D3D11Backend.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace rp;

// Producer writes a solid color into a shared texture; consumer reads it back.
TEST_CASE("d3d11: cross-device shared keyed-mutex handoff") {
    if (!D3D11Backend::probe_shared_keyed_mutex()) {
        WARN("environment lacks shared keyed-mutex support; skipping");
        return;
    }
    D3D11Backend host;               // consumer (WARP, headless)
    std::string err;
    REQUIRE(host.initialize(nullptr, 8, 8, err) == RP_OK);

    std::vector<rp_surface_desc> descs;
    REQUIRE(host.allocate_surfaces(1, 8, 8, descs, err) == RP_OK);
    REQUIRE(descs.size() == 1);
    REQUIRE(descs[0].shared_handle != nullptr);

    // Producer: a second WARP device opens the shared handle and clears it red.
    ComPtr<ID3D11Device> pdev; ComPtr<ID3D11DeviceContext> pctx;
    D3D_FEATURE_LEVEL fl;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &pdev, &fl, &pctx)));
    ComPtr<ID3D11Device1> pdev1; pdev.As(&pdev1);
    ComPtr<ID3D11Texture2D> ptex;
    REQUIRE(SUCCEEDED(pdev1->OpenSharedResource1(descs[0].shared_handle, IID_PPV_ARGS(&ptex))));
    ComPtr<IDXGIKeyedMutex> pkm; ptex.As(&pkm);
    REQUIRE(SUCCEEDED(pkm->AcquireSync(0, 100)));   // producer takes key 0
    ComPtr<ID3D11RenderTargetView> prtv;
    REQUIRE(SUCCEEDED(pdev->CreateRenderTargetView(ptex.Get(), nullptr, &prtv)));
    const float red[4] = {1,0,0,1};
    pctx->ClearRenderTargetView(prtv.Get(), red);
    pctx->Flush();
    REQUIRE(SUCCEEDED(pkm->ReleaseSync(1)));         // hand to consumer with key 1

    // Consumer reads back pixel (0,0) and expects red.
    uint8_t rgba[4] = {0,0,0,0};
    REQUIRE(host.readback_surface_pixel(descs[0].index, 0, 0, rgba, err) == RP_OK);
    CHECK(rgba[0] == 255);
    CHECK(rgba[1] == 0);
    CHECK(rgba[2] == 0);
}

// Re-allocating surfaces on the same backend must not leak or double-close the
// underlying NT shared handles: allocate_surfaces() clears surfaces_ and rebuilds
// it, so the previous batch's Surface objects are destroyed (closing their
// handles) while the new batch is moved in cleanly.
TEST_CASE("d3d11: allocate_surfaces twice on the same backend does not crash") {
    if (!D3D11Backend::probe_shared_keyed_mutex()) {
        WARN("environment lacks shared keyed-mutex support; skipping");
        return;
    }
    D3D11Backend host;
    std::string err;
    REQUIRE(host.initialize(nullptr, 8, 8, err) == RP_OK);

    std::vector<rp_surface_desc> descs;
    REQUIRE(host.allocate_surfaces(2, 8, 8, descs, err) == RP_OK);
    REQUIRE(descs.size() == 2);
    CHECK(descs[0].shared_handle != nullptr);
    CHECK(descs[1].shared_handle != nullptr);

    // Re-allocate: this exercises surfaces_.clear() destroying the first batch's
    // Surface objects (closing their handles exactly once) and then moving the
    // freshly created second batch into surfaces_.
    REQUIRE(host.allocate_surfaces(2, 8, 8, descs, err) == RP_OK);
    REQUIRE(descs.size() == 2);
    CHECK(descs[0].shared_handle != nullptr);
    CHECK(descs[1].shared_handle != nullptr);
}
