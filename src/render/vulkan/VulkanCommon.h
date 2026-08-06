#pragma once
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <string>
#include <cstring>
#include <retropark/retropark_abi.h>

namespace rp {
#define VK_CHECK(expr, err, msg) do { if ((expr) != VK_SUCCESS) { (err) = (msg); return RP_ERR_DEVICE; } } while(0)

// Device extensions required for the presenting handoff. The windowed present
// path additionally needs VK_KHR_swapchain, which VulkanBackend appends to its own
// device-extension list (it isn't put here because handoff-only consumers of this
// list create surfaceless instances that couldn't satisfy swapchain's dependency).
inline const char* const kRequiredDeviceExts[] = {
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
};
}
