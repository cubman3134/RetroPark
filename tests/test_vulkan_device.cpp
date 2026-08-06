#include <doctest/doctest.h>
#include "render/vulkan/VulkanBackend.h"
using namespace rp;

TEST_CASE("vulkan: device initializes and exposes a non-zero UUID") {
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    VulkanBackend b; std::string err;
    REQUIRE(b.initialize(nullptr, 64, 64, err) == RP_OK);
    uint8_t uuid[16]; b.present_device_uuid(uuid);
    uint8_t zero[16] = {0};
    CHECK(std::memcmp(uuid, zero, 16) != 0);
}
