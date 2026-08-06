#include <doctest/doctest.h>
#include "render/vulkan/VulkanBackend.h"
#include "render/vulkan/VulkanCommon.h"   // vulkan.h (+ VK_USE_PLATFORM_WIN32_KHR), kRequiredDeviceExts
#include <vector>
#include <cstring>

using namespace rp;

// The handoff is the Vulkan analog of test_d3d11_handoff: a *second* VkDevice on
// the SAME physical device imports the host's exported image + timeline, clears the
// image red on the GPU, signals the timeline to 2, and the host reads the pixel
// back cross-device. Everything is finite-timeout and validation-layer clean.

namespace {

// Both image objects (host's exported one and the producer's imported one) MUST
// share identical create info for opaque-Win32 sharing to be valid.
static const VkFormat kFmt = VK_FORMAT_R8G8B8A8_UNORM;
static const VkImageUsageFlags kUsage =
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
static const uint64_t kOneSecondNs = 1000000000ull;

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

// Test subclass: reach the protected device/queue/surface/timeline handles so the
// host-side readback can be driven from the test without growing the public API.
struct TestVk : public VulkanBackend {
    VkDevice         dev()      const { return device_; }
    VkPhysicalDevice phys()     const { return phys_; }
    VkQueue          queue()    const { return queue_; }
    uint32_t         qfam()     const { return queue_family_; }
    VkImage          image(size_t i) const { return surfaces_[i].image; }
    VkSemaphore      timeline() const { return timeline_; }
};

// Producer: a second VkDevice on the same physical device imports the shared image
// memory + timeline, clears the image `color`, and signals the timeline to `signal`.
// Fully self-contained: creates and tears down its own instance/device. The host's
// exported memory keeps the underlying allocation alive after the producer exits.
bool vk_test_producer_clear(void* mem_handle, void* sem_handle,
                            const uint8_t uuid[16], uint32_t w, uint32_t h,
                            const float color[4], uint64_t signal, std::string& why) {
    // --- instance ---
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
        // --- pick the physical device matching the host UUID ---
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

        // --- device (same required extensions + timeline feature) ---
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

        // --- recreate the identical image and import the shared memory into it ---
        VkExternalMemoryImageCreateInfo emici{};
        VkImageCreateInfo imgci = shared_image_ci(w, h, &emici);
        if (vkCreateImage(dev, &imgci, nullptr, &img) != VK_SUCCESS) { why = "producer vkCreateImage"; break; }
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, img, &req);

        // Opaque-Win32 handles carry their own memory-type info (querying handle
        // properties is disallowed for opaque types), and this is a dedicated import
        // of an identical image on the same physical device, so the exporter's
        // device-local requirements apply directly.
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
        imp.handle = static_cast<HANDLE>(mem_handle);   // host retains ownership of the NT handle
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &imp;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = typeIndex;
        if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS) { why = "producer import vkAllocateMemory"; break; }
        if (vkBindImageMemory(dev, img, mem, 0) != VK_SUCCESS) { why = "producer vkBindImageMemory"; break; }

        // --- import the shared timeline semaphore ---
        VkSemaphoreTypeCreateInfo stci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext = &stci;
        if (vkCreateSemaphore(dev, &sci, nullptr, &sem) != VK_SUCCESS) { why = "producer vkCreateSemaphore"; break; }
        VkImportSemaphoreWin32HandleInfoKHR isi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        isi.semaphore = sem;
        isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        isi.handle = static_cast<HANDLE>(sem_handle);   // host retains ownership of the NT handle
        if (pfnImportSem(dev, &isi) != VK_SUCCESS) { why = "producer vkImportSemaphoreWin32HandleKHR"; break; }

        // --- record: UNDEFINED->GENERAL, clear red, release to EXTERNAL as GENERAL ---
        // The shared image lives in GENERAL for its whole life so the QFOT carries no
        // layout change (release and acquire halves must specify identical layouts, and
        // validation can't catch a mismatch that straddles two VkDevices).
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

        // Release ownership to the external (host) queue family with NO layout change
        // (GENERAL->GENERAL); the host's acquire half specifies the identical layouts so
        // the QFOT is spec-correct and the cleared contents are preserved.
        VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; release.dstAccessMask = 0;
        release.oldLayout = VK_IMAGE_LAYOUT_GENERAL; release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = qfam; release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL_KHR;
        release.image = img; release.subresourceRange = range;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &release);
        vkEndCommandBuffer(cb);

        // --- submit, signalling the timeline to `signal` ---
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
    if (mem)  vkFreeMemory(dev, mem, nullptr);   // releases the imported reference; NT handle stays with host
    if (dev)  vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return ok;
}

