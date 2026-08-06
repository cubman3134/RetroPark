// Reference Vulkan presenting core (refcore_present_vk).
//
// The Vulkan analog of refcore_present (D3D11): a self-contained presenting core
// that creates its OWN VkDevice on the physical device matching the host's UUID,
// imports the host's exported shared images + shared timeline semaphore, and runs
// the PRODUCER half of the timeline handoff protocol on its own render thread.
//
// Import setup (image memory dedicated-import, timeline import, GENERAL-throughout
// layout, GENERAL->GENERAL QFOT release to the external family) mirrors the proven
// Task 4 producer in tests/test_vulkan_handoff.cpp exactly; the timeline wait/signal
// VALUES implement §3's producer contract: frame f signals T = 2*f and waits
// T >= 2*(f-N)+1 (skipped while f <= N).

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <retropark/retropark_abi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {

constexpr uint64_t kOneSecondNs = 1000000000ull;

// Instance / device extensions — identical to the host backend (VulkanCommon.h /
// VulkanBackend::create_instance_and_device).
const char* const kInstanceExts[] = {
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
};
const char* const kDeviceExts[] = {
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
};

// The shared image create-info MUST match the host's byte-for-byte for opaque-Win32
// sharing to be valid (same format/extent/usage/tiling/sharing + external pNext).
constexpr VkFormat kFmt = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags kUsage =
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

bool find_mem_type(VkPhysicalDevice phys, uint32_t type_bits,
                   VkMemoryPropertyFlags props, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            out = i;
            return true;
        }
    return false;
}

// One imported shared image slot on the core's own device. The VkImage + backing
// VkDeviceMemory are OURS to destroy; the NT handle is the HOST's (imported, never
// closed here). One command buffer + fence per slot lets the loop re-record a slot
// only after its previous GPU submission has retired.
struct Slot {
    VkImage         image = VK_NULL_HANDLE;
    VkDeviceMemory  mem   = VK_NULL_HANDLE;
    VkCommandBuffer cmd   = VK_NULL_HANDLE;   // owned via the shared pool
    VkFence         fence = VK_NULL_HANDLE;
    bool            submitted = false;
    uint32_t        width = 0, height = 0;
    uint32_t        index = 0;
    uint64_t        generation = 0;
};

struct RefCoreVk {
    rp_host_iface host{};

    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    uint32_t         qfam     = 0;
    VkQueue          queue    = VK_NULL_HANDLE;
    VkCommandPool    pool     = VK_NULL_HANDLE;
    VkSemaphore      timeline = VK_NULL_HANDLE;   // imported; NOT owned (host's handle)

    std::vector<Slot> slots;

    std::thread th;
    std::atomic<bool> running{false};

