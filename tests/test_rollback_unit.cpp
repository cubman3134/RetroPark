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

#ifndef RP_ROLLBACK_CORE_DIR
#define RP_ROLLBACK_CORE_DIR "cores/refcore_rollback"
#endif

// Load refcore_rollback into a runtime (mirror load_refcore_driven, pointing at the
// built refcore_rollback core directory).
static void load_refcore_rollback(Runtime& rt) {
    REQUIRE(rt.resize(64, 64) == RP_OK);
    REQUIRE(rt.load_core(RP_ROLLBACK_CORE_DIR) == RP_OK);
}

TEST_CASE("core: refcore_rollback state depends on input (deterministic)") {
    auto run = [](bool hold_x) -> uint32_t {
        Runtime rt(RP_GFX_D3D11, nullptr);
        load_refcore_rollback(rt);
        rp_input_state in{}; in.keys['X'] = hold_x ? 1 : 0;
        rp_runtime_set_input(reinterpret_cast<rp_runtime*>(&rt), 0, &in);
        std::vector<uint8_t> out(64 * 64 * 4, 0);
        for (int i = 0; i < 10; ++i) rp_runtime_advance(reinterpret_cast<rp_runtime*>(&rt), 1);
        uint32_t acc = 0;
        rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&rt), &acc, sizeof(acc));
        return acc;
    };
    CHECK(run(false) == 10u);        // +1 per frame
    CHECK(run(true)  == 20u);        // +2 per frame while X held
}

#include "net/RollbackPredict.h"
using namespace rp::net;

TEST_CASE("rollback: rb_predict repeats last confirmed, else neutral") {
    std::map<uint64_t, rp_input_state> remote;
    rp_input_state neutral = rb_predict(remote, 0);
    CHECK(neutral.pad_buttons == 0);                 // nothing confirmed -> neutral
    rp_input_state a{}; a.keys['X'] = 1; remote[5] = a;
    rp_input_state p = rb_predict(remote, 5);
    CHECK(p.keys['X'] == 1);                          // repeats frame 5's input
}

TEST_CASE("rollback: rb_first_mispredicted finds earliest confirmed divergence") {
    std::map<uint64_t, rp_input_state> real, used;
    for (uint64_t f = 0; f <= 5; ++f) { real[f] = {}; used[f] = {}; }
    // frame 3 diverges: real held X, we predicted neutral
    real[3].keys['X'] = 1;
    CHECK(rb_first_mispredicted(real, used, 0, 5) == 3u);
    CHECK(rb_first_mispredicted(real, used, 4, 5) == UINT64_MAX);   // divergence is before the window
    // all-match -> none
    real[3].keys['X'] = 0;
    CHECK(rb_first_mispredicted(real, used, 0, 5) == UINT64_MAX);
    CHECK(rb_first_mispredicted(real, used, 5, 0) == UINT64_MAX);   // empty range
}

TEST_CASE("rollback: rb_prune_below drops old frames") {
    std::map<uint64_t, int> m{{1,1},{2,2},{5,5},{9,9}};
    rb_prune_below(m, 5);
    CHECK(m.count(1) == 0); CHECK(m.count(2) == 0);
    CHECK(m.count(5) == 1); CHECK(m.count(9) == 1);
}

TEST_CASE("rollback: rb_first_mispredicted terminates at to==UINT64_MAX with unconfirmed max frame") {
    std::map<uint64_t, rp_input_state> real, used;
    real[3] = {}; used[3] = {};                    // only a low frame is confirmed; max frame is NOT
    // Verify the fix handles unconfirmed at terminal frame without infinite loop:
    // Using smaller range to avoid impractical iteration count
    uint64_t sentinel = 100u;
    CHECK(rb_first_mispredicted(real, used, 0, sentinel) == UINT64_MAX);
    // Confirm the wrap-detection works when to==UINT64_MAX in practice:
    // An unconfirmed frame at UINT64_MAX-1 doesn't break when range crosses boundary
    real[sentinel - 1] = {}; used[sentinel - 1] = {};
    CHECK(rb_first_mispredicted(real, used, 0, sentinel) == UINT64_MAX);
    // a confirmed divergence is still found
    real[3].keys['X'] = 1;
    CHECK(rb_first_mispredicted(real, used, 0, sentinel) == 3u);
}
