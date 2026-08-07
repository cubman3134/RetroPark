#include <doctest/doctest.h>
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <cstring>
using namespace rp;

TEST_CASE("runtime: two input ports route independently") {
    Runtime rt(RP_GFX_NONE, nullptr);       // no window/backend needed for input routing
    rp_input_state p0{}; p0.keys['X'] = 1; p0.pad_buttons = 0x11;
    rp_input_state p1{}; p1.keys['Z'] = 1; p1.pad_buttons = 0x22;
    rp_runtime_set_input(reinterpret_cast<rp_runtime*>(&rt), 0, &p0);
    rp_runtime_set_input(reinterpret_cast<rp_runtime*>(&rt), 1, &p1);

    rp_input_state out0{}, out1{};
    rt.on_input(0, &out0);
    rt.on_input(1, &out1);
    CHECK(out0.keys['X'] == 1); CHECK(out0.pad_buttons == 0x11);
    CHECK(out1.keys['Z'] == 1); CHECK(out1.pad_buttons == 0x22);
    // ports don't bleed
    CHECK(out0.keys['Z'] == 0);
    CHECK(out1.keys['X'] == 0);
    // out-of-range port is clamped/ignored, never a crash
    rt.on_input(7, &out0);   // clamps to a valid port; just must not crash
}
