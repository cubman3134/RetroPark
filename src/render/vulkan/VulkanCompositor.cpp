#include "render/vulkan/VulkanCompositor.h"
#include "vk_fullscreen_vert_generated.h"
#include "vk_sample_frag_generated.h"
#include "vk_overlay_vert_generated.h"
#include "vk_overlay_frag_generated.h"
#include <cstring>

namespace rp {

// Mirrors the `P { vec4 rect; vec4 color; }` push-constant block shared by
// overlay.vert/overlay.frag.
struct OverlayPush { float rect[4]; float color[4]; };

static rp_result make_shader(VkDevice dev, const unsigned int* code, unsigned long words,
                             VkShaderModule& out, std::string& err) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = static_cast<size_t>(words) * sizeof(uint32_t);
    ci.pCode = code;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &out), err, "vkCreateShaderModule");
    return RP_OK;
}

rp_result VulkanCompositor::initialize(VkDevice dev, VkFormat color_format, std::string& err) {
    dev_ = dev;
    rp_result r;
    if ((r = make_shader(dev_, vk_fullscreen_vert, vk_fullscreen_vert_len, fs_vert_, err)) != RP_OK) return r;
    if ((r = make_shader(dev_, vk_sample_frag, vk_sample_frag_len, sample_frag_, err)) != RP_OK) return r;
    if ((r = make_shader(dev_, vk_overlay_vert, vk_overlay_vert_len, ov_vert_, err)) != RP_OK) return r;
    if ((r = make_shader(dev_, vk_overlay_frag, vk_overlay_frag_len, ov_frag_, err)) != RP_OK) return r;

    // Descriptor set layout: one combined image sampler, sampled only in the frag stage.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1; dslci.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(dev_, &dslci, nullptr, &set_layout_), err, "descriptor set layout");

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(dev_, &sci, nullptr, &sampler_), err, "sampler");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(dev_, &dpci, nullptr, &desc_pool_), err, "descriptor pool");

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = desc_pool_; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &set_layout_;
    VK_CHECK(vkAllocateDescriptorSets(dev_, &dsai, &desc_set_), err, "descriptor set alloc");

    // Render pass: single color attachment, cleared every render() and stored so the
    // caller can copy it out afterward.
    VkAttachmentDescription attach{};
    attach.format = color_format;
    attach.samples = VK_SAMPLE_COUNT_1_BIT;
    attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attach.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colorRef;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1; rpci.pAttachments = &attach;
    rpci.subpassCount = 1; rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(dev_, &rpci, nullptr, &render_pass_), err, "render pass");

    // Pipeline layouts: the core pass samples the descriptor set; the overlay pass
    // only consumes push constants.
    VkPipelineLayoutCreateInfo coreLci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    coreLci.setLayoutCount = 1; coreLci.pSetLayouts = &set_layout_;
    VK_CHECK(vkCreatePipelineLayout(dev_, &coreLci, nullptr, &core_layout_), err, "core pipeline layout");

    VkPushConstantRange pcRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(OverlayPush)};
    VkPipelineLayoutCreateInfo ovLci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ovLci.pushConstantRangeCount = 1; ovLci.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev_, &ovLci, nullptr, &overlay_layout_), err, "overlay pipeline layout");

    // Shared fixed-function state: no vertex buffers (both shaders build vertices from
    // gl_VertexIndex), dynamic viewport/scissor (render() is called at arbitrary w x h).
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vpState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpState.viewportCount = 1; vpState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    // Core pipeline: fullscreen triangle sampling the core image, blend disabled.
    VkPipelineColorBlendAttachmentState noBlend{};
    noBlend.blendEnable = VK_FALSE;
    noBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo coreBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    coreBlend.attachmentCount = 1; coreBlend.pAttachments = &noBlend;

    VkPipelineInputAssemblyStateCreateInfo iaTri{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iaTri.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineShaderStageCreateInfo coreStages[2] = {
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
    };
    coreStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; coreStages[0].module = fs_vert_; coreStages[0].pName = "main";
    coreStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; coreStages[1].module = sample_frag_; coreStages[1].pName = "main";

    VkGraphicsPipelineCreateInfo coreGpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    coreGpci.stageCount = 2; coreGpci.pStages = coreStages;
    coreGpci.pVertexInputState = &vi;
    coreGpci.pInputAssemblyState = &iaTri;
    coreGpci.pViewportState = &vpState;
    coreGpci.pRasterizationState = &rs;
    coreGpci.pMultisampleState = &ms;
    coreGpci.pColorBlendState = &coreBlend;
    coreGpci.pDynamicState = &dyn;
    coreGpci.layout = core_layout_;
    coreGpci.renderPass = render_pass_;
    coreGpci.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &coreGpci, nullptr, &core_pipeline_),
             err, "core pipeline");

    // Overlay pipeline: top-left-quadrant quad (triangle strip), src_alpha/one_minus_src_alpha blend.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo ovBlendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    ovBlendState.attachmentCount = 1; ovBlendState.pAttachments = &blend;

    VkPipelineInputAssemblyStateCreateInfo iaStrip{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iaStrip.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineShaderStageCreateInfo ovStages[2] = {
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
    };
    ovStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; ovStages[0].module = ov_vert_; ovStages[0].pName = "main";
    ovStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ovStages[1].module = ov_frag_; ovStages[1].pName = "main";

    VkGraphicsPipelineCreateInfo ovGpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ovGpci.stageCount = 2; ovGpci.pStages = ovStages;
    ovGpci.pVertexInputState = &vi;
    ovGpci.pInputAssemblyState = &iaStrip;
    ovGpci.pViewportState = &vpState;
    ovGpci.pRasterizationState = &rs;
    ovGpci.pMultisampleState = &ms;
    ovGpci.pColorBlendState = &ovBlendState;
    ovGpci.pDynamicState = &dyn;
    ovGpci.layout = overlay_layout_;
    ovGpci.renderPass = render_pass_;
    ovGpci.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &ovGpci, nullptr, &overlay_pipeline_),
             err, "overlay pipeline");

    return RP_OK;
}

