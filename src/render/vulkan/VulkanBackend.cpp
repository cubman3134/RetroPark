#include "render/vulkan/VulkanBackend.h"
#include "render/FramebufferCopy.h"
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
    // VK_KHR_surface + VK_KHR_win32_surface are enabled unconditionally (widely
    // supported); the windowed present path needs them, and the headless path is
    // unaffected by their presence.
    const char* instExts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo=&app;
    ici.enabledExtensionCount=(uint32_t)(sizeof(instExts)/sizeof(char*));
    ici.ppEnabledExtensionNames=instExts;
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
    // The backend's device also drives the windowed present path, so it enables
    // VK_KHR_swapchain on top of the shared handoff extensions. Its VK_KHR_surface
    // instance dependency is satisfied by the surface exts enabled above.
    std::vector<const char*> devExts(kRequiredDeviceExts,
        kRequiredDeviceExts + sizeof(kRequiredDeviceExts)/sizeof(char*));
    devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext=&tsf;
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    dci.enabledExtensionCount=(uint32_t)devExts.size();
    dci.ppEnabledExtensionNames=devExts.data();
    VK_CHECK(vkCreateDevice(phys_,&dci,nullptr,&device_), err, "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return RP_OK;
}

rp_result VulkanBackend::initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) {
    width_=w; height_=h;
    // Device is created once; initialize() may be called again (e.g. a pre-load
    // resize) — don't leak a second instance/device on the repeat call.
    if (!device_) {
        rp_result r = create_instance_and_device(err);
        if (r != RP_OK) return r;
    }
    if (native_window) return create_swapchain(native_window, err);
    return RP_OK;
}

// Create (or recreate) the Win32 surface and a FIFO swapchain sized to the
// window's client area. Idempotent: an existing surface/swapchain is torn down
// first so a pre-load resize can rebuild cleanly.
rp_result VulkanBackend::create_swapchain(void* native_window, std::string& err) {
    destroy_swapchain();

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandle(nullptr);
    sci.hwnd = static_cast<HWND>(native_window);
    VK_CHECK(vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &swap_surface_), err, "vkCreateWin32SurfaceKHR");

    VkBool32 supported = VK_FALSE;
    VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(phys_, queue_family_, swap_surface_, &supported),
             err, "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (!supported) { err = "graphics queue cannot present to surface"; return RP_ERR_UNSUPPORTED; }

    // Pick a surface format: prefer B8G8R8A8_UNORM / SRGB-nonlinear, then
    // R8G8B8A8_UNORM, else the driver's first. The compositor's render pass is
    // built to match whatever format is chosen here.
    uint32_t fmtCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, swap_surface_, &fmtCount, nullptr), err, "surface fmt count");
    if (fmtCount == 0) { err = "no surface formats"; return RP_ERR_UNSUPPORTED; }
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, swap_surface_, &fmtCount, fmts.data()), err, "surface fmts");
    VkSurfaceFormatKHR chosen = fmts[0];
    for (const auto& f : fmts) {
        if (f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM) chosen = f;
    }
    swap_format_ = chosen.format;

    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, swap_surface_, &caps), err, "surface caps");
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width  = width_;  ext.height = height_;
    }
    // Clamp to the surface's allowed range and never let a zero slip through.
    if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
    if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (ext.width == 0)  ext.width  = 1;
    if (ext.height == 0) ext.height = 1;
    swap_extent_ = ext;

    uint32_t imageCount = caps.minImageCount < 2u ? 2u : caps.minImageCount;
    if (caps.maxImageCount != 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR scci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    scci.surface = swap_surface_;
    scci.minImageCount = imageCount;
    scci.imageFormat = swap_format_;
    scci.imageColorSpace = chosen.colorSpace;
    scci.imageExtent = swap_extent_;
    scci.imageArrayLayers = 1;
    scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // always supported
    scci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(device_, &scci, nullptr, &swapchain_), err, "vkCreateSwapchainKHR");

    uint32_t imgCount = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &imgCount, nullptr), err, "swapchain image count");
    swap_images_.resize(imgCount);
    VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &imgCount, swap_images_.data()), err, "swapchain images");

    swap_views_.resize(imgCount, VK_NULL_HANDLE);
    present_sems_.resize(imgCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imgCount; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = swap_images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swap_format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &swap_views_[i]), err, "swapchain image view");

        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &semci, nullptr, &present_sems_[i]), err, "present semaphore");
    }

    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(device_, &semci, nullptr, &acquire_sem_), err, "acquire semaphore");
    return RP_OK;
}

