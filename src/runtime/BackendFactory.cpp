#include "runtime/BackendFactory.h"
#include "render/d3d11/D3D11Backend.h"
#include "render/vulkan/VulkanBackend.h"
#include "render/gl/GLBackend.h"
namespace rp {
std::unique_ptr<IRenderBackend> make_backend(rp_graphics_api api) {
    switch (api) {
        case RP_GFX_D3D11:  return std::make_unique<D3D11Backend>();
        case RP_GFX_VULKAN: return std::make_unique<VulkanBackend>();
        case RP_GFX_OPENGL: return std::make_unique<GLBackend>();
        default: return nullptr;
    }
}
}