    // Reverse-order teardown of everything set_surfaces built on `device`. Null-safe
    // and idempotent. NEVER closes the imported image/timeline NT handles — those are
    // the host's to close.
    void destroy_device() {
        if (device) vkDeviceWaitIdle(device);
        for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
            if (it->fence) vkDestroyFence(device, it->fence, nullptr);
            if (it->image) vkDestroyImage(device, it->image, nullptr);
            if (it->mem)   vkFreeMemory(device, it->mem, nullptr);   // releases our imported ref; host keeps the handle
        }
        slots.clear();
        if (timeline) { vkDestroySemaphore(device, timeline, nullptr); timeline = VK_NULL_HANDLE; }
        if (pool)     { vkDestroyCommandPool(device, pool, nullptr);   pool = VK_NULL_HANDLE; }
        if (device)   { vkDestroyDevice(device, nullptr);             device = VK_NULL_HANDLE; }
        phys = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
    }

    // Producer render loop. Frame f = 1,2,3,..., slot i = f % count:
    //   submit WAITS  T >= 2*(f-N)+1   (skipped while f <= N)
    //   submit SIGNALS T  = 2*f
    //   host.submit_frame(host, i, generation, 2*f)
    // Animate: clear color (0, 1, t, 1) with t rising over time (green, rising blue).
    void loop() {
        const uint32_t count = static_cast<uint32_t>(slots.size());
        uint64_t f = 0;
        while (running.load()) {
            if (count > 0) {
                f += 1;
                const uint32_t i = static_cast<uint32_t>(f % count);
                Slot& s = slots[i];

                // Re-recording a slot's command buffer requires its previous GPU
                // submission to have retired (finite CPU wait; steady-state this is
                // already satisfied, since the host consumed slot i N frames ago).
                if (s.submitted) {
                    if (vkWaitForFences(device, 1, &s.fence, VK_TRUE, kOneSecondNs) != VK_SUCCESS)
                        break;   // host stalled: bail cleanly rather than spin
                    vkResetFences(device, 1, &s.fence);
                }
                vkResetCommandBuffer(s.cmd, 0);

                const float t = static_cast<float>(f % 120) / 120.0f;   // rising blue channel
                const float color[4] = {0.0f, 1.0f, t, 1.0f};
                record_frame(s, color);

                const uint64_t signalVal = 2 * f;
                const uint64_t waitVal   = (f > count) ? (2 * (f - count) + 1) : 0;
                const bool     doWait    = (f > count);

                VkTimelineSemaphoreSubmitInfo tssi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
                if (doWait) { tssi.waitSemaphoreValueCount = 1; tssi.pWaitSemaphoreValues = &waitVal; }
                tssi.signalSemaphoreValueCount = 1; tssi.pSignalSemaphoreValues = &signalVal;

                VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.pNext = &tssi;
                si.commandBufferCount = 1; si.pCommandBuffers = &s.cmd;
                if (doWait) {
                    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &timeline; si.pWaitDstStageMask = &waitStage;
                }
                si.signalSemaphoreCount = 1; si.pSignalSemaphores = &timeline;

                if (vkQueueSubmit(queue, 1, &si, s.fence) != VK_SUCCESS)
                    break;
                s.submitted = true;

                if (host.submit_frame)
                    host.submit_frame(host.host, s.index, s.generation, signalVal);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    // Record slot `s`'s command buffer: plain UNDEFINED->GENERAL (contents discarded;
    // we clear the whole image), clear in GENERAL, then a GENERAL->GENERAL QFOT RELEASE
    // to the external (host) queue family. No layout change ever touches the shared
    // image — it lives in GENERAL its whole life (the QFOT bug fixed in Task 4).
    void record_frame(Slot& s, const float color[4]) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.cmd, &bi);

        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toGeneral.srcAccessMask = 0;
        toGeneral.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = s.image;
        toGeneral.subresourceRange = range;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toGeneral);

        VkClearColorValue cc{};
        std::memcpy(cc.float32, color, sizeof(float) * 4);
        vkCmdClearColorImage(s.cmd, s.image, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &range);

        VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        release.dstAccessMask = 0;
        release.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = qfam;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL_KHR;
        release.image = s.image;
        release.subresourceRange = range;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &release);

        vkEndCommandBuffer(s.cmd);
    }
};

// --- device creation + imports (the body of set_surfaces) --------------------

rp_result pick_device_by_uuid(VkInstance inst, const uint8_t uuid[16],
                              VkPhysicalDevice& outPhys, uint32_t& outQfam) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    for (auto d : devs) {
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &idp;
        vkGetPhysicalDeviceProperties2(d, &p2);
        if (std::memcmp(idp.deviceUUID, uuid, 16) != 0) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        for (uint32_t i = 0; i < qn; ++i)
            if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                outPhys = d;
                outQfam = i;
                return RP_OK;
            }
    }
    return RP_ERR_DEVICE;
}