// Reverse-order teardown of the windowed present path: swap-image views + the
// per-image present semaphores + the acquire semaphore, then the swapchain, then
// the surface (with the instance still alive). Safe on a partially-built path.
void VulkanBackend::destroy_swapchain() {
    if (device_) vkDeviceWaitIdle(device_);
    if (acquire_sem_) { vkDestroySemaphore(device_, acquire_sem_, nullptr); acquire_sem_ = VK_NULL_HANDLE; }
    for (auto it = present_sems_.rbegin(); it != present_sems_.rend(); ++it)
        if (*it) vkDestroySemaphore(device_, *it, nullptr);
    present_sems_.clear();
    for (auto it = swap_views_.rbegin(); it != swap_views_.rend(); ++it)
        if (*it) vkDestroyImageView(device_, *it, nullptr);
    swap_views_.clear();
    swap_images_.clear();
    if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
    if (swap_surface_ && instance_) { vkDestroySurfaceKHR(instance_, swap_surface_, nullptr); swap_surface_ = VK_NULL_HANDLE; }
    swap_format_ = VK_FORMAT_UNDEFINED;
    swap_extent_ = {0, 0};
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
    if (consume_timeline_) { vkDestroySemaphore(device_, consume_timeline_, nullptr); consume_timeline_ = VK_NULL_HANDLE; }
    if (consume_sync_handle_) { CloseHandle(static_cast<HANDLE>(consume_sync_handle_)); consume_sync_handle_ = nullptr; }
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
    last_present_sync_ = 0;   // timeline restarts at 0; nothing consumed yet on this batch

    VkSemaphoreGetWin32HandleInfoKHR sgi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    sgi.semaphore = timeline_;
    sgi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE sh = nullptr;
    VK_CHECK(pfnGetSemHandle(device_, &sgi, &sh), err, "vkGetSemaphoreWin32HandleKHR");
    sync_handle_ = sh;   // owned; closed in destroy_surfaces()

    // A SECOND exported timeline: the consume channel. The host signals it (2f+3) after the composite of
    // frame f completes on the GPU; the core waits on it before reusing the slot frame f used. It is a
    // separate semaphore precisely so the core's own produce signals cannot advance it past the host's
    // real release — the multi-slot back-pressure that the single shared timeline could not express.
    VkSemaphore consume_sem = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &consume_sem), err, "vkCreateSemaphore (consume)");
    consume_timeline_ = consume_sem;
    VkSemaphoreGetWin32HandleInfoKHR csgi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    csgi.semaphore = consume_timeline_;
    csgi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE csh = nullptr;
    VK_CHECK(pfnGetSemHandle(device_, &csgi, &csh), err, "vkGetSemaphoreWin32HandleKHR (consume)");
    consume_sync_handle_ = csh;   // owned; closed in destroy_surfaces()

    return RP_OK;
}
// Lazily create everything the headless composite path needs: the compositor's
// pipelines, an offscreen render target (+ view), a host-visible staging buffer to
// read it back through, and a reusable command pool/buffer/fence. Idempotent.
rp_result VulkanBackend::ensure_composite_resources(std::string& err) {
    const bool windowed = (swapchain_ != VK_NULL_HANDLE);
    // The compositor's render pass must match its target's format: the swapchain's
    // format when presenting to a window, else the headless offscreen's R8G8B8A8.
    if (!compositor_ready_) {
        const VkFormat fmt = windowed ? swap_format_ : VK_FORMAT_R8G8B8A8_UNORM;
        rp_result r = compositor_.initialize(device_, fmt, err);
        if (r != RP_OK) return r;
        compositor_ready_ = true;
    }

    // The offscreen render target + host-visible staging buffer exist only for the
    // headless readback path; the windowed path renders straight into swap images.
    if (!windowed && !offscreen_image_) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {width_, height_, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device_, &ici, nullptr, &offscreen_image_), err, "offscreen image");

        VkMemoryRequirements req; vkGetImageMemoryRequirements(device_, offscreen_image_, &req);
        uint32_t typeIndex = 0;
        if (!find_mem_type(phys_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex)) {
            err = "no device-local memory type for offscreen target"; return RP_ERR_DEVICE;
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size; mai.memoryTypeIndex = typeIndex;
        VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &offscreen_mem_), err, "offscreen alloc");
        VK_CHECK(vkBindImageMemory(device_, offscreen_image_, offscreen_mem_, 0), err, "offscreen bind");

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = offscreen_image_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &offscreen_view_), err, "offscreen view");

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width_) * height_ * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &staging_buf_), err, "staging buffer");
        VkMemoryRequirements breq; vkGetBufferMemoryRequirements(device_, staging_buf_, &breq);
        uint32_t btype = 0;
        if (!find_mem_type(phys_, breq.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, btype)) {
            err = "no host-visible memory type for staging"; return RP_ERR_DEVICE;
        }
        VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bmai.allocationSize = breq.size; bmai.memoryTypeIndex = btype;
        VK_CHECK(vkAllocateMemory(device_, &bmai, nullptr, &staging_mem_), err, "staging alloc");
        VK_CHECK(vkBindBufferMemory(device_, staging_buf_, staging_mem_, 0), err, "staging bind");
    }

    if (!composite_pool_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = queue_family_;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &composite_pool_), err, "composite pool");

        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = composite_pool_;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device_, &cbai, &composite_cmd_), err, "composite cmd alloc");

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &composite_fence_), err, "composite fence");
    }
    return RP_OK;
}

