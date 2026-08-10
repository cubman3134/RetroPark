#pragma once
#include <string>
#include <retropark/retropark_abi.h>

namespace rp {
// Process-wide map core_id -> compiled-in rp_get_core_abi getter. Statically-linked cores register here
// at startup so the Runtime can load them with no DLL and no filesystem (the iOS shape).
struct StaticCoreRegistry {
    static void register_core(const std::string& id, rp_get_core_abi_fn getter);  // last-wins
    static bool has(const std::string& id);
    static rp_get_core_abi_fn get(const std::string& id);  // nullptr if unknown
};
}
