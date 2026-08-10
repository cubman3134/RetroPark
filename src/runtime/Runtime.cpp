#include "runtime/Runtime.h"
#include "runtime/RewindRing.h"
#include "runtime/BackendFactory.h"
#include "loader/Manifest.h"
#include "render/FramebufferCopy.h"
#include "audio/XAudio2Output.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>

namespace rp {

static void host_log(rp_host*, int, const char*) {}
static void host_submit(rp_host* h, uint32_t i, uint64_t g, uint64_t sv) {
    reinterpret_cast<Runtime*>(h)->on_submit(i, g, sv);
}
static void host_input(rp_host* h, uint32_t port, rp_input_state* out) {
    reinterpret_cast<Runtime*>(h)->on_input(port, out);
}
static void host_video_refresh(rp_host* h, const void* d, uint32_t w, uint32_t hh, uint32_t p) {
    reinterpret_cast<Runtime*>(h)->on_video_refresh(d, w, hh, p);
}
static void host_audio_sample(rp_host* h, const int16_t* f, size_t n) {
    reinterpret_cast<Runtime*>(h)->on_audio_sample(f, n);
}

Runtime::Runtime(rp_graphics_api api, void* native_window) : native_window_(native_window), api_(api) {
    backend_ = make_backend(api_);
    if (!backend_) {
        init_ok_ = false;
        return;
    }
    std::string err;
    init_ok_ = (backend_->initialize(native_window_, width_, height_, err) == RP_OK);
    host_iface_.host = reinterpret_cast<rp_host*>(this);
    host_iface_.log = host_log;
    host_iface_.submit_frame = host_submit;
    host_iface_.input_state = host_input;
    host_iface_.video_refresh = host_video_refresh;
    host_iface_.audio_sample = host_audio_sample;
}

Runtime::~Runtime() { unload_core(); }

void Runtime::on_submit(uint32_t index, uint64_t generation, uint64_t sync_value) {
    ring_.accept_submit(index, generation, sync_value);
}
void Runtime::on_input(uint32_t port, rp_input_state* out) {
    input_polls_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(input_mtx_);
    *out = input_[port & 1u];               // clamp to {0,1}
}
void Runtime::set_input(uint32_t port, const rp_input_state& in) {
    std::lock_guard<std::mutex> lk(input_mtx_);
    input_[port & 1u] = in;
}
void Runtime::on_video_refresh(const void* data, uint32_t w, uint32_t h, uint32_t pitch) {
    dr_have_ = true;
    dr_dupe_ = (data == nullptr);
    dr_data_ = data;
    dr_w_ = w; dr_h_ = h; dr_pitch_ = pitch;
}
void Runtime::on_audio_sample(const int16_t* frames, size_t n) {
    if (suppress_audio_) return;           // silent re-simulation during rollback
    if (!frames || n == 0) return;
    audio_frames_ += n;
    if (!audio_nonsilent_) {
        // n * 2: stereo-only pipeline contract — open_audio() always opens channels = 2 and
        // the shim forwards interleaved stereo, so each of the n frames is 2 int16 samples.
        // Revisit this scan if a mono path is ever added.
        for (size_t i = 0; i < n * 2; ++i) { int16_t s = frames[i]; if (s > 128 || s < -128) { audio_nonsilent_ = true; break; } }
    }
    if (audio_) audio_->submit(frames, n);
}
void Runtime::open_audio(const rp_av_info& av) {
    audio_frames_ = 0;
    audio_nonsilent_ = false;
    if (av.sample_rate <= 0.0) return;           // no audio
    auto out = std::make_unique<XAudio2Output>();
    std::string err;
    if (out->open((uint32_t)av.sample_rate, 2, err) == RP_OK) audio_ = std::move(out);
    // best-effort: on failure leave audio_ null (game runs silent); do NOT fail load
}

rp_result Runtime::rebuild_surfaces(std::string& err) {
    if (loader_.state() == LoaderState::Started) {
        std::string e;
        loader_.stop(e);
    }
    std::vector<rp_surface_desc> descs;
    rp_result r = backend_->allocate_surfaces(ring_.slot_count(), width_, height_, descs, err);
    if (r != RP_OK) return r;
    uint64_t gen = ring_.reallocate(width_, height_);
    for (auto& d : descs) d.generation = gen;
    if (loader_.state() == LoaderState::Created) {
        rp_surface_set set{};
        set.count = (uint32_t)descs.size();
        set.surfaces = descs.data();
        set.sync_handle = backend_->present_sync_handle();
        backend_->present_device_uuid(set.device_uuid);
        return loader_.set_surfaces(&set, err);
    }
    return RP_OK;
}

rp_result Runtime::resize(uint32_t w, uint32_t h) {
    if (!backend_) return RP_ERR_DEVICE;
    width_ = w; height_ = h;
    std::string err;
    if (!core_loaded_) {
        rp_result r = backend_->initialize(native_window_, w, h, err);
        init_ok_ = (r == RP_OK);
        return r;
    }
    if (!init_ok_) return RP_ERR_DEVICE;
    rp_result r = rebuild_surfaces(err);
    if (r != RP_OK) return r;
    if (loader_.state() == LoaderState::Created) return loader_.start(err);
    return RP_OK;
}

rp_result Runtime::load_core(const std::string& core_dir) {
    if (!init_ok_) return RP_ERR_DEVICE;
    if (core_loaded_ || loader_.state() != LoaderState::Unloaded) unload_core();

    std::string manifest_path = core_dir + "/core.json";
    std::ifstream f(manifest_path, std::ios::binary);
    if (!f) return RP_ERR_NOT_FOUND;
    std::stringstream ss; ss << f.rdbuf();
    CoreManifest m; std::string err;
    if (parse_manifest(ss.str(), m, err) != RP_OK) return RP_ERR_BAD_ARG;
    if (m.type == RP_CORE_PRESENTING && m.graphics_api != api_) return RP_ERR_UNSUPPORTED; // presenting core must match runtime's backend api

    std::string dll = core_dir + "/" + m.entry;
    std::unique_ptr<Win32CoreModule> mod;
    if (Win32CoreModule::open(dll, mod, err) != RP_OK) return RP_ERR_NOT_FOUND;
    module_ = std::move(mod);
    if (loader_.load(module_.get(), err) != RP_OK) {
        module_.reset();
        return RP_ERR_ABI_MISMATCH;
    }
    if (loader_.create(&host_iface_, err) != RP_OK) {
        loader_.destroy();
        module_.reset();
        return RP_ERR_INTERNAL;
    }
    return finish_load_core(m.type, err);   // `m` = the CoreManifest; its .type feeds the shared branch logic
}

rp_result Runtime::finish_load_core(rp_core_type type, std::string& err) {
    core_loaded_ = true;
    core_type_ = type;

    if (type == RP_CORE_DRIVEN) {
        requires_content_ = loader_.has_load_content();
        content_loaded_ = false;
        if (!requires_content_) {
            rp_av_info av{};
            rp_result r = loader_.get_av_info(&av, err);
            if (r != RP_OK) { unload_core(); return r; }
            if (!(av.base_width > 0 && av.base_height > 0)) { unload_core(); return RP_ERR_UNSUPPORTED; }
            if (av.pixel_format != RP_FMT_R8G8B8A8_UNORM) { unload_core(); return RP_ERR_UNSUPPORTED; }
            // A core reporting max geometry smaller than its base geometry is malformed; clamp
            // so a well-behaved base-size frame is never wrongly rejected as oversize.
            dr_max_w_ = std::max(av.max_width, av.base_width);
            dr_max_h_ = std::max(av.max_height, av.base_height);
            open_audio(av);
        }
        return RP_OK;
    }

    // Presenting core. A presenting core that takes content (e.g. dolphin_present) must be started
    // AFTER load_content, so defer start until then; a content-free presenting core starts now.
    requires_content_ = loader_.has_load_content();
    content_loaded_ = false;
    rp_result r = rebuild_surfaces(err);
    if (r != RP_OK) {
        unload_core();
        return r;
    }
    if (!requires_content_) {
        r = loader_.start(err);
        if (r != RP_OK) {
            unload_core();
            return r;
        }
    }
    return RP_OK;
}

rp_result Runtime::load_static_core(const std::string& core_id) {
    if (!init_ok_) return RP_ERR_DEVICE;
    if (core_loaded_ || loader_.state() != LoaderState::Unloaded) unload_core();
    rp_get_core_abi_fn getter = StaticCoreRegistry::get(core_id);
    if (!getter) return RP_ERR_NOT_FOUND;
    std::string err;
    module_ = std::make_unique<StaticCoreModule>(getter);
    if (loader_.load(module_.get(), err) != RP_OK) { module_.reset(); return RP_ERR_ABI_MISMATCH; }
    rp_core_info info{};
    if (loader_.abi() && loader_.abi()->get_info) loader_.abi()->get_info(&info);
    else { loader_.destroy(); module_.reset(); return RP_ERR_INTERNAL; }
    if (info.type == RP_CORE_PRESENTING && (rp_graphics_api)info.graphics_api != api_) {
        loader_.destroy(); module_.reset(); return RP_ERR_UNSUPPORTED;   // presenting core must match runtime api
    }
    if (loader_.create(&host_iface_, err) != RP_OK) { loader_.destroy(); module_.reset(); return RP_ERR_INTERNAL; }
    return finish_load_core((rp_core_type)info.type, err);
}

rp_result Runtime::unload_core() {
    input_polls_.store(0, std::memory_order_relaxed);
    if (!core_loaded_ && loader_.state() == LoaderState::Unloaded) return RP_OK;
    loader_.destroy();
    module_.reset();
    core_loaded_ = false;
    core_type_ = RP_CORE_PRESENTING;
    dr_data_ = nullptr; dr_w_ = 0; dr_h_ = 0; dr_pitch_ = 0;
    dr_dupe_ = false; dr_have_ = false;
    dr_max_w_ = 0; dr_max_h_ = 0;
    requires_content_ = false; content_loaded_ = false;
    if (audio_) { audio_->close(); audio_.reset(); }
    audio_frames_ = 0; audio_nonsilent_ = false;
    rewind_ring_.clear();
    rewind_enabled_ = false; rewind_replay_ = false; rewind_max_ = 0;
    return RP_OK;
}

rp_result Runtime::load_content(const char* path) {
    if (!core_loaded_) return RP_ERR_INTERNAL;
    std::string err;
    if (core_type_ == RP_CORE_PRESENTING) {
        // A presenting content core (e.g. dolphin_present): hand it the content, then start it —
        // load_core deferred start precisely so start() sees the ISO. Surfaces were already set.
        if (!requires_content_) return RP_ERR_INTERNAL; // content-free presenting core takes no content
        rp_result r = loader_.load_content(path ? path : "", err);
        if (r != RP_OK) return r;
        content_loaded_ = true;
        if (loader_.state() != LoaderState::Started) {
            r = loader_.start(err);
            if (r != RP_OK) return r;
        }
        // A presenting core that produces audio reports its rate via get_av_info (e.g. dolphin_present
        // pulls Dolphin's 48 kHz mix and forwards it through audio_sample). Open host audio best-effort;
        // a presenting core without get_av_info (refcore_present_vk) returns UNSUPPORTED and stays silent.
        rp_av_info av{};
        if (loader_.get_av_info(&av, err) == RP_OK && av.sample_rate > 0.0)
            open_audio(av);
        return RP_OK;
    }
    if (core_type_ != RP_CORE_DRIVEN) return RP_ERR_INTERNAL;
    rp_result r = loader_.load_content(path ? path : "", err);
    if (r != RP_OK) return r;
    rp_av_info av{};
    if (loader_.get_av_info(&av, err) != RP_OK) return RP_ERR_INTERNAL;
    if (av.base_width == 0 || av.base_height == 0) return RP_ERR_UNSUPPORTED;
    if (av.pixel_format != RP_FMT_R8G8B8A8_UNORM) return RP_ERR_UNSUPPORTED;
    dr_max_w_ = std::max(av.max_width, av.base_width);
    dr_max_h_ = std::max(av.max_height, av.base_height);
    content_loaded_ = true;
    open_audio(av);
    return RP_OK;
}

rp_result Runtime::advance(int emit_audio) {
    if (!backend_) return RP_ERR_DEVICE;
    if (!(core_loaded_ && core_type_ == RP_CORE_DRIVEN)) return RP_ERR_UNSUPPORTED;
    if (requires_content_ && !content_loaded_) return RP_ERR_INTERNAL;
    std::string err;
    // Rewind ring capture (forward frames only). A present that re-renders a rewound-to
    // frame (rewind_replay_) must NOT capture — that would re-grow the ring with the very
    // frames we are stepping back through. Clearing the flag here means the NEXT forward
    // present resumes capturing cleanly from the restored point (spec §2, e2e step 5).
    if (rewind_replay_) {
        rewind_replay_ = false;
    } else if (rewind_enabled_) {
        size_t sz = loader_.serialize_size();
        if (sz > 0) {
            std::vector<uint8_t> snap(sz);
            std::string serr;
            // A serialize failure mid-capture just skips this snapshot — never crash.
            if (loader_.serialize(snap.data(), sz, serr) == RP_OK)
                rewind_ring_push(rewind_ring_, std::move(snap), rewind_max_);
        }
    }
    dr_have_ = false;
    suppress_audio_ = (emit_audio == 0);
    rp_result r = loader_.run_frame(err);
    suppress_audio_ = false;               // always clear, even on failure
    return r;
}

rp_result Runtime::render(uint8_t* out_rgba) {
    if (!backend_) return RP_ERR_DEVICE;
    std::string err;
    if (core_loaded_ && core_type_ == RP_CORE_DRIVEN) {
        // Spec §4: a frame with a too-small pitch or dimensions beyond the core's declared
        // max geometry is skipped (treated as a duplicate of the last good frame) rather
        // than uploaded.
        bool valid = dr_have_ && !dr_dupe_ &&
                     driven_frame_valid(dr_w_, dr_h_, dr_pitch_, dr_max_w_, dr_max_h_);
        bool dupe = !valid;
        return backend_->composite_driven(valid ? dr_data_ : nullptr,
                                          dr_w_, dr_h_, dr_pitch_, dupe, out_rgba, err);
    }
    uint32_t idx = 0; uint64_t sv = 0;
    bool has = ring_.latest_ready(idx, sv);
    return backend_->composite_and_present(idx, sv, has, out_rgba, err);
}

rp_result Runtime::present(uint8_t* out_rgba) {
    if (core_loaded_ && core_type_ == RP_CORE_DRIVEN) {
        rp_result r = advance(1);
        if (r != RP_OK) return r;
        return render(out_rgba);
    }
    return render(out_rgba);                // presenting: composite_and_present, unchanged
}

size_t Runtime::serialize_size() {
    return loader_.serialize_size();
}

rp_result Runtime::save_state(void* buf, size_t size) {
    if (!buf) return RP_ERR_BAD_ARG;
    size_t sz = loader_.serialize_size();
    if (sz == 0) return RP_ERR_UNSUPPORTED;
    if (size < sz) return RP_ERR_BAD_ARG;
    std::string err;
    return loader_.serialize(buf, sz, err);
}

rp_result Runtime::load_state(const void* buf, size_t size) {
    if (!buf) return RP_ERR_BAD_ARG;
    if (size == 0) return RP_ERR_BAD_ARG;
    std::string err;
    return loader_.unserialize(buf, size, err);
}

rp_result Runtime::set_rewind(int enabled, uint32_t max_snapshots) {
    // Rewind only makes sense for a serialize-capable (driven) core — the host owning the
    // state is the driven model's whole premise. A no-serialize / presenting / no core -> 0.
    if (loader_.serialize_size() == 0) return RP_ERR_UNSUPPORTED;
    rewind_enabled_ = (enabled != 0);
    rewind_max_ = max_snapshots ? max_snapshots : 600;   // sane default (~10s at 60fps)
    rewind_ring_.clear();
    rewind_replay_ = false;
    return RP_OK;
}

rp_result Runtime::rewind() {
    if (!rewind_enabled_) return RP_ERR_INTERNAL;
    // Need at least two snapshots: the newest is the pre-state of the just-displayed frame; we
    // discard it and step back to the previous frame's pre-state. Fewer than two = no history.
    if (rewind_ring_.size() < 2) return RP_ERR_NOT_FOUND;
    rewind_ring_.pop_back();                     // drop the just-displayed frame's pre-state
    const std::vector<uint8_t>& snap = rewind_ring_.back();
    std::string err;
    rp_result r = loader_.unserialize(snap.data(), snap.size(), err);
    // The next present() re-renders this restored frame; flag it so that present does NOT
    // capture (which would re-grow the ring by replaying frames we are stepping back through).
    rewind_replay_ = true;
    return r;
}

} // namespace rp

