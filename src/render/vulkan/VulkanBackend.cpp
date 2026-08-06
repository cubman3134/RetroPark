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

rp_result VulkanBackend::allocate_surfaces(uint32_t, uint32_t, uint32_t,
                                           std::vector<rp_surface_desc>&, std::string& err) {
    err="allocate_surfaces not implemented until Task 4"; return RP_ERR_UNSUPPORTED;
}
rp_result VulkanBackend::composite_and_present(uint32_t, uint64_t, bool, uint8_t*, std::string& err) {
    err="composite not implemented until Task 5"; return RP_ERR_UNSUPPORTED;
}

VulkanBackend::~VulkanBackend() {
    if (device_) { vkDeviceWaitIdle(device_); vkDestroyDevice(device_, nullptr); }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}
}
