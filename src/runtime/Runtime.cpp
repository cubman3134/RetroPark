#include "runtime/Runtime.h"
#include "runtime/BackendFactory.h"
#include "loader/Manifest.h"
#include "render/FramebufferCopy.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>

namespace rp {

static void host_log(rp_host*, int, const char*) {}
static void host_submit(rp_host* h, uint32_t i, uint64_t g, uint64_t sv) {
    reinterpret_cast<Runtime*>(h)->on_submit(i, g, sv);
}
static void host_input(rp_host* h, rp_input_state* out) {
    reinterpret_cast<Runtime*>(h)->on_input(out);
}
static void host_video_refresh(rp_host* h, const void* d, uint32_t w, uint32_t hh, uint32_t p) {
    reinterpret_cast<Runtime*>(h)->on_video_refresh(d, w, hh, p);
}
static void host_audio_sample(rp_host*, const int16_t*, size_t) {}

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
void Runtime::on_input(rp_input_state* out) {
    std::lock_guard<std::mutex> lk(input_mtx_);
    *out = input_;
}
void Runtime::set_input(const rp_input_state& in) {
    std::lock_guard<std::mutex> lk(input_mtx_);
    input_ = in;
}
void Runtime::on_video_refresh(const void* data, uint32_t w, uint32_t h, uint32_t pitch) {
    dr_have_ = true;
    dr_dupe_ = (data == nullptr);
    dr_data_ = data;
    dr_w_ = w; dr_h_ = h; dr_pitch_ = pitch;
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
    if (Win32CoreModule::open(dll, module_, err) != RP_OK) return RP_ERR_NOT_FOUND;
    if (loader_.load(module_.get(), err) != RP_OK) {
        module_.reset();
        return RP_ERR_ABI_MISMATCH;
    }
    if (loader_.create(&host_iface_, err) != RP_OK) {
        loader_.destroy();
        module_.reset();
        return RP_ERR_INTERNAL;
    }

    core_loaded_ = true;
    core_type_ = m.type;

    if (core_type_ == RP_CORE_DRIVEN) {
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
        }
        return RP_OK;
    }

    rp_result r = rebuild_surfaces(err);
    if (r != RP_OK) {
        unload_core();
        return r;
    }
    r = loader_.start(err);
    if (r != RP_OK) {
        unload_core();
        return r;
    }
    return RP_OK;
}

rp_result Runtime::unload_core() {
    if (!core_loaded_ && loader_.state() == LoaderState::Unloaded) return RP_OK;
    loader_.destroy();
    module_.reset();
    core_loaded_ = false;
    core_type_ = RP_CORE_PRESENTING;
    dr_data_ = nullptr; dr_w_ = 0; dr_h_ = 0; dr_pitch_ = 0;
    dr_dupe_ = false; dr_have_ = false;
    dr_max_w_ = 0; dr_max_h_ = 0;
    requires_content_ = false; content_loaded_ = false;
    return RP_OK;
}

rp_result Runtime::load_content(const char* path) {
    if (!core_loaded_ || core_type_ != RP_CORE_DRIVEN) return RP_ERR_INTERNAL;
    std::string err;
    rp_result r = loader_.load_content(path ? path : "", err);
    if (r != RP_OK) return r;
    rp_av_info av{};
    if (loader_.get_av_info(&av, err) != RP_OK) return RP_ERR_INTERNAL;
    if (av.base_width == 0 || av.base_height == 0) return RP_ERR_UNSUPPORTED;
    if (av.pixel_format != RP_FMT_R8G8B8A8_UNORM) return RP_ERR_UNSUPPORTED;
    dr_max_w_ = std::max(av.max_width, av.base_width);
    dr_max_h_ = std::max(av.max_height, av.base_height);
    content_loaded_ = true;
    return RP_OK;
}

rp_result Runtime::present(uint8_t* out_rgba) {
    if (!backend_) return RP_ERR_DEVICE;
    std::string err;
    if (core_loaded_ && core_type_ == RP_CORE_DRIVEN) {
        if (requires_content_ && !content_loaded_) return RP_ERR_INTERNAL;
        dr_have_ = false;
        rp_result r = loader_.run_frame(err);
        if (r != RP_OK) return r;
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
rp_result rp_runtime_unload_core(rp_runtime* rt) {
    return reinterpret_cast<Runtime*>(rt)->unload_core();
}
rp_result rp_runtime_load_content(rp_runtime* rt, const char* path) {
    return reinterpret_cast<Runtime*>(rt)->load_content(path);
}
rp_result rp_runtime_resize(rp_runtime* rt, uint32_t w, uint32_t h) {
    return reinterpret_cast<Runtime*>(rt)->resize(w, h);
}
void rp_runtime_set_input(rp_runtime* rt, const rp_input_state* in) {
    if (in) reinterpret_cast<Runtime*>(rt)->set_input(*in);
}
rp_result rp_runtime_present(rp_runtime* rt, uint8_t* out_rgba) {
    return reinterpret_cast<Runtime*>(rt)->present(out_rgba);
}
}