rp_result build_from_surfaces(RefCoreVk* c, const rp_surface_set* set) {
    if (pick_device_by_uuid(c->instance, set->device_uuid, c->phys, c->qfam) != RP_OK)
        return RP_ERR_DEVICE;

    // --- device + queue (timeline feature, external-memory/semaphore-win32 exts) ---
    VkPhysicalDeviceTimelineSemaphoreFeatures tsf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    tsf.timelineSemaphore = VK_TRUE;
    float pri = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = c->qfam; qci.queueCount = 1; qci.pQueuePriorities = &pri;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &tsf;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(sizeof(kDeviceExts) / sizeof(char*));
    dci.ppEnabledExtensionNames = kDeviceExts;
    if (vkCreateDevice(c->phys, &dci, nullptr, &c->device) != VK_SUCCESS) return RP_ERR_DEVICE;
    vkGetDeviceQueue(c->device, c->qfam, 0, &c->queue);

    auto pfnImportSem = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
        vkGetDeviceProcAddr(c->device, "vkImportSemaphoreWin32HandleKHR"));
    if (!pfnImportSem) return RP_ERR_DEVICE;

    // --- one command pool for all per-slot command buffers ---
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = c->qfam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(c->device, &pci, nullptr, &c->pool) != VK_SUCCESS) return RP_ERR_DEVICE;

    // --- import each surface's memory into an identical VkImage + per-slot cmd/fence ---
    c->slots.reserve(set->count);
    for (uint32_t i = 0; i < set->count; ++i) {
        const rp_surface_desc& d = set->surfaces[i];
        c->slots.push_back(Slot{});
        Slot& s = c->slots.back();
        s.index = d.index;
        s.generation = d.generation;
        s.width = d.width;
        s.height = d.height;

        VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        emici.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &emici;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = kFmt;
        ici.extent = {d.width, d.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = kUsage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(c->device, &ici, nullptr, &s.image) != VK_SUCCESS) return RP_ERR_DEVICE;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(c->device, s.image, &req);
        // Opaque-Win32 handles carry their own memory-type info (querying handle
        // properties is disallowed for opaque types); this is a dedicated import of an
        // identical image on the same physical device, so the exporter's device-local
        // requirements apply directly.
        uint32_t typeIndex = 0;
        if (!find_mem_type(c->phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex))
            return RP_ERR_DEVICE;

        VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dai.image = s.image;
        VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        imp.pNext = &dai;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        imp.handle = static_cast<HANDLE>(d.shared_handle);   // host retains ownership of the NT handle
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &imp;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = typeIndex;
        if (vkAllocateMemory(c->device, &mai, nullptr, &s.mem) != VK_SUCCESS) return RP_ERR_DEVICE;
        if (vkBindImageMemory(c->device, s.image, s.mem, 0) != VK_SUCCESS) return RP_ERR_DEVICE;

        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = c->pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(c->device, &cbai, &s.cmd) != VK_SUCCESS) return RP_ERR_DEVICE;

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(c->device, &fci, nullptr, &s.fence) != VK_SUCCESS) return RP_ERR_DEVICE;
    }

    // --- import the shared timeline semaphore (init value comes from the host) ---
    VkSemaphoreTypeCreateInfo stci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &stci;
    if (vkCreateSemaphore(c->device, &sci, nullptr, &c->timeline) != VK_SUCCESS) return RP_ERR_DEVICE;
    VkImportSemaphoreWin32HandleInfoKHR isi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    isi.semaphore = c->timeline;
    isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    isi.handle = static_cast<HANDLE>(set->sync_handle);   // host retains ownership of the NT handle
    if (pfnImportSem(c->device, &isi) != VK_SUCCESS) return RP_ERR_DEVICE;

    return RP_OK;
}

// --- ABI entry points --------------------------------------------------------

void ref_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_PRESENTING;
    out->graphics_api = RP_GFX_VULKAN;
    out->id = "refcore_present_vk";
}

rp_core* ref_create(const rp_host_iface* host) {
    auto* c = new RefCoreVk();
    c->host = *host;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(sizeof(kInstanceExts) / sizeof(char*));
    ici.ppEnabledExtensionNames = kInstanceExts;
#ifndef NDEBUG
    ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers;
#else
    (void)layers;
#endif
    if (vkCreateInstance(&ici, nullptr, &c->instance) != VK_SUCCESS) { delete c; return nullptr; }
    return reinterpret_cast<rp_core*>(c);
}

void ref_destroy(rp_core* core) {
    auto* c = reinterpret_cast<RefCoreVk*>(core);
    if (!c) return;
    c->running = false;
    if (c->th.joinable()) c->th.join();
    c->destroy_device();
    if (c->instance) vkDestroyInstance(c->instance, nullptr);
    delete c;
}

rp_result ref_set_surfaces(rp_core* core, const rp_surface_set* set) {
    auto* c = reinterpret_cast<RefCoreVk*>(core);
    if (!c || !set || set->count == 0 || !set->surfaces || !set->sync_handle) return RP_ERR_BAD_ARG;

    // Re-entry safety: stop the loop and tear the prior device down first, so a second
    // set_surfaces never double-builds (imported handles are never closed here).
    c->running = false;
    if (c->th.joinable()) c->th.join();
    c->destroy_device();

    rp_result r = build_from_surfaces(c, set);
    if (r != RP_OK) c->destroy_device();   // reverse-order cleanup of the partial build
    return r;
}

rp_result ref_start(rp_core* core) {
    auto* c = reinterpret_cast<RefCoreVk*>(core);
    if (!c || c->device == VK_NULL_HANDLE) return RP_ERR_INTERNAL;
    if (c->running.load()) return RP_OK;
    c->running = true;
    c->th = std::thread([c] { c->loop(); });
    return RP_OK;
}

rp_result ref_stop(rp_core* core) {
    auto* c = reinterpret_cast<RefCoreVk*>(core);
    if (!c) return RP_ERR_BAD_ARG;
    c->running = false;
    if (c->th.joinable()) c->th.join();
    return RP_OK;
}

const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, ref_get_info, ref_create, ref_destroy,
    ref_set_surfaces, ref_start, ref_stop
};

}  // namespace

RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
