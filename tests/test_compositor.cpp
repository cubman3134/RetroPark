#include <doctest/doctest.h>
#include "render/d3d11/D3D11Backend.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace rp;

// Fill the single core surface with solid green from a producer device, then
// composite; assert green shows outside the overlay and a blue-ward blend inside it.
TEST_CASE("compositor: core frame shows and overlay blends over it") {
    if (!D3D11Backend::probe_shared_keyed_mutex()) { WARN("no shared keyed mutex; skip"); return; }
    const uint32_t W=64, H=64;
    D3D11Backend host; std::string err;
    REQUIRE(host.initialize(nullptr, W, H, err) == RP_OK);
    std::vector<rp_surface_desc> descs;
    REQUIRE(host.allocate_surfaces(1, W, H, descs, err) == RP_OK);

    // Producer clears the shared surface green (key 0 -> release key 1).
    ComPtr<ID3D11Device> pdev; ComPtr<ID3D11DeviceContext> pctx; D3D_FEATURE_LEVEL fl;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &pdev, &fl, &pctx)));
    ComPtr<ID3D11Device1> pdev1; pdev.As(&pdev1);
    ComPtr<ID3D11Texture2D> ptex; REQUIRE(SUCCEEDED(pdev1->OpenSharedResource1(descs[0].shared_handle, IID_PPV_ARGS(&ptex))));
    ComPtr<IDXGIKeyedMutex> pkm; ptex.As(&pkm);
    REQUIRE(SUCCEEDED(pkm->AcquireSync(0, 100)));
    ComPtr<ID3D11RenderTargetView> prtv; REQUIRE(SUCCEEDED(pdev->CreateRenderTargetView(ptex.Get(), nullptr, &prtv)));
    const float green[4]={0,1,0,1}; pctx->ClearRenderTargetView(prtv.Get(), green); pctx->Flush();
    REQUIRE(SUCCEEDED(pkm->ReleaseSync(1)));

    std::vector<uint8_t> img(W*H*4, 0);
    REQUIRE(host.composite_and_present(/*ready_index=*/0, /*sync_value=*/0, /*has_frame=*/true, img.data(), err) == RP_OK);

    auto at = [&](uint32_t x, uint32_t y, int c){ return img[(y*W + x)*4 + c]; };
    // Bottom-right quadrant: no overlay -> pure green.
    CHECK(at(60, 60, 1) > 200);            // G high
    CHECK(at(60, 60, 2) < 60);             // B low
    // Top-left quadrant: overlay blended over green -> blue raised, green reduced.
    CHECK(at(4, 4, 2) > 80);               // B raised by overlay
    CHECK(at(4, 4, 1) < at(60, 60, 1));    // G reduced vs the non-overlay region
}