// Records a TRANSFER_SRC transition of offscreen_image_ and a copy of its full extent
// (width_ x height_) into staging_buf_. Shared by composite_and_present() and
// composite_driven() so the offscreen->staging readback path exists exactly once.
void VulkanBackend::record_offscreen_readback(VkCommandBuffer cmd) {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = offscreen_image_;
    toSrc.subresourceRange = range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(cmd, offscreen_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buf_, 1, &region);
}

// Maps staging_buf_'s memory (already populated by a completed record_offscreen_readback()
// submission the caller has fence-waited) and copies width_ x height_ RGBA8 rows into
// out_rgba. Shared by composite_and_present() and composite_driven().
rp_result VulkanBackend::copy_staging_to_out(uint8_t* out_rgba, std::string& err) {
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, staging_mem_, 0, VK_WHOLE_SIZE, 0, &mapped), err, "map staging");
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < height_; ++y)
        std::memcpy(out_rgba + static_cast<size_t>(y) * width_ * 4,
                   src + static_cast<size_t>(y) * width_ * 4, static_cast<size_t>(width_) * 4);
    vkUnmapMemory(device_, staging_mem_);
    return RP_OK;
}

rp_result VulkanBackend::composite_and_present(uint32_t ready_index, uint64_t sync_value, bool has_frame,
                                               uint8_t* out_rgba, std::string& err) {
    if (!device_) { err = "device not initialized"; return RP_ERR_INTERNAL; }
    if (has_frame && ready_index >= surfaces_.size()) { err = "bad ready_index"; return RP_ERR_BAD_ARG; }

    // Windowed readback (swapchain present + CPU pixel readback in one call) is not
    // supported: the swapchain path never draws into the offscreen/staging pair the
    // readback copies from. Reject it explicitly, mirroring the D3D11 backend's guard.
    if (swapchain_ && out_rgba) {
        err = "windowed readback (swapchain + out_rgba) is not supported";
        return RP_ERR_UNSUPPORTED;
    }

    rp_result r = ensure_composite_resources(err);
    if (r != RP_OK) return r;

    if (swapchain_) return present_windowed(ready_index, sync_value, has_frame, err);

    // The host may call present() faster than the core produces, so latest_ready()
    // keeps returning the same frame. Only the FIRST composite of a given producer
    // value performs the cross-queue QFOT acquire and advances the timeline (wait
    // sync_value / signal sync_value+1); a repeat still re-composites and reads back
    // the image we already own (so the caller always gets valid pixels) but must not
    // re-acquire it or re-signal the timeline to a value it already reached. Mirrors
    // the windowed guard so both present paths are single-acquire / single-signal.
    const bool new_frame = has_frame && (sync_value > last_present_sync_);

    VK_CHECK(vkResetFences(device_, 1, &composite_fence_), err, "reset composite fence");
    VK_CHECK(vkResetCommandBuffer(composite_cmd_, 0), err, "reset composite cmd");

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(composite_cmd_, &bi), err, "begin composite cmd");

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Acquire ownership of the core's shared image from the external queue family. No
    // layout change (GENERAL->GENERAL): the shared images live in GENERAL for their
    // whole life, and the compositor's descriptor samples them in GENERAL too. Only on
    // a new frame: a repeat already owns the image and must not re-acquire it.
    if (new_frame) {
        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL_KHR;
        acquire.dstQueueFamilyIndex = queue_family_;
        acquire.image = surfaces_[ready_index].image;
        acquire.subresourceRange = range;
        vkCmdPipelineBarrier(composite_cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &acquire);
    }

    r = compositor_.render(composite_cmd_, offscreen_view_,
                           has_frame ? surfaces_[ready_index].view : VK_NULL_HANDLE,
                           width_, height_, err);
    if (r != RP_OK) { vkEndCommandBuffer(composite_cmd_); return r; }

    record_offscreen_readback(composite_cmd_);

    VK_CHECK(vkEndCommandBuffer(composite_cmd_), err, "end composite cmd");

    // Wait the PRODUCE timeline >= sync_value (the core's producer signal) before the GPU touches its
    // image, and signal the CONSUME value sync_value+1 once this composite finishes reading it. The
    // consume signal goes on the separate consume_timeline_ for the multi-slot ring (slot_count>=2): the
    // channel the core waits on before reuse, kept off the produce timeline so the core's own produce
    // signals can't race/self-satisfy it. For a single shared image (slot_count==1) the protocol is
    // lock-step and the peer waits the PRODUCE timeline (the rp_dolphin_boot path is never handed a
    // consume handle), so signal 2f+3 there — the original single-timeline behavior, race-free because
    // lock-step lets the core produce only after the host's consume. Only on a new frame: a repeat
    // re-samples the already-owned image and must leave each timeline strictly increasing.
    VkSemaphore consumeSem = (surfaces_.size() >= 2 && consume_timeline_) ? consume_timeline_ : timeline_;
    uint64_t waitValue = sync_value, signalValue = sync_value + 1;
    VkTimelineSemaphoreSubmitInfo tssi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &composite_cmd_;
    if (new_frame) {
        tssi.waitSemaphoreValueCount = 1; tssi.pWaitSemaphoreValues = &waitValue;
        tssi.signalSemaphoreValueCount = 1; tssi.pSignalSemaphoreValues = &signalValue;
        si.pNext = &tssi;
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &timeline_; si.pWaitDstStageMask = &waitStage;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &consumeSem;
    }
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, composite_fence_), err, "composite queue submit");

    const uint64_t kOneSecondNs = 1000000000ull;
    VkResult wr = vkWaitForFences(device_, 1, &composite_fence_, VK_TRUE, kOneSecondNs);
    if (wr != VK_SUCCESS) { err = "composite fence wait timed out"; return RP_ERR_TIMEOUT; }

    if (out_rgba) {
        rp_result rr = copy_staging_to_out(out_rgba, err);
        if (rr != RP_OK) return rr;
    }
    if (new_frame) last_present_sync_ = sync_value;   // consumed exactly once
    return RP_OK;
}

