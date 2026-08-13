#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <string>
#include <retropark/retropark_abi.h>

namespace rp {
class D3D11Compositor {
public:
    rp_result initialize(ID3D11Device* dev, std::string& err);
    rp_result render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target,
                     ID3D11ShaderResourceView* core_srv, uint32_t w, uint32_t h, std::string& err);
private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> fs_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  sample_ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};
}
