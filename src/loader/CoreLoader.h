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
    rp_result set_surfaces(const rp_surface_set* set, std::string& error);
    rp_result start(std::string& error);
    rp_result stop(std::string& error);
    void destroy();
    rp_result run_frame(std::string& error);
    rp_result get_av_info(rp_av_info* out, std::string& error);
    size_t serialize_size();
    rp_result serialize(void* data, size_t size, std::string& error);
    rp_result unserialize(const void* data, size_t size, std::string& error);
    rp_result load_content(const char* path, std::string& error);

    LoaderState state() const { return state_; }
    const rp_core_abi* abi() const { return abi_; }
    bool has_load_content() const { return abi_ && abi_->load_content != nullptr; }

private:
    LoaderState state_ = LoaderState::Unloaded;
    ICoreModule* module_ = nullptr;
    const rp_core_abi* abi_ = nullptr;
    rp_core* core_ = nullptr;
};
}
