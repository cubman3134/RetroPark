#include "render/vulkan/VulkanBackend.h"
#include <vector>

namespace rp {

static bool pick_physical(VkInstance inst, VkPhysicalDevice& out, uint8_t uuid[16], uint32_t& qfam) {
    uint32_t n=0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(inst, &n, devs.data());
    VkPhysicalDevice best = VK_NULL_HANDLE; bool bestDiscrete=false;
    for (auto d : devs) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
        // require a graphics queue
        uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn); vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,qs.data());
        int gfx=-1; for (uint32_t i=0;i<qn;++i) if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){ gfx=(int)i; break; }
        if (gfx<0) continue;
        bool discrete = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (best==VK_NULL_HANDLE || (discrete && !bestDiscrete)) {
            best=d; bestDiscrete=discrete; qfam=(uint32_t)gfx;
        }
    }
    if (best==VK_NULL_HANDLE) return false;
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext=&idp;
    vkGetPhysicalDeviceProperties2(best, &p2);
    std::memcpy(uuid, idp.deviceUUID, 16);
    out = best; return true;
}

rp_result VulkanBackend::create_instance_and_device(std::string& err) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    const char* instExts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo=&app;
    ici.enabledExtensionCount=3; ici.ppEnabledExtensionNames=instExts;
#ifndef NDEBUG
    ici.enabledLayerCount=1; ici.ppEnabledLayerNames=layers;
#endif
    VK_CHECK(vkCreateInstance(&ici,nullptr,&instance_), err, "vkCreateInstance");

    if (!pick_physical(instance_, phys_, device_uuid_, queue_family_)) { err="no gfx device"; return RP_ERR_UNSUPPORTED; }

    // Enable timeline semaphore feature.
    VkPhysicalDeviceTimelineSemaphoreFeatures tsf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    tsf.timelineSemaphore = VK_TRUE;
    float pri=1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=queue_family_; qci.queueCount=1; qci.pQueuePriorities=&pri;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext=&tsf;
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    dci.enabledExtensionCount=(uint32_t)(sizeof(kRequiredDeviceExts)/sizeof(char*));
    dci.ppEnabledExtensionNames=kRequiredDeviceExts;
    VK_CHECK(vkCreateDevice(phys_,&dci,nullptr,&device_), err, "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return RP_OK;
}

rp_result VulkanBackend::initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) {
    width_=w; height_=h; (void)native_window;
    return create_instance_and_device(err);
}

bool VulkanBackend::probe_vulkan_shared() {
    VulkanBackend b; std::string e;
    bool ok = (b.create_instance_and_device(e) == RP_OK);
    return ok;   // destructor tears down
}

static bool find_mem_type(VkPhysicalDevice phys, uint32_t type_bits,
                          VkMemoryPropertyFlags props, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) { out = i; return true; }
    return false;
}

// Release a surface batch (and the timeline) in reverse order, closing every
// owned NT handle exactly once. Safe to call with partially-built surfaces.
void VulkanBackend::destroy_surfaces() {
    if (device_) vkDeviceWaitIdle(device_);
    for (auto it = surfaces_.rbegin(); it != surfaces_.rend(); ++it) {
        if (it->view)  vkDestroyImageView(device_, it->view, nullptr);
        if (it->image) vkDestroyImage(device_, it->image, nullptr);
        if (it->mem)   vkFreeMemory(device_, it->mem, nullptr);
        if (it->handle) CloseHandle(static_cast<HANDLE>(it->handle));
    }
    surfaces_.clear();
    if (timeline_) { vkDestroySemaphore(device_, timeline_, nullptr); timeline_ = VK_NULL_HANDLE; }
    if (sync_handle_) { CloseHandle(static_cast<HANDLE>(sync_handle_)); sync_handle_ = nullptr; }
}

rp_result VulkanBackend::allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                           std::vector<rp_surface_desc>& out, std::string& err) {
    if (!device_) { err = "device not initialized"; return RP_ERR_INTERNAL; }
    if (count == 0) { err = "count must be > 0"; return RP_ERR_BAD_ARG; }

    // Free/close any prior batch first so re-allocation never double-closes a handle.
    destroy_surfaces();

    auto pfnGetMemHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryWin32HandleKHR"));
    auto pfnGetSemHandle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        vkGetDeviceProcAddr(device_, "vkGetSemaphoreWin32HandleKHR"));
    if (!pfnGetMemHandle || !pfnGetSemHandle) { err = "load vkGet*Win32HandleKHR"; return RP_ERR_DEVICE; }

    const VkFormat kFmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags kUsage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    out.clear();
    surfaces_.reserve(count);
    width_ = w; height_ = h;

    for (uint32_t i = 0; i < count; ++i) {
        surfaces_.push_back(VkSurface{});
        VkSurface& s = surfaces_.back();   // pushed early so a partial failure is torn down by dtor

        VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        emici.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &emici;
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
        VK_CHECK(vkCreateImage(device_, &ici, nullptr, &s.image), err, "vkCreateImage");

        VkMemoryRequirements req; vkGetImageMemoryRequirements(device_, s.image, &req);
        uint32_t typeIndex = 0;
        if (!find_mem_type(phys_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex)) {
            err = "no device-local memory type for shared image"; return RP_ERR_DEVICE;
        }

        VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dai.image = s.image;
        VkExportMemoryAllocateInfo emai{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        emai.pNext = &dai;
        emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &emai;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = typeIndex;
        VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &s.mem), err, "vkAllocateMemory");
        VK_CHECK(vkBindImageMemory(device_, s.image, s.mem, 0), err, "vkBindImageMemory");

        VkMemoryGetWin32HandleInfoKHR ghi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        ghi.memory = s.mem;
        ghi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE mh = nullptr;
        VK_CHECK(pfnGetMemHandle(device_, &ghi, &mh), err, "vkGetMemoryWin32HandleKHR");
        s.handle = mh;   // owned; closed in destroy_surfaces()

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = s.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kFmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &s.view), err, "vkCreateImageView");

        rp_surface_desc d{};
        d.index = i; d.width = w; d.height = h;
        d.format = RP_FMT_R8G8B8A8_UNORM;
        d.shared_handle = s.handle;
        d.generation = 0;
        out.push_back(d);
    }

    // One exported shared timeline semaphore, initial value 0.
    VkSemaphoreTypeCreateInfo stci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    stci.initialValue = 0;
    VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    esci.pNext = &stci;
    esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &esci;
    VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &timeline_), err, "vkCreateSemaphore");

    VkSemaphoreGetWin32HandleInfoKHR sgi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    sgi.semaphore = timeline_;
    sgi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE sh = nullptr;
    VK_CHECK(pfnGetSemHandle(device_, &sgi, &sh), err, "vkGetSemaphoreWin32HandleKHR");
    sync_handle_ = sh;   // owned; closed in destroy_surfaces()

    return RP_OK;
}
rp_result VulkanBackend::composite_and_present(uint32_t, uint64_t, bool, uint8_t*, std::string& err) {
    err="composite not implemented until Task 5"; return RP_ERR_UNSUPPORTED;
}

VulkanBackend::~VulkanBackend() {
    if (device_) { vkDeviceWaitIdle(device_); destroy_surfaces(); vkDestroyDevice(device_, nullptr); }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}
}
