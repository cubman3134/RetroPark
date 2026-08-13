#include "render/d3d11/D3D11Compositor.h"
#include "render/d3d11/Shaders.h"
#include <d3dcompiler.h>
#include <cstring>
using Microsoft::WRL::ComPtr;

namespace rp {

static rp_result compile(const char* src, const char* target, ComPtr<ID3DBlob>& out, std::string& err) {
    ComPtr<ID3DBlob> errblob;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target,
                            0, 0, &out, &errblob);
    if (FAILED(hr)) { err = errblob ? (const char*)errblob->GetBufferPointer() : "compile failed"; return RP_ERR_DEVICE; }
    return RP_OK;
}

rp_result D3D11Compositor::initialize(ID3D11Device* dev, std::string& err) {
    ComPtr<ID3DBlob> b;
    rp_result r;
    if ((r = compile(kFullscreenVS, "vs_5_0", b, err)) != RP_OK) return r;
    if (FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fs_vs_))) { err="fsVS"; return RP_ERR_DEVICE; }
    if ((r = compile(kSamplePS, "ps_5_0", b, err)) != RP_OK) return r;
    if (FAILED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &sample_ps_))) { err="samplePS"; return RP_ERR_DEVICE; }

    D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(dev->CreateSamplerState(&sd, &sampler_))) { err="sampler"; return RP_ERR_DEVICE; }
    return RP_OK;
}

rp_result D3D11Compositor::render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target,
                                  ID3D11ShaderResourceView* core_srv, uint32_t w, uint32_t h, std::string& err) {
    (void)err;
    const float clear[4] = {0,0,0,1};
    ctx->ClearRenderTargetView(target, clear);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT vp{0,0,(float)w,(float)h,0,1}; ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);

    // Pass 1: core fullscreen (opaque) if we have a frame.
    if (core_srv) {
        float noBlend[4]={0,0,0,0};
        ctx->OMSetBlendState(nullptr, noBlend, 0xffffffff);
        ctx->VSSetShader(fs_vs_.Get(), nullptr, 0);
        ctx->PSSetShader(sample_ps_.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &core_srv);
        ID3D11SamplerState* s = sampler_.Get(); ctx->PSSetSamplers(0, 1, &s);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullsrv=nullptr; ctx->PSSetShaderResources(0,1,&nullsrv);
    }
    return RP_OK;
}
}
