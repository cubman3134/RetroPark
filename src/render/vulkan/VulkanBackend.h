#pragma once
#include "render/IRenderBackend.h"
#include "render/vulkan/VulkanCommon.h"
#include <vector>

namespace rp {
class VulkanBackend : public IRenderBackend {
public:
    ~VulkanBackend() override;
    rp_result initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) override;
    rp_result allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                std::vector<rp_surface_desc>& out, std::string& err) override;
    rp_result composite_and_present(uint32_t ready_index, uint64_t sync_value, bool has_frame,
                                    uint8_t* out_rgba, std::string& err) override;
    void  present_device_uuid(uint8_t out[16]) const override { std::memcpy(out, device_uuid_, 16); }
    void* present_sync_handle() const override { return sync_handle_; }

    static bool probe_vulkan_shared();

protected:
    rp_result create_instance_and_device(std::string& err);
    void destroy_surfaces();         // free/close a surface batch (reverse order)

    // One exported shared image slot: the VkImage lives on device_, its backing
    // VkDeviceMemory is exported as an opaque-Win32 NT handle that another
    // VkDevice (or D3D) can import. `handle` is owned and CloseHandle'd once.
    struct VkSurface {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory mem    = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
        void*          handle = nullptr;   // exported NT handle (owned)
    };

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint8_t device_uuid_[16] = {0};
    std::vector<VkSurface> surfaces_;        // exported shared image ring
    VkSemaphore timeline_ = VK_NULL_HANDLE;  // exported shared timeline semaphore
    void* sync_handle_ = nullptr;            // exported NT handle for timeline_ (owned)
    uint32_t width_ = 0, height_ = 0;
};
}
