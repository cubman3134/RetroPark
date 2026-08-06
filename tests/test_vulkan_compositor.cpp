#include <doctest/doctest.h>
#include "render/vulkan/VulkanBackend.h"
#include "render/vulkan/VulkanCommon.h"
#include <vector>
#include <cstring>

using namespace rp;

// Vulkan analog of test_compositor.cpp (D3D11): a second VkDevice on the same
// physical device imports the host's exported image + timeline, clears the shared
// image green on the GPU, and signals the timeline to 2. The host then runs
// composite_and_present (QFOT-acquiring the core image, sampling it, drawing the
// blended overlay quad, reading the result back headless) and the test proves the
// overlay genuinely BLENDS rather than just layering opaquely.

namespace {

static const VkFormat kFmt = VK_FORMAT_R8G8B8A8_UNORM;
static const VkImageUsageFlags kUsage =
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

static VkImageCreateInfo shared_image_ci(uint32_t w, uint32_t h,
                                         VkExternalMemoryImageCreateInfo* emici) {
    emici->sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emici->pNext = nullptr;
    emici->handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = emici;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kFmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = kUsage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return ici;
}

static bool find_mem_type(VkPhysicalDevice phys, uint32_t bits,
                          VkMemoryPropertyFlags props, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) { out = i; return true; }
    return false;
}

// Producer: a second VkDevice on the same physical device imports the shared image
// memory + timeline, clears the image `color`, and signals the timeline to `signal`.
// Self-contained: owns its own instance/device. Mirrors
// vk_test_producer_clear in test_vulkan_handoff.cpp.
static bool vk_test_producer_clear(void* mem_handle, void* sem_handle,
                                   const uint8_t uuid[16], uint32_t w, uint32_t h,
                                   const float color[4], uint64_t signal, std::string& why) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    const char* instExts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 3; ici.ppEnabledExtensionNames = instExts;
#ifndef NDEBUG
    ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers;
#else
    (void)layers;
