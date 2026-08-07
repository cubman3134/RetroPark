#include "loader/CoreLoader.h"

namespace rp {

rp_result CoreLoader::load(ICoreModule* module, std::string& error) {
    if (state_ != LoaderState::Unloaded) { error = "already loaded"; return RP_ERR_INTERNAL; }
    if (!module) { error = "null module"; return RP_ERR_BAD_ARG; }
    auto fn = reinterpret_cast<rp_get_core_abi_fn>(module->resolve(RP_CORE_ABI_EXPORT_NAME));
    if (!fn) { error = "core does not export " RP_CORE_ABI_EXPORT_NAME; return RP_ERR_NOT_FOUND; }
    const rp_core_abi* abi = fn();
    if (!abi) { error = "core returned null abi"; return RP_ERR_INTERNAL; }
    if (abi->abi_version != RETROPARK_ABI_VERSION) {
        error = "abi version mismatch"; return RP_ERR_ABI_MISMATCH;
    }
    module_ = module; abi_ = abi; state_ = LoaderState::Loaded;
    return RP_OK;
}

rp_result CoreLoader::create(const rp_host_iface* host, std::string& error) {
    if (state_ != LoaderState::Loaded) { error = "create requires Loaded"; return RP_ERR_INTERNAL; }
    if (!abi_->create) { error = "core missing create"; return RP_ERR_INTERNAL; }
    core_ = abi_->create(host);
    if (!core_) { error = "core create returned null"; return RP_ERR_INTERNAL; }
    state_ = LoaderState::Created;
    return RP_OK;
}

rp_result CoreLoader::set_surfaces(const rp_surface_set* set, std::string& error) {
    if (state_ != LoaderState::Created) { error = "set_surfaces requires Created"; return RP_ERR_INTERNAL; }
    if (!abi_->set_surfaces) { error = "core missing set_surfaces"; return RP_ERR_UNSUPPORTED; }
    return abi_->set_surfaces(core_, set);
}

rp_result CoreLoader::start(std::string& error) {
    if (state_ != LoaderState::Created) { error = "start requires Created"; return RP_ERR_INTERNAL; }
    rp_result r = abi_->start ? abi_->start(core_) : RP_ERR_UNSUPPORTED;
    if (r == RP_OK) state_ = LoaderState::Started;
    return r;
}

rp_result CoreLoader::stop(std::string& error) {
    if (state_ != LoaderState::Started) { error = "stop requires Started"; return RP_ERR_INTERNAL; }
    rp_result r = abi_->stop ? abi_->stop(core_) : RP_OK;
    state_ = LoaderState::Created;
    return r;
}

rp_result CoreLoader::run_frame(std::string& error) {
    if (state_ != LoaderState::Created && state_ != LoaderState::Started) { error = "run_frame needs Created"; return RP_ERR_INTERNAL; }
    if (!abi_->run_frame) { error = "core has no run_frame"; return RP_ERR_UNSUPPORTED; }
    abi_->run_frame(core_);
    return RP_OK;
}

rp_result CoreLoader::get_av_info(rp_av_info* out, std::string& error) {
    if (state_ != LoaderState::Created && state_ != LoaderState::Started) { error = "get_av_info needs Created"; return RP_ERR_INTERNAL; }
    if (!abi_->get_av_info) { error = "core has no get_av_info"; return RP_ERR_UNSUPPORTED; }
    abi_->get_av_info(core_, out);
    return RP_OK;
}

void CoreLoader::destroy() {
    if (state_ == LoaderState::Started) { std::string e; stop(e); }
    if (core_ && abi_ && abi_->destroy) abi_->destroy(core_);
    core_ = nullptr; abi_ = nullptr; module_ = nullptr;
    state_ = LoaderState::Unloaded;
}
}
