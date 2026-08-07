#pragma once
#include "render/vulkan/VulkanCommon.h"

namespace rp {

// Draws the core's shared image (fullscreen triangle, sampled) then a blended
// overlay quad (top-left quadrant) into a caller-supplied render target. Two
// graphics pipelines against a single R8G8B8A8_UNORM render pass; no swapchain
// or presentation knowledge lives here.
class VulkanCompositor {
public:
    rp_result initialize(VkDevice dev, VkFormat color_format, std::string& err);

    // Records into `cmd`: begins its own render pass against `targetView` (w x h,
    // cleared to black); if `coreView` is non-null, samples it via the fullscreen
    // triangle (in the layout given by `coreLayout` — GENERAL for the presenting
    // path's shared images, SHADER_READ_ONLY_OPTIMAL for a normal sampled image
    // like the driven-model upload target); then draws the blended overlay quad;
    // ends the render pass. Does not submit.
    rp_result render(VkCommandBuffer cmd, VkImageView targetView, VkImageView coreView,
                     uint32_t w, uint32_t h, std::string& err,
                     VkImageLayout coreLayout = VK_IMAGE_LAYOUT_GENERAL);

    // Reverse-order teardown of everything created in initialize(). Safe to call on
    // a partially-built or never-initialized compositor.
    void destroy();

private:
    VkDevice dev_ = VK_NULL_HANDLE;

    VkShaderModule fs_vert_ = VK_NULL_HANDLE;
    VkShaderModule sample_frag_ = VK_NULL_HANDLE;
    VkShaderModule ov_vert_ = VK_NULL_HANDLE;
    VkShaderModule ov_frag_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;   // updated to point at coreView each render()

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineLayout core_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout overlay_layout_ = VK_NULL_HANDLE;
    VkPipeline core_pipeline_ = VK_NULL_HANDLE;      // blend disabled
    VkPipeline overlay_pipeline_ = VK_NULL_HANDLE;   // src_alpha / one_minus_src_alpha

    // Cached framebuffer for the (targetView, w, h) most recently rendered; the
    // composite target is normally fixed-size, so this is recreated only when one
    // of those actually changes. render() only recreates it between fully-completed
    // submissions (the caller waits a fence before calling render() again), so it's
    // never destroyed while a pending command buffer still references it.
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkImageView fb_view_ = VK_NULL_HANDLE;
    uint32_t fb_w_ = 0, fb_h_ = 0;
};
}