// Host-side readback: wait the timeline >= wait_value (1s CPU timeout), then acquire
// the image from the EXTERNAL family (GENERAL->GENERAL, no layout change) and copy
// pixel (x,y) out through a host-visible staging buffer. Returns the RGBA bytes in `rgba`.
bool vk_host_readback(TestVk& host, uint32_t w, uint32_t h, uint64_t wait_value,
                      uint8_t rgba[4], std::string& why) {
    VkDevice dev = host.dev();
    VkSemaphore tl = host.timeline();

    // Bounded CPU wait on the shared timeline.
    uint64_t waitValue = wait_value;
    VkSemaphoreWaitInfo swi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    swi.semaphoreCount = 1; swi.pSemaphores = &tl; swi.pValues = &waitValue;
    if (vkWaitSemaphores(dev, &swi, kOneSecondNs) != VK_SUCCESS) { why = "host vkWaitSemaphores timed out"; return false; }

    bool ok = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;

    do {
        // host-visible/coherent staging buffer for the copy target
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev, &bci, nullptr, &staging) != VK_SUCCESS) { why = "host vkCreateBuffer"; break; }
        VkMemoryRequirements breq; vkGetBufferMemoryRequirements(dev, staging, &breq);
        uint32_t bt = 0;
        if (!find_mem_type(host.phys(), breq.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, bt)) {
            why = "host: no host-visible memory type"; break;
        }
        VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bmai.allocationSize = breq.size; bmai.memoryTypeIndex = bt;
        if (vkAllocateMemory(dev, &bmai, nullptr, &stagingMem) != VK_SUCCESS) { why = "host staging vkAllocateMemory"; break; }
        REQUIRE(vkBindBufferMemory(dev, staging, stagingMem, 0) == VK_SUCCESS);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = host.qfam();
        if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) { why = "host vkCreateCommandPool"; break; }
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE; REQUIRE(vkAllocateCommandBuffers(dev, &cbai, &cb) == VK_SUCCESS);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);

        // Acquire from the EXTERNAL family with NO layout change (GENERAL->GENERAL),
        // identical to the producer's release half; the copy then reads the image as
        // GENERAL, which vkCmdCopyImageToBuffer accepts.
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.srcAccessMask = 0; acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL; acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL_KHR; acquire.dstQueueFamilyIndex = host.qfam();
        acquire.image = host.image(0); acquire.subresourceRange = range;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &acquire);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, host.image(0), VK_IMAGE_LAYOUT_GENERAL, staging, 1, &region);
        vkEndCommandBuffer(cb);

        // Queue-wait on the timeline too: pairs the QFOT acquire and establishes the
        // cross-queue memory dependency for the producer's writes.
        uint64_t qWaitValue = wait_value;
        VkTimelineSemaphoreSubmitInfo tssi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tssi.waitSemaphoreValueCount = 1; tssi.pWaitSemaphoreValues = &qWaitValue;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSemaphore waitSem = tl;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.pNext = &tssi;
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &waitSem; si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) { why = "host vkCreateFence"; break; }
        if (vkQueueSubmit(host.queue(), 1, &si, fence) != VK_SUCCESS) { why = "host vkQueueSubmit"; break; }
        if (vkWaitForFences(dev, 1, &fence, VK_TRUE, kOneSecondNs) != VK_SUCCESS) { why = "host vkWaitForFences timed out"; break; }

        void* mapped = nullptr;
        if (vkMapMemory(dev, stagingMem, 0, bytes, 0, &mapped) != VK_SUCCESS) { why = "host vkMapMemory"; break; }
        std::memcpy(rgba, mapped, 4);   // pixel (0,0) is the first texel, tightly packed
        vkUnmapMemory(dev, stagingMem);
        ok = true;
    } while (false);

    if (dev) vkDeviceWaitIdle(dev);
    if (fence) vkDestroyFence(dev, fence, nullptr);
    if (pool) vkDestroyCommandPool(dev, pool, nullptr);
    if (staging) vkDestroyBuffer(dev, staging, nullptr);
    if (stagingMem) vkFreeMemory(dev, stagingMem, nullptr);
    return ok;
}

} // namespace

