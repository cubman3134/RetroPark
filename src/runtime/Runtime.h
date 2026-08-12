#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <mutex>
#include <deque>
#include <vector>
#include <cstdint>
#include <retropark/retropark.h>
#include "loader/CoreLoader.h"
#include "loader/ICoreModule.h"
#include "loader/Win32CoreModule.h"
#include "loader/StaticCoreModule.h"
#include "loader/StaticCoreRegistry.h"
#include "render/SurfaceRing.h"
#include "render/IRenderBackend.h"
#include "audio/IAudioOutput.h"

namespace rp {
class Runtime {
public:
    Runtime(rp_graphics_api api, void* native_window);
    ~Runtime();
    rp_result load_core(const std::string& core_dir);
    rp_result load_static_core(const std::string& core_id);
    rp_result unload_core();
    rp_result load_content(const char* path);
    rp_result resize(uint32_t w, uint32_t h);
    void set_input(uint32_t port, const rp_input_state& in);
    rp_result present(uint8_t* out_rgba);
    rp_result advance(int emit_audio);
    rp_result render(uint8_t* out_rgba);
    size_t serialize_size();
    rp_result save_state(void* buf, size_t size);
    rp_result load_state(const void* buf, size_t size);
    rp_result set_rewind(int enabled, uint32_t max_snapshots);
    rp_result rewind();
    rp_result pause();
    rp_result resume();
    rp_result reset();
    rp_result get_status(rp_runtime_status* out);

    // Host-iface trampolines.
    void on_submit(uint32_t index, uint64_t generation, uint64_t sync_value);
    void on_input(uint32_t port, rp_input_state* out);
    void on_video_refresh(const void* data, uint32_t w, uint32_t h, uint32_t pitch);
    void on_audio_sample(const int16_t* frames, size_t num_frames);

    uint64_t audio_frames() const { return audio_frames_; }
    bool audio_nonsilent() const { return audio_nonsilent_; }
    uint64_t input_polls() const { return input_polls_.load(std::memory_order_relaxed); }

private:
    rp_result rebuild_surfaces(std::string& err);
    rp_result finish_load_core(rp_core_type type, std::string& err);  // shared post-create logic
    void open_audio(const rp_av_info& av);
    void tick_fps();  // rolling ~0.5s present-rate measure; cheap, no alloc/lock

    void* native_window_ = nullptr;
    rp_graphics_api api_;
    std::unique_ptr<IRenderBackend> backend_;
    std::unique_ptr<ICoreModule> module_;
    CoreLoader loader_;
    SurfaceRing ring_{3};
    rp_host_iface host_iface_{};
    rp_input_state input_[2]{};
    std::mutex input_mtx_;
    bool suppress_audio_ = false;
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
    std::atomic<uint64_t> audio_frames_{0};
    std::atomic<bool>     audio_nonsilent_{false};
    std::atomic<uint64_t> input_polls_{0};

    // Rewind ring: bounded uncompressed per-frame snapshots of the driven core's pre-frame
    // state, captured at the top of each forward present(). See RewindRing.h / spec §2.
    std::deque<std::vector<uint8_t>> rewind_ring_;
    bool     rewind_enabled_ = false;
    bool     rewind_replay_  = false;   // set by rewind(); the next present() re-renders a
                                        // restored frame and must NOT capture it
    uint32_t rewind_max_     = 0;

    std::atomic<bool> paused_{false};  // read on the audio puller thread (on_audio_sample) while
                                       // written from the frontend thread (pause/resume/reset)
    std::string content_path_;   // last-loaded content path, for reset()
    std::string core_dir_;       // last dynamic core dir (reset fallback; empty for static)
    std::string core_id_;        // last static core id (empty for dynamic)
    // Presenting-core pause freeze: re-composite the last ring frame instead of acquiring a new one.
    uint32_t last_ready_idx_ = 0; uint64_t last_ready_sv_ = 0; bool have_last_ready_ = false;
    // fps measurement (present rate).
    double fps_ = 0.0; uint64_t fps_count_ = 0; uint64_t fps_t0_ns_ = 0;
};
}