// Windowed present: acquire a swapchain image, composite the core frame
// straight into it (same compositor render used headless), transition it to
// PRESENT_SRC, and present. The core-image handoff (QFOT acquire GENERAL->GENERAL +
// timeline wait>=sync_value / signal sync_value+1) is byte-for-byte the headless
// logic; only the render TARGET changes to the acquired swap image. The swap-image
// layout transition is a PLAIN same-queue barrier — NOT the cross-queue QFOT.
rp_result VulkanBackend::present_windowed(uint32_t ready_index, uint64_t sync_value, bool has_frame,
                                          std::string& err) {
    const uint64_t kTimeoutNs = 1000000000ull;

    // The present loop outruns the core, so latest_ready() keeps returning the same
    // frame. Only the FIRST present of a given producer value performs the cross-queue
    // acquire and advances the timeline (wait 2f / signal 2f+1); repeats just re-sample
    // the image we already own and re-present it, leaving the timeline strictly
    // increasing and the QFOT protocol single-acquire-per-frame.
    const bool new_frame = has_frame && (sync_value > last_present_sync_);

    uint32_t imageIndex = 0;
    VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, kTimeoutNs, acquire_sem_, VK_NULL_HANDLE, &imageIndex);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        err = "vkAcquireNextImageKHR failed";
        return (ar == VK_TIMEOUT || ar == VK_NOT_READY) ? RP_ERR_TIMEOUT : RP_ERR_DEVICE;
    }

    VK_CHECK(vkResetFences(device_, 1, &composite_fence_), err, "reset composite fence");
    VK_CHECK(vkResetCommandBuffer(composite_cmd_, 0), err, "reset composite cmd");

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(composite_cmd_, &bi), err, "begin composite cmd");

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Cross-queue-family ownership acquire of the core's shared image (EXTERNAL ->
    // ours), GENERAL->GENERAL — identical to the headless path. Only on a new frame:
    // a repeat present already owns the image and must not re-acquire it.
    if (new_frame) {
        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL_KHR;
        acquire.dstQueueFamilyIndex = queue_family_;
        acquire.image = surfaces_[ready_index].image;
        acquire.subresourceRange = range;
        vkCmdPipelineBarrier(composite_cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &acquire);
    }

    // Composite into the acquired swap image's view. The compositor's render pass
    // takes the attachment from UNDEFINED (contents discarded, then cleared) to
    // COLOR_ATTACHMENT_OPTIMAL, so no pre-transition is needed here.
    rp_result r = compositor_.render(composite_cmd_, swap_views_[imageIndex],
                                     has_frame ? surfaces_[ready_index].view : VK_NULL_HANDLE,
                                     swap_extent_.width, swap_extent_.height, err);
    if (r != RP_OK) { vkEndCommandBuffer(composite_cmd_); return r; }

    // Plain (same-queue-family) transition COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC.
    VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = swap_images_[imageIndex];
    toPresent.subresourceRange = range;
    vkCmdPipelineBarrier(composite_cmd_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

    VK_CHECK(vkEndCommandBuffer(composite_cmd_), err, "end composite cmd");

    // Submit: always wait the binary acquire semaphore and signal the per-image
    // present semaphore; when a frame is present, also wait the shared timeline
    // >= sync_value and signal sync_value+1 (values for the binary entries are
    // ignored but must still be supplied, one per semaphore).
    VkSemaphore          waitSems[2];
    VkPipelineStageFlags waitStages[2];
    uint64_t             waitVals[2];
    VkSemaphore          sigSems[2];
    uint64_t             sigVals[2];
    uint32_t nWait = 0, nSig = 0;
    waitSems[nWait] = acquire_sem_; waitStages[nWait] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    waitVals[nWait] = 0; ++nWait;
    sigSems[nSig] = present_sems_[imageIndex]; sigVals[nSig] = 0; ++nSig;
    if (new_frame) {
        // Wait the PRODUCE timeline (2f+2); signal the CONSUME value (2f+3) on the separate consume
        // timeline for the multi-slot ring, else on the produce timeline (single-image lock-step).
        waitSems[nWait] = timeline_; waitStages[nWait] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        waitVals[nWait] = sync_value; ++nWait;
        sigSems[nSig] = (surfaces_.size() >= 2 && consume_timeline_) ? consume_timeline_ : timeline_;
        sigVals[nSig] = sync_value + 1; ++nSig;
    }

    VkTimelineSemaphoreSubmitInfo tssi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tssi.waitSemaphoreValueCount = nWait; tssi.pWaitSemaphoreValues = waitVals;
    tssi.signalSemaphoreValueCount = nSig; tssi.pSignalSemaphoreValues = sigVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &tssi;
    si.waitSemaphoreCount = nWait; si.pWaitSemaphores = waitSems; si.pWaitDstStageMask = waitStages;
    si.commandBufferCount = 1; si.pCommandBuffers = &composite_cmd_;
    si.signalSemaphoreCount = nSig; si.pSignalSemaphores = sigSems;
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, composite_fence_), err, "composite queue submit");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &present_sems_[imageIndex];
    pi.swapchainCount = 1; pi.pSwapchains = &swapchain_; pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { err = "vkQueuePresentKHR failed"; return RP_ERR_DEVICE; }

    // Single frame in flight: draining the fence guarantees the acquire semaphore is
    // unsignalled before the next acquire, and (with per-image present semaphores
    // reclaimed by the next acquire of the same index) keeps every semaphore reuse safe.
    VkResult wr = vkWaitForFences(device_, 1, &composite_fence_, VK_TRUE, kTimeoutNs);
    if (wr != VK_SUCCESS) { err = "composite fence wait timed out"; return RP_ERR_TIMEOUT; }

    if (new_frame) last_present_sync_ = sync_value;   // consumed exactly once
    return RP_OK;
}

