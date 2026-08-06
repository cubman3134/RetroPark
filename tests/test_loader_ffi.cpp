#include <doctest/doctest.h>
#include "loader/Win32CoreModule.h"
#include "loader/CoreLoader.h"
#include <memory>

using namespace rp;

// Directory of the test exe; the mock_core dll is emitted alongside it.
static std::string mock_dll_path() {
#if defined(_WIN32)
    return "mock_core.dll";     // same dir as the test exe; CWD is set by ctest WORKING_DIRECTORY
#else
    return "./libmock_core.so";
#endif
}

TEST_CASE("ffi: load real mock core dll through the full lifecycle") {
    std::unique_ptr<Win32CoreModule> mod;
    std::string err;
    REQUIRE(Win32CoreModule::open(mock_dll_path(), mod, err) == RP_OK);

    CoreLoader ld;
    CHECK(ld.load(mod.get(), err) == RP_OK);
    rp_host_iface host{};
    CHECK(ld.create(&host, err) == RP_OK);
    rp_surface_desc d[3] = {};
    rp_surface_set set{};
    set.count = 3; set.surfaces = d;
    CHECK(ld.set_surfaces(&set, err) == RP_OK);
    CHECK(ld.start(err) == RP_OK);
    CHECK(ld.stop(err) == RP_OK);
    ld.destroy();
    CHECK(ld.state() == LoaderState::Unloaded);
}

TEST_CASE("ffi: opening a missing dll fails cleanly") {
    std::unique_ptr<Win32CoreModule> mod;
    std::string err;
    CHECK(Win32CoreModule::open("does_not_exist.dll", mod, err) == RP_ERR_NOT_FOUND);
    CHECK(mod == nullptr);
}
