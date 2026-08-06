#pragma once
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <string>
#include <cstring>
#include <retropark/retropark_abi.h>

namespace rp {
#define VK_CHECK(expr, err, msg) do { if ((expr) != VK_SUCCESS) { (err) = (msg); return RP_ERR_DEVICE; } } while(0)

// Device extensions required for the presenting handoff.
inline const char* const kRequiredDeviceExts[] = {
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
};
}