// Reverse-order teardown of the composite path: fence/pool, staging, offscreen
// target, then the compositor's own objects. Safe to call on a partially-built or
// never-used composite path (every handle is null-guarded).
void VulkanBackend::destroy_composite() {
    if (!device_) return;
    if (composite_fence_) { vkDestroyFence(device_, composite_fence_, nullptr); composite_fence_ = VK_NULL_HANDLE; }
    if (composite_pool_) {   // also frees composite_cmd_
        vkDestroyCommandPool(device_, composite_pool_, nullptr);
        composite_pool_ = VK_NULL_HANDLE; composite_cmd_ = VK_NULL_HANDLE;
    }
    if (staging_buf_) { vkDestroyBuffer(device_, staging_buf_, nullptr); staging_buf_ = VK_NULL_HANDLE; }
    if (staging_mem_) { vkFreeMemory(device_, staging_mem_, nullptr); staging_mem_ = VK_NULL_HANDLE; }
    if (offscreen_view_) { vkDestroyImageView(device_, offscreen_view_, nullptr); offscreen_view_ = VK_NULL_HANDLE; }
    if (offscreen_image_) { vkDestroyImage(device_, offscreen_image_, nullptr); offscreen_image_ = VK_NULL_HANDLE; }
    if (offscreen_mem_) { vkFreeMemory(device_, offscreen_mem_, nullptr); offscreen_mem_ = VK_NULL_HANDLE; }
    if (compositor_ready_) { compositor_.destroy(); compositor_ready_ = false; }
}