TEST_CASE("vulkan: cross-device exported-image + timeline handoff reads back red") {
    if (!VulkanBackend::probe_vulkan_shared()) {
        WARN("no capable Vulkan device; skipping");
        return;
    }
    TestVk host; std::string err;
    REQUIRE(host.initialize(nullptr, 8, 8, err) == RP_OK);

    std::vector<rp_surface_desc> descs;
    REQUIRE(host.allocate_surfaces(1, 8, 8, descs, err) == RP_OK);
    REQUIRE(descs.size() == 1);
    REQUIRE(descs[0].shared_handle != nullptr);
    REQUIRE(host.present_sync_handle() != nullptr);
    REQUIRE(descs[0].format == RP_FMT_R8G8B8A8_UNORM);

    uint8_t uuid[16]; host.present_device_uuid(uuid);

    // Producer (second device, same GPU) clears red and signals the timeline to 2.
    const float red[4] = {1, 0, 0, 1};
    std::string why;
    REQUIRE_MESSAGE(vk_test_producer_clear(descs[0].shared_handle, host.present_sync_handle(),
                                           uuid, 8, 8, red, /*signal=*/2, why), why);

    // Host waits T>=2 then reads back pixel (0,0) cross-device.
    uint8_t rgba[4] = {0, 0, 0, 0};
    REQUIRE_MESSAGE(vk_host_readback(host, 8, 8, /*wait_value=*/2, rgba, why), why);
    CHECK(rgba[0] == 255);
    CHECK(rgba[1] == 0);
    CHECK(rgba[2] == 0);
    CHECK(rgba[3] == 255);
}

// Re-allocating surfaces on the same backend must free/close the prior batch's NT
// handles exactly once (mirrors the D3D11 double-close guard).
TEST_CASE("vulkan: allocate_surfaces twice on the same backend does not leak or double-close") {
    if (!VulkanBackend::probe_vulkan_shared()) {
        WARN("no capable Vulkan device; skipping");
        return;
    }
    VulkanBackend host; std::string err;
    REQUIRE(host.initialize(nullptr, 8, 8, err) == RP_OK);

    std::vector<rp_surface_desc> descs;
    REQUIRE(host.allocate_surfaces(2, 8, 8, descs, err) == RP_OK);
    REQUIRE(descs.size() == 2);
    CHECK(descs[0].shared_handle != nullptr);
    CHECK(descs[1].shared_handle != nullptr);
    void* firstSync = host.present_sync_handle();
    CHECK(firstSync != nullptr);

    REQUIRE(host.allocate_surfaces(2, 8, 8, descs, err) == RP_OK);
    REQUIRE(descs.size() == 2);
    CHECK(descs[0].shared_handle != nullptr);
    CHECK(descs[1].shared_handle != nullptr);
    CHECK(host.present_sync_handle() != nullptr);
}
