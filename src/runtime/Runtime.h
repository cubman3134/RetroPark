#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <retropark/retropark.h>
#include "loader/CoreLoader.h"
#include "loader/Win32CoreModule.h"
#include "render/SurfaceRing.h"
#include "render/IRenderBackend.h"
#include "audio/IAudioOutput.h"

namespace rp {
class Runtime {
public:
    Runtime(rp_graphics_api api, void* native_window);
    ~Runtime();
    rp_result load_core(const std::string& core_dir);
    rp_result unload_core();
    rp_result load_content(const char* path);
    rp_result resize(uint32_t w, uint32_t h);
    void set_input(const rp_input_state& in);
    rp_result present(uint8_t* out_rgba);
    size_t serialize_size();
    rp_result save_state(void* buf, size_t size);
    rp_result load_state(const void* buf, size_t size);

    // Host-iface trampolines.
    void on_submit(uint32_t index, uint64_t generation, uint64_t sync_value);
    void on_input(rp_input_state* out);
    void on_video_refresh(const void* data, uint32_t w, uint32_t h, uint32_t pitch);
    void on_audio_sample(const int16_t* frames, size_t num_frames);

    uint64_t audio_frames() const { return audio_frames_; }
    bool audio_nonsilent() const { return audio_nonsilent_; }

private:
    rp_result rebuild_surfaces(std::string& err);
    void open_audio(const rp_av_info& av);

    void* native_window_ = nullptr;
    rp_graphics_api api_;
    std::unique_ptr<IRenderBackend> backend_;
    std::unique_ptr<Win32CoreModule> module_;
    CoreLoader loader_;
    SurfaceRing ring_{3};
    rp_host_iface host_iface_{};
    rp_input_state input_{};
    std::mutex input_mtx_;
    uint32_t width_ = 64, height_ = 64;
    bool core_loaded_ = false;
    bool init_ok_ = false;
    rp_core_type core_type_ = RP_CORE_PRESENTING;
    const void* dr_data_ = nullptr;
    uint32_t dr_w_ = 0, dr_h_ = 0, dr_pitch_ = 0;
    bool dr_dupe_ = false, dr_have_ = false;
    uint32_t dr_max_w_ = 0, dr_max_h_ = 0;
    bool requires_content_ = false;
    bool content_loaded_ = false;
    std::unique_ptr<IAudioOutput> audio_;
    uint64_t audio_frames_ = 0;
    bool audio_nonsilent_ = false;
};
}
