#include "loader/Manifest.h"
#include <nlohmann/json.hpp>

namespace rp {

static bool as_string(const nlohmann::json& j, const char* key, std::string& out) {
    if (!j.contains(key) || !j[key].is_string()) return false;
    out = j[key].get<std::string>();
    return true;
}

rp_result parse_manifest(const std::string& text, CoreManifest& out, std::string& error) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) { error = "malformed json"; return RP_ERR_BAD_ARG; }

    std::string type_s, gfx_s;
    if (!as_string(j, "id", out.id))    { error = "missing id"; return RP_ERR_BAD_ARG; }
    if (!as_string(j, "name", out.name)){ error = "missing name"; return RP_ERR_BAD_ARG; }
    if (!as_string(j, "entry", out.entry)){ error = "missing entry"; return RP_ERR_BAD_ARG; }
    if (!as_string(j, "type", type_s)) { error = "missing type"; return RP_ERR_BAD_ARG; }
    if (!as_string(j, "graphics_api", gfx_s)) { error = "missing graphics_api"; return RP_ERR_BAD_ARG; }
    if (!j.contains("abi_version") || !j["abi_version"].is_number_unsigned()) {
        error = "missing abi_version"; return RP_ERR_BAD_ARG;
    }
    out.abi_version = j["abi_version"].get<uint32_t>();

    if (type_s == "presenting") out.type = RP_CORE_PRESENTING;
    else if (type_s == "driven") out.type = RP_CORE_DRIVEN;
    else { error = "unknown type: " + type_s; return RP_ERR_BAD_ARG; }

    if (gfx_s == "d3d11") out.graphics_api = RP_GFX_D3D11;
    else if (gfx_s == "vulkan") out.graphics_api = RP_GFX_VULKAN;
    else { error = "unknown graphics_api: " + gfx_s; return RP_ERR_BAD_ARG; }

    return RP_OK;
}
}
