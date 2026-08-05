#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <retropark/retropark.h>
#include "loader/CoreLoader.h"
#include "loader/Win32CoreModule.h"
#include "render/SurfaceRing.h"
#include "render/d3d11/D3D11Backend.h"

namespace rp {
class Runtime {
public:
    Runtime(rp_graphics_api api, void* native_window);
    ~Runtime();
    rp_result load_core(const std::string& core_dir);
    rp_result unload_core();
    rp_result resize(uint32_t w, uint32_t h);
    void set_input(const rp_input_state& in);
    rp_result present(uint8_t* out_rgba);

    // Host-iface trampolines.
    void on_submit(uint32_t index, uint64_t generation);
    void on_input(rp_input_state* out);

private:
    rp_result rebuild_surfaces(std::string& err);

    void* native_window_ = nullptr;
    std::unique_ptr<D3D11Backend> backend_;
    std::unique_ptr<Win32CoreModule> module_;
    CoreLoader loader_;
    SurfaceRing ring_{3};
    rp_host_iface host_iface_{};
    rp_input_state input_{};
    std::mutex input_mtx_;
    uint32_t width_ = 64, height_ = 64;
    bool core_loaded_ = false;
};
}
