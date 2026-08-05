#include <doctest/doctest.h>
#include "render/IRenderBackend.h"

TEST_CASE("backend: interface is abstract and includes cleanly") {
    // A no-op subclass proves the vtable shape compiles.
    struct Stub : rp::IRenderBackend {
        rp_result initialize(void*, uint32_t, uint32_t, std::string&) override { return RP_OK; }
        rp_result allocate_surfaces(uint32_t, uint32_t, uint32_t,
                                    std::vector<rp_surface_desc>&, std::string&) override { return RP_OK; }
        rp_result composite_and_present(uint32_t, bool, uint8_t*, std::string&) override { return RP_OK; }
    } s;
    std::string e;
    CHECK(s.initialize(nullptr, 1, 1, e) == RP_OK);
}
