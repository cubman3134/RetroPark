#include "loader/StaticCoreRegistry.h"
#include <map>

namespace rp {
namespace {
std::map<std::string, rp_get_core_abi_fn>& registry() {
    static std::map<std::string, rp_get_core_abi_fn> r;   // function-local static: safe init order
    return r;
}
}
void StaticCoreRegistry::register_core(const std::string& id, rp_get_core_abi_fn getter) {
    registry()[id] = getter;   // last-wins
}
bool StaticCoreRegistry::has(const std::string& id) { return registry().count(id) != 0; }
rp_get_core_abi_fn StaticCoreRegistry::get(const std::string& id) {
    auto it = registry().find(id);
    return it == registry().end() ? nullptr : it->second;
}
}