// Reverse-order teardown of the driven upload path: view, image, image memory, then the
// staging buffer + its memory. Safe to call on a partially-built or never-used path.
void VulkanBackend::destroy_driven() {
    if (!device_) return;
    if (driven_view_) { vkDestroyImageView(device_, driven_view_, nullptr); driven_view_ = VK_NULL_HANDLE; }
    if (driven_img_) { vkDestroyImage(device_, driven_img_, nullptr); driven_img_ = VK_NULL_HANDLE; }
    if (driven_mem_) { vkFreeMemory(device_, driven_mem_, nullptr); driven_mem_ = VK_NULL_HANDLE; }
    if (driven_staging_buf_) { vkDestroyBuffer(device_, driven_staging_buf_, nullptr); driven_staging_buf_ = VK_NULL_HANDLE; }
    if (driven_staging_mem_) { vkFreeMemory(device_, driven_staging_mem_, nullptr); driven_staging_mem_ = VK_NULL_HANDLE; }
    driven_w_ = 0; driven_h_ = 0;
}

// (Re)creates driven_img_/driven_view_ + driven_staging_buf_ on a size change, maps the
// staging buffer and copies `data` into it (pitch-respecting, tightly packed dst) via
// copy_rgba8_rows, then records + submits a single command buffer that transitions the
// image UNDEFINED->TRANSFER_DST_OPTIMAL (a plain single-device transition: QUEUE_FAMILY_
// IGNORED, NOT the cross-queue-family QFOT the shared presenting images use), copies the
// staging buffer into it, and transitions it to SHADER_READ_ONLY_OPTIMAL; then fence-waits.
// Using UNDEFINED as the transition's oldLayout is valid (and validation-clean) even on a
// repeat call that reuses the same image: it tells the driver to discard prior contents,
// which is exactly right since the copy below overwrites the whole image every time — and
// it means this call never needs to track the image's previous actual layout.
rp_result VulkanBackend::upload_driven_frame(const void* data, uint32_t width, uint32_t height,
                                             uint32_t pitch, std::string& err) {
    if (!driven_img_ || driven_w_ != width || driven_h_ != height) {
        destroy_driven();

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device_, &ici, nullptr, &driven_img_), err, "driven image");

        VkMemoryRequirements req; vkGetImageMemoryRequirements(device_, driven_img_, &req);
        uint32_t typeIndex = 0;
        if (!find_mem_type(phys_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, typeIndex)) {
            err = "no device-local memory type for driven image"; return RP_ERR_DEVICE;
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size; mai.memoryTypeIndex = typeIndex;
        VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &driven_mem_), err, "driven alloc");
        VK_CHECK(vkBindImageMemory(device_, driven_img_, driven_mem_, 0), err, "driven bind");

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = driven_img_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &driven_view_), err, "driven view");

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &driven_staging_buf_), err, "driven staging buffer");
        VkMemoryRequirements breq; vkGetBufferMemoryRequirements(device_, driven_staging_buf_, &breq);
        uint32_t btype = 0;
        if (!find_mem_type(phys_, breq.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, btype)) {
            err = "no host-visible memory type for driven staging"; return RP_ERR_DEVICE;
        }
        VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bmai.allocationSize = breq.size; bmai.memoryTypeIndex = btype;
        VK_CHECK(vkAllocateMemory(device_, &bmai, nullptr, &driven_staging_mem_), err, "driven staging alloc");
        VK_CHECK(vkBindBufferMemory(device_, driven_staging_buf_, driven_staging_mem_, 0), err, "driven staging bind");

        driven_w_ = width; driven_h_ = height;
    }

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, driven_staging_mem_, 0, VK_WHOLE_SIZE, 0, &mapped), err, "map driven staging");
    copy_rgba8_rows(static_cast<const uint8_t*>(data), width, height, pitch,
                    static_cast<uint8_t*>(mapped), width * 4);
    vkUnmapMemory(device_, driven_staging_mem_);   // HOST_COHERENT: no explicit flush needed

    VK_CHECK(vkResetFences(device_, 1, &composite_fence_), err, "reset driven upload fence");
    VK_CHECK(vkResetCommandBuffer(composite_cmd_, 0), err, "reset driven upload cmd");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(composite_cmd_, &bi), err, "begin driven upload cmd");

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = driven_img_;
    toDst.subresourceRange = range;
    vkCmdPipelineBarrier(composite_cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(composite_cmd_, driven_staging_buf_, driven_img_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = driven_img_;
    toRead.subresourceRange = range;
    vkCmdPipelineBarrier(composite_cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);

    VK_CHECK(vkEndCommandBuffer(composite_cmd_), err, "end driven upload cmd");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &composite_cmd_;
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, composite_fence_), err, "driven upload submit");

    const uint64_t kOneSecondNs = 1000000000ull;
    VkResult wr = vkWaitForFences(device_, 1, &composite_fence_, VK_TRUE, kOneSecondNs);
    if (wr != VK_SUCCESS) { err = "driven upload fence wait timed out"; return RP_ERR_TIMEOUT; }
    return RP_OK;
}

