#pragma once
#include <string>
#include <cstdint>
#include <retropark/retropark_abi.h>
#include "loader/ICoreModule.h"

namespace rp {
enum class LoaderState { Unloaded, Loaded, Created, Started };

class CoreLoader {
public:
    rp_result load(ICoreModule* module, std::string& error);
    rp_result create(const rp_host_iface* host, std::string& error);
    rp_result set_surfaces(const rp_surface_desc* descs, uint32_t count, std::string& error);
    rp_result start(std::string& error);
    rp_result stop(std::string& error);
    void destroy();

    LoaderState state() const { return state_; }
    const rp_core_abi* abi() const { return abi_; }

private:
    LoaderState state_ = LoaderState::Unloaded;
    ICoreModule* module_ = nullptr;
    const rp_core_abi* abi_ = nullptr;
    rp_core* core_ = nullptr;
};
}