// ---- C API ----
using rp::Runtime;
extern "C" {

rp_runtime* rp_runtime_create(rp_graphics_api api, void* native_window) {
    return reinterpret_cast<rp_runtime*>(new Runtime(api, native_window));
}
void rp_runtime_destroy(rp_runtime* rt) { delete reinterpret_cast<Runtime*>(rt); }
rp_result rp_runtime_load_core(rp_runtime* rt, const char* dir) {
    return reinterpret_cast<Runtime*>(rt)->load_core(dir ? dir : "");
}
rp_result rp_runtime_load_static_core(rp_runtime* rt, const char* core_id) {
    return reinterpret_cast<Runtime*>(rt)->load_static_core(core_id ? core_id : "");
}
rp_result rp_runtime_unload_core(rp_runtime* rt) {
    return reinterpret_cast<Runtime*>(rt)->unload_core();
}
rp_result rp_runtime_load_content(rp_runtime* rt, const char* path) {
    return reinterpret_cast<Runtime*>(rt)->load_content(path);
}
rp_result rp_runtime_resize(rp_runtime* rt, uint32_t w, uint32_t h) {
    return reinterpret_cast<Runtime*>(rt)->resize(w, h);
}
void rp_runtime_set_input(rp_runtime* rt, uint32_t port, const rp_input_state* in) {
    if (in) reinterpret_cast<Runtime*>(rt)->set_input(port, *in);
}
uint64_t rp_runtime_input_poll_count(rp_runtime* rt) {
    return reinterpret_cast<Runtime*>(rt)->input_polls();
}
rp_result rp_runtime_present(rp_runtime* rt, uint8_t* out_rgba) {
    return reinterpret_cast<Runtime*>(rt)->present(out_rgba);
}
rp_result rp_runtime_advance(rp_runtime* rt, int emit_audio) {
    return reinterpret_cast<Runtime*>(rt)->advance(emit_audio);
}
rp_result rp_runtime_render(rp_runtime* rt, uint8_t* out_rgba) {
    return reinterpret_cast<Runtime*>(rt)->render(out_rgba);
}
void rp_runtime_audio_stats(rp_runtime* rt, uint64_t* frames_out, int* nonsilent_out) {
    auto* r = reinterpret_cast<Runtime*>(rt);
    if (frames_out) *frames_out = r->audio_frames();
    if (nonsilent_out) *nonsilent_out = r->audio_nonsilent() ? 1 : 0;
}
size_t rp_runtime_serialize_size(rp_runtime* rt) {
    if (!rt) return 0;
    return reinterpret_cast<Runtime*>(rt)->serialize_size();
}
rp_result rp_runtime_save_state(rp_runtime* rt, void* buf, size_t size) {
    if (!rt || !buf) return RP_ERR_BAD_ARG;
    return reinterpret_cast<Runtime*>(rt)->save_state(buf, size);
}
rp_result rp_runtime_load_state(rp_runtime* rt, const void* buf, size_t size) {
    if (!rt || !buf) return RP_ERR_BAD_ARG;
    return reinterpret_cast<Runtime*>(rt)->load_state(buf, size);
}
rp_result rp_runtime_set_rewind(rp_runtime* rt, int enabled, uint32_t max_snapshots) {
    if (!rt) return RP_ERR_BAD_ARG;
    return reinterpret_cast<Runtime*>(rt)->set_rewind(enabled, max_snapshots);
}
rp_result rp_runtime_rewind(rp_runtime* rt) {
    if (!rt) return RP_ERR_BAD_ARG;
    return reinterpret_cast<Runtime*>(rt)->rewind();
}
}