rp_result VulkanCompositor::render(VkCommandBuffer cmd, VkImageView targetView, VkImageView coreView,
                                   uint32_t w, uint32_t h, std::string& err) {
    if (!framebuffer_ || fb_view_ != targetView || fb_w_ != w || fb_h_ != h) {
        if (framebuffer_) { vkDestroyFramebuffer(dev_, framebuffer_, nullptr); framebuffer_ = VK_NULL_HANDLE; }
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = render_pass_;
        fci.attachmentCount = 1; fci.pAttachments = &targetView;
        fci.width = w; fci.height = h; fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(dev_, &fci, nullptr, &framebuffer_), err, "framebuffer");
        fb_view_ = targetView; fb_w_ = w; fb_h_ = h;
    }

    VkClearValue clear{};
    clear.color.float32[0] = 0.0f; clear.color.float32[1] = 0.0f;
    clear.color.float32[2] = 0.0f; clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = render_pass_; rbi.framebuffer = framebuffer_;
    rbi.renderArea.offset = {0, 0};
    rbi.renderArea.extent = {w, h};
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    if (coreView) {
        VkDescriptorImageInfo dii{sampler_, coreView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = desc_set_; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &dii;
        // Safe to update here: the caller (VulkanBackend::composite_and_present) waits
        // a fence for the previous submission before ever calling render() again, so
        // no in-flight command buffer references this descriptor set concurrently.
        vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, core_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, core_layout_, 0, 1, &desc_set_, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    OverlayPush push{};
    push.rect[0] = -1.0f; push.rect[1] = -1.0f; push.rect[2] = 0.0f; push.rect[3] = 0.0f;
    push.color[0] = 0.0f; push.color[1] = 0.0f; push.color[2] = 1.0f; push.color[3] = 0.5f;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_pipeline_);
    vkCmdPushConstants(cmd, overlay_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(cmd, 4, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
    return RP_OK;
}

void VulkanCompositor::destroy() {
    if (!dev_) return;
    if (framebuffer_) vkDestroyFramebuffer(dev_, framebuffer_, nullptr);
    if (overlay_pipeline_) vkDestroyPipeline(dev_, overlay_pipeline_, nullptr);
    if (core_pipeline_) vkDestroyPipeline(dev_, core_pipeline_, nullptr);
    if (overlay_layout_) vkDestroyPipelineLayout(dev_, overlay_layout_, nullptr);
    if (core_layout_) vkDestroyPipelineLayout(dev_, core_layout_, nullptr);
    if (render_pass_) vkDestroyRenderPass(dev_, render_pass_, nullptr);
    if (desc_pool_) vkDestroyDescriptorPool(dev_, desc_pool_, nullptr);   // frees desc_set_ too
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    if (ov_frag_) vkDestroyShaderModule(dev_, ov_frag_, nullptr);
    if (ov_vert_) vkDestroyShaderModule(dev_, ov_vert_, nullptr);
    if (sample_frag_) vkDestroyShaderModule(dev_, sample_frag_, nullptr);
    if (fs_vert_) vkDestroyShaderModule(dev_, fs_vert_, nullptr);

    framebuffer_ = VK_NULL_HANDLE;
    overlay_pipeline_ = VK_NULL_HANDLE; core_pipeline_ = VK_NULL_HANDLE;
    overlay_layout_ = VK_NULL_HANDLE; core_layout_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    desc_pool_ = VK_NULL_HANDLE; desc_set_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE; set_layout_ = VK_NULL_HANDLE;
    ov_frag_ = VK_NULL_HANDLE; ov_vert_ = VK_NULL_HANDLE;
    sample_frag_ = VK_NULL_HANDLE; fs_vert_ = VK_NULL_HANDLE;
    dev_ = VK_NULL_HANDLE;
}
}
