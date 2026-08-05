#include "runtime/Runtime.h"
#include "loader/Manifest.h"
#include <fstream>
#include <sstream>
#include <cstring>

namespace rp {

static void host_log(rp_host*, int, const char*) {}
static void host_submit(rp_host* h, uint32_t i, uint64_t g) {
    reinterpret_cast<Runtime*>(h)->on_submit(i, g);
}
static void host_input(rp_host* h, rp_input_state* out) {
    reinterpret_cast<Runtime*>(h)->on_input(out);
}

Runtime::Runtime(rp_graphics_api, void* native_window) : native_window_(native_window) {
    backend_ = std::make_unique<D3D11Backend>();
    std::string err;
    backend_->initialize(native_window_, width_, height_, err);
    host_iface_.host = reinterpret_cast<rp_host*>(this);
    host_iface_.log = host_log;
    host_iface_.submit_frame = host_submit;
    host_iface_.input_state = host_input;
}

Runtime::~Runtime() { unload_core(); }

void Runtime::on_submit(uint32_t index, uint64_t generation) {
    ring_.accept_submit(index, generation);
}
void Runtime::on_input(rp_input_state* out) {
    std::lock_guard<std::mutex> lk(input_mtx_);
    *out = input_;
}
void Runtime::set_input(const rp_input_state& in) {
    std::lock_guard<std::mutex> lk(input_mtx_);
    input_ = in;
}

rp_result Runtime::rebuild_surfaces(std::string& err) {
    std::vector<rp_surface_desc> descs;
    rp_result r = backend_->allocate_surfaces(ring_.slot_count(), width_, height_, descs, err);
    if (r != RP_OK) return r;
    uint64_t gen = ring_.reallocate(width_, height_);
    for (auto& d : descs) d.generation = gen;
    if (loader_.state() == LoaderState::Started) { std::string e; loader_.stop(e); }
    if (loader_.state() == LoaderState::Created)
        return loader_.set_surfaces(descs.data(), (uint32_t)descs.size(), err);
    return RP_OK;
}

rp_result Runtime::resize(uint32_t w, uint32_t h) {
    width_ = w; height_ = h;
    std::string err;
    if (!core_loaded_) return backend_->initialize(native_window_, w, h, err);
    rp_result r = rebuild_surfaces(err);
    if (r != RP_OK) return r;
    if (loader_.state() == LoaderState::Created) return loader_.start(err);
    return RP_OK;
}

rp_result Runtime::load_core(const std::string& core_dir) {
    std::string manifest_path = core_dir + "/core.json";
    std::ifstream f(manifest_path, std::ios::binary);
    if (!f) return RP_ERR_NOT_FOUND;
    std::stringstream ss; ss << f.rdbuf();
    CoreManifest m; std::string err;
    if (parse_manifest(ss.str(), m, err) != RP_OK) return RP_ERR_BAD_ARG;
    if (m.type != RP_CORE_PRESENTING) return RP_ERR_UNSUPPORTED; // driven not in Slice A

    std::string dll = core_dir + "/" + m.entry;
    if (Win32CoreModule::open(dll, module_, err) != RP_OK) return RP_ERR_NOT_FOUND;
    if (loader_.load(module_.get(), err) != RP_OK) return RP_ERR_ABI_MISMATCH;
    if (loader_.create(&host_iface_, err) != RP_OK) return RP_ERR_INTERNAL;

    core_loaded_ = true;
    rp_result r = rebuild_surfaces(err);
    if (r != RP_OK) return r;
    return loader_.start(err);
}

rp_result Runtime::unload_core() {
    if (!core_loaded_) return RP_OK;
    loader_.destroy();
    module_.reset();
    core_loaded_ = false;
    return RP_OK;
}

rp_result Runtime::present(uint8_t* out_rgba) {
    uint32_t idx = 0;
    bool has = ring_.latest_ready(idx);
    std::string err;
    return backend_->composite_and_present(idx, has, out_rgba, err);
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
