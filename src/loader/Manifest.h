#pragma once
#include <string>
#include <cstdint>
#include <retropark/retropark_abi.h>

namespace rp {
struct CoreManifest {
    std::string id;
    std::string name;
    std::string entry;
    rp_core_type type = RP_CORE_PRESENTING;
    rp_graphics_api graphics_api = RP_GFX_D3D11;
    uint32_t abi_version = 0;
};
rp_result parse_manifest(const std::string& json_text, CoreManifest& out, std::string& error);
}
