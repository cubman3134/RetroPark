#include <doctest/doctest.h>
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <vector>
#include <cstring>
#include <string>
using namespace rp;
static rp_runtime* c(Runtime* r){ return reinterpret_cast<rp_runtime*>(r); }

#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif

// Load refcore_driven into a runtime (lifted from tests/test_driven_e2e.cpp's setup sequence:
// resize then load_core, called directly on the C++ object since this test already holds one).
static void load_refcore_driven(Runtime& rt) {
    REQUIRE(rt.resize(64, 64) == RP_OK);
    REQUIRE(rt.load_core(RP_DRIVEN_CORE_DIR) == RP_OK);
}

TEST_CASE("runtime: advance advances, render does not; present == advance+render") {
    Runtime rt(RP_GFX_D3D11, nullptr);         // WARP, device-free (as Slice F portable e2e)
    load_refcore_driven(rt);
    auto frame_of = [&]() -> uint32_t {
        uint32_t f = 0; size_t sz = rp_runtime_serialize_size(c(&rt));
        REQUIRE(sz == sizeof(uint32_t));
        REQUIRE(rp_runtime_save_state(c(&rt), &f, sizeof(f)) == RP_OK);
        return f;
    };
    std::vector<uint8_t> out(64 * 64 * 4, 0);
    uint32_t f0 = frame_of();
    REQUIRE(rp_runtime_advance(c(&rt), 1) == RP_OK);
    CHECK(frame_of() == f0 + 1);               // advance advanced the sim one frame
    REQUIRE(rp_runtime_render(c(&rt), out.data()) == RP_OK);
    CHECK(frame_of() == f0 + 1);               // render did NOT advance
    REQUIRE(rp_runtime_render(c(&rt), out.data()) == RP_OK);
    CHECK(frame_of() == f0 + 1);               // render is idempotent on state
    REQUIRE(rp_runtime_present(c(&rt), out.data()) == RP_OK);
    CHECK(frame_of() == f0 + 2);               // present advances exactly one frame
}
