#include "render/d3d11/D3D11Compositor.h"
#include "render/d3d11/Shaders.h"
#include <d3dcompiler.h>
#include <cstring>
using Microsoft::WRL::ComPtr;

namespace rp {

struct OverlayCB { float rect[4]; float color[4]; };

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
    if ((r = compile(kOverlayVS, "vs_5_0", b, err)) != RP_OK) return r;
    if (FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ov_vs_))) { err="ovVS"; return RP_ERR_DEVICE; }
    if ((r = compile(kOverlayPS, "ps_5_0", b, err)) != RP_OK) return r;
    if (FAILED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ov_ps_))) { err="ovPS"; return RP_ERR_DEVICE; }

    D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(dev->CreateSamplerState(&sd, &sampler_))) { err="sampler"; return RP_ERR_DEVICE; }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &blend_))) { err="blend"; return RP_ERR_DEVICE; }

    D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof(OverlayCB); cb.Usage=D3D11_USAGE_DEFAULT;
    cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&cb, nullptr, &ov_cb_))) { err="cb"; return RP_ERR_DEVICE; }
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

    // Pass 2: blended overlay quad in the top-left quadrant, alpha 0.5 toward blue.
    OverlayCB data{};
    data.rect[0]=-1.0f; data.rect[1]=1.0f; data.rect[2]=0.0f; data.rect[3]=0.0f; // NDC top-left quadrant
    data.color[0]=0.0f; data.color[1]=0.0f; data.color[2]=1.0f; data.color[3]=0.5f;
    ctx->UpdateSubresource(ov_cb_.Get(), 0, nullptr, &data, 0, 0);
    float bf[4]={0,0,0,0};
    ctx->OMSetBlendState(blend_.Get(), bf, 0xffffffff);
    ctx->VSSetShader(ov_vs_.Get(), nullptr, 0);
    ID3D11Buffer* cb = ov_cb_.Get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShader(ov_ps_.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->Draw(4, 0);
    return RP_OK;
}
}