#endif
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { why = "producer vkCreateInstance"; return false; }

    bool ok = false;
    VkDevice dev = VK_NULL_HANDLE;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkSemaphore sem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;

    do {
        uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(inst, &n, devs.data());
        VkPhysicalDevice phys = VK_NULL_HANDLE; uint32_t qfam = 0;
        for (auto d : devs) {
            VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext = &idp;
            vkGetPhysicalDeviceProperties2(d, &p2);
            if (std::memcmp(idp.deviceUUID, uuid, 16) != 0) continue;
            uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn); vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
            for (uint32_t i = 0; i < qn; ++i)
                if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; phys = d; break; }
            if (phys) break;
        }
        if (!phys) { why = "producer: no physical device matched host UUID"; break; }

        VkPhysicalDeviceTimelineSemaphoreFeatures tsf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        tsf.timelineSemaphore = VK_TRUE;
        float pri = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam; qci.queueCount = 1; qci.pQueuePriorities = &pri;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &tsf;
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = (uint32_t)(sizeof(kRequiredDeviceExts) / sizeof(char*));
        dci.ppEnabledExtensionNames = kRequiredDeviceExts;
        if (vkCreateDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) { why = "producer vkCreateDevice"; break; }
        VkQueue q; vkGetDeviceQueue(dev, qfam, 0, &q);

        auto pfnImportSem = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(dev, "vkImportSemaphoreWin32HandleKHR"));
        if (!pfnImportSem) { why = "producer: load import fns"; break; }

        VkExternalMemoryImageCreateInfo emici{};
        VkImageCreateInfo imgci = shared_image_ci(w, h, &emici);
        if (vkCreateImage(dev, &imgci, nullptr, &img) != VK_SUCCESS) { why = "producer vkCreateImage"; break; }
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, img, &req);

        uint32_t typeIndex = 0;
        if (!find_mem_type(phys, req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex)) {
            why = "producer: no importable device-local memory type"; break;
        }

        VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dai.image = img;
        VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        imp.pNext = &dai;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        imp.handle = static_cast<HANDLE>(mem_handle);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &imp;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = typeIndex;
        if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS) { why = "producer import vkAllocateMemory"; break; }
        if (vkBindImageMemory(dev, img, mem, 0) != VK_SUCCESS) { why = "producer vkBindImageMemory"; break; }

        VkSemaphoreTypeCreateInfo stci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext = &stci;
        if (vkCreateSemaphore(dev, &sci, nullptr, &sem) != VK_SUCCESS) { why = "producer vkCreateSemaphore"; break; }
        VkImportSemaphoreWin32HandleInfoKHR isi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        isi.semaphore = sem;
        isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        isi.handle = static_cast<HANDLE>(sem_handle);
        if (pfnImportSem(dev, &isi) != VK_SUCCESS) { why = "producer vkImportSemaphoreWin32HandleKHR"; break; }

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = qfam;
        if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) { why = "producer vkCreateCommandPool"; break; }
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE; REQUIRE(vkAllocateCommandBuffers(dev, &cbai, &cb) == VK_SUCCESS);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);

        // Plain (non-QFOT) transition to GENERAL within this device before the clear.
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toGeneral.srcAccessMask = 0; toGeneral.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = img; toGeneral.subresourceRange = range;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toGeneral);

        VkClearColorValue cc{}; std::memcpy(cc.float32, color, sizeof(float) * 4);
        vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &range);

        // Release ownership to the external (host) queue family, GENERAL->GENERAL (no
        // layout change), so the host's QFOT acquire preserves the cleared contents.
        VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; release.dstAccessMask = 0;
        release.oldLayout = VK_IMAGE_LAYOUT_GENERAL; release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = qfam; release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL_KHR;
        release.image = img; release.subresourceRange = range;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &release);
        vkEndCommandBuffer(cb);

        uint64_t signalValue = signal;
        VkTimelineSemaphoreSubmitInfo tssi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tssi.signalSemaphoreValueCount = 1; tssi.pSignalSemaphoreValues = &signalValue;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.pNext = &tssi;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sem;
        if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) { why = "producer vkQueueSubmit"; break; }
        if (vkQueueWaitIdle(q) != VK_SUCCESS) { why = "producer vkQueueWaitIdle"; break; }
        ok = true;
    } while (false);

    if (dev) vkDeviceWaitIdle(dev);
    if (pool) vkDestroyCommandPool(dev, pool, nullptr);
    if (sem)  vkDestroySemaphore(dev, sem, nullptr);
    if (img)  vkDestroyImage(dev, img, nullptr);
    if (mem)  vkFreeMemory(dev, mem, nullptr);
    if (dev)  vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return ok;
}

} // namespace

TEST_CASE("vulkan compositor: core frame shows and overlay blends over it") {
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }

    const uint32_t W = 64, H = 64;
    VulkanBackend host; std::string err;
    REQUIRE(host.initialize(nullptr, W, H, err) == RP_OK);

    std::vector<rp_surface_desc> descs;
    REQUIRE(host.allocate_surfaces(1, W, H, descs, err) == RP_OK);
    REQUIRE(descs.size() == 1);
    REQUIRE(descs[0].shared_handle != nullptr);
    REQUIRE(host.present_sync_handle() != nullptr);

    uint8_t uuid[16]; host.present_device_uuid(uuid);

    // Producer clears the shared surface green and signals the timeline to 2.
    const float green[4] = {0, 1, 0, 1};
    std::string why;
    REQUIRE_MESSAGE(vk_test_producer_clear(descs[0].shared_handle, host.present_sync_handle(),
                                           uuid, W, H, green, /*signal=*/2, why), why);

    std::vector<uint8_t> img(W * H * 4, 0);
    REQUIRE(host.composite_and_present(/*ready_index=*/0, /*sync_value=*/2, /*has_frame=*/true,
                                       img.data(), err) == RP_OK);

    auto at = [&](uint32_t x, uint32_t y, int c) { return img[(y * W + x) * 4 + c]; };
    // Bottom-right quadrant: no overlay -> pure green.
    CHECK(at(60, 60, 1) > 200);            // G high
    CHECK(at(60, 60, 2) < 60);             // B low
    // Top-left quadrant: overlay blended over green -> blue raised, green reduced.
    CHECK(at(4, 4, 2) > 80);               // B raised by overlay
    CHECK(at(4, 4, 1) < at(60, 60, 1));    // G reduced vs the non-overlay region
}
