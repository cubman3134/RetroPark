#include <doctest/doctest.h>
#include "loader/CoreLoader.h"
#include <retropark/retropark_abi.h>
#include <cstring>

using namespace rp;

// ---- A fake core implemented inline, exposed through a fake module ----
namespace {
struct FakeCoreState { bool created=false, started=false; uint32_t surfaces=0; };
FakeCoreState g_fake;

void fake_get_info(rp_core_info* out){
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_PRESENTING;
    out->graphics_api = RP_GFX_D3D11;
    out->id = "fake";
}
rp_core* fake_create(const rp_host_iface*){ g_fake.created=true; return reinterpret_cast<rp_core*>(&g_fake); }
void      fake_destroy(rp_core*){ g_fake.created=false; }
rp_result fake_set_surfaces(rp_core*, const rp_surface_set* set){ g_fake.surfaces=set->count; return RP_OK; }
rp_result fake_start(rp_core*){ g_fake.started=true; return RP_OK; }
rp_result fake_stop(rp_core*){ g_fake.started=false; return RP_OK; }

const rp_core_abi kGoodAbi = {
    RETROPARK_ABI_VERSION, fake_get_info, fake_create, fake_destroy,
    fake_set_surfaces, fake_start, fake_stop,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};
const rp_core_abi kBadVersionAbi = {
    999u, fake_get_info, fake_create, fake_destroy, fake_set_surfaces, fake_start, fake_stop,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};
const rp_core_abi* good_entry(){ return &kGoodAbi; }
const rp_core_abi* bad_entry(){ return &kBadVersionAbi; }

struct FakeModule : ICoreModule {
    rp_get_core_abi_fn fn;
    bool return_null_symbol = false;
    explicit FakeModule(rp_get_core_abi_fn f): fn(f) {}
    void* resolve(const char* symbol) override {
        if (return_null_symbol) return nullptr;
        if (std::strcmp(symbol, RP_CORE_ABI_EXPORT_NAME) == 0) return reinterpret_cast<void*>(fn);
        return nullptr;
    }
};
}

TEST_CASE("loader: happy path advances states") {
    g_fake = {};
    CoreLoader ld; std::string err;
    FakeModule mod(good_entry);
    CHECK(ld.load(&mod, err) == RP_OK);
    CHECK(ld.state() == LoaderState::Loaded);

    rp_host_iface host{}; host.host = nullptr;
    CHECK(ld.create(&host, err) == RP_OK);
    CHECK(ld.state() == LoaderState::Created);
    CHECK(g_fake.created);

    rp_surface_desc descs[2] = {};
    rp_surface_set set{};
    set.count = 2; set.surfaces = descs;
    CHECK(ld.set_surfaces(&set, err) == RP_OK);
    CHECK(g_fake.surfaces == 2u);

    CHECK(ld.start(err) == RP_OK);
    CHECK(ld.state() == LoaderState::Started);
    CHECK(g_fake.started);

    CHECK(ld.stop(err) == RP_OK);
    CHECK(ld.state() == LoaderState::Created);
    ld.destroy();
    CHECK(ld.state() == LoaderState::Unloaded);
    CHECK(!g_fake.created);
}

TEST_CASE("loader: abi version mismatch rejected at load") {
    CoreLoader ld; std::string err;
    FakeModule mod(bad_entry);
    CHECK(ld.load(&mod, err) == RP_ERR_ABI_MISMATCH);
    CHECK(ld.state() == LoaderState::Unloaded);
}

TEST_CASE("loader: missing export rejected") {
    CoreLoader ld; std::string err;
    FakeModule mod(good_entry);
    mod.return_null_symbol = true;
    CHECK(ld.load(&mod, err) == RP_ERR_NOT_FOUND);
    CHECK(ld.state() == LoaderState::Unloaded);
}

TEST_CASE("loader: create before load is rejected") {
    CoreLoader ld; std::string err; rp_host_iface host{};
    CHECK(ld.create(&host, err) == RP_ERR_INTERNAL);
}

TEST_CASE("loader: start before create is rejected") {
    CoreLoader ld; std::string err;
    FakeModule mod(good_entry);
    CHECK(ld.load(&mod, err) == RP_OK);
    CHECK(ld.start(err) == RP_ERR_INTERNAL);
}