// Headless half of composite_driven(): renders the driven image into the
// offscreen target at DISPLAY size (width_ x height_ — not the per-call core width/height;
// the compositor's fullscreen triangle samples 0..1 UVs so the core frame scales to fill
// it), then reads it back into out_rgba via the same offscreen->staging path
// composite_and_present() uses.
rp_result VulkanBackend::composite_driven_headless(uint8_t* out_rgba, std::string& err) {
    VK_CHECK(vkResetFences(device_, 1, &composite_fence_), err, "reset driven composite fence");
    VK_CHECK(vkResetCommandBuffer(composite_cmd_, 0), err, "reset driven composite cmd");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(composite_cmd_, &bi), err, "begin driven composite cmd");

    // driven_view_ may be null here (dupe==true, never uploaded) -> clears to black only.
    rp_result r = compositor_.render(composite_cmd_, offscreen_view_, driven_view_,
                                     width_, height_, err, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (r != RP_OK) { vkEndCommandBuffer(composite_cmd_); return r; }

    if (out_rgba) record_offscreen_readback(composite_cmd_);

    VK_CHECK(vkEndCommandBuffer(composite_cmd_), err, "end driven composite cmd");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &composite_cmd_;
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, composite_fence_), err, "driven composite submit");

    const uint64_t kOneSecondNs = 1000000000ull;
    VkResult wr = vkWaitForFences(device_, 1, &composite_fence_, VK_TRUE, kOneSecondNs);
    if (wr != VK_SUCCESS) { err = "driven composite fence wait timed out"; return RP_ERR_TIMEOUT; }

    if (out_rgba) {
        rp_result rr = copy_staging_to_out(out_rgba, err);
        if (rr != RP_OK) return rr;
    }
    return RP_OK;
}

// Windowed half of composite_driven(): acquires a swap image, renders the driven image
// into it at DISPLAY size (swap_extent_, mirroring present_windowed()), and
// presents. Unlike present_windowed(), there is no QFOT acquire and no shared timeline to
// wait/signal — the driven image is a normal single-device sampled image, so only the
// binary acquire/present semaphore pair is needed.
rp_result VulkanBackend::present_driven_windowed(std::string& err) {
    const uint64_t kTimeoutNs = 1000000000ull;

    uint32_t imageIndex = 0;
    VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, kTimeoutNs, acquire_sem_, VK_NULL_HANDLE, &imageIndex);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        err = "vkAcquireNextImageKHR failed";
        return (ar == VK_TIMEOUT || ar == VK_NOT_READY) ? RP_ERR_TIMEOUT : RP_ERR_DEVICE;
    }

    VK_CHECK(vkResetFences(device_, 1, &composite_fence_), err, "reset driven present fence");
    VK_CHECK(vkResetCommandBuffer(composite_cmd_, 0), err, "reset driven present cmd");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(composite_cmd_, &bi), err, "begin driven present cmd");

    rp_result r = compositor_.render(composite_cmd_, swap_views_[imageIndex], driven_view_,
                                     swap_extent_.width, swap_extent_.height, err,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (r != RP_OK) { vkEndCommandBuffer(composite_cmd_); return r; }

    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = swap_images_[imageIndex];
    toPresent.subresourceRange = range;
    vkCmdPipelineBarrier(composite_cmd_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

    VK_CHECK(vkEndCommandBuffer(composite_cmd_), err, "end driven present cmd");

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acquire_sem_; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &composite_cmd_;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &present_sems_[imageIndex];
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, composite_fence_), err, "driven present submit");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &present_sems_[imageIndex];
    pi.swapchainCount = 1; pi.pSwapchains = &swapchain_; pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { err = "vkQueuePresentKHR failed"; return RP_ERR_DEVICE; }

    VkResult wr = vkWaitForFences(device_, 1, &composite_fence_, VK_TRUE, kTimeoutNs);
    if (wr != VK_SUCCESS) { err = "driven present fence wait timed out"; return RP_ERR_TIMEOUT; }
    return RP_OK;
}

rp_result VulkanBackend::composite_driven(const void* data, uint32_t width, uint32_t height, uint32_t pitch,
                                          bool dupe, uint8_t* out_rgba, std::string& err) {
    if (!device_) { err = "device not initialized"; return RP_ERR_INTERNAL; }

    // Same windowed-readback restriction as composite_and_present: the windowed render
    // target is the acquired swap image, but the readback below copies from the offscreen
    // target, which is never drawn into when a swapchain exists.
    if (swapchain_ && out_rgba) {
        err = "windowed readback (swapchain + out_rgba) is not supported";
        return RP_ERR_UNSUPPORTED;
    }

    rp_result r = ensure_composite_resources(err);
    if (r != RP_OK) return r;

    if (!dupe) {
        r = upload_driven_frame(data, width, height, pitch, err);
        if (r != RP_OK) return r;
    }
    // dupe==true reuses driven_view_ as-is (no re-upload); if it was never populated
    // (still VK_NULL_HANDLE), the compositor below gets a null core view -> clears to black.

    if (swapchain_) return present_driven_windowed(err);
    return composite_driven_headless(out_rgba, err);
}

VulkanBackend::~VulkanBackend() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroy_driven();      // driven upload path (independent of the composite/swapchain state)
        destroy_composite();   // framebuffer may reference a swap view -> tear it down first
        destroy_swapchain();   // swapchain/views/surface (surface freed with instance alive)
        destroy_surfaces();
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}
}
