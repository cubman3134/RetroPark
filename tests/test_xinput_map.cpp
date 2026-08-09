#include <doctest/doctest.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#include "../harness/windowed/xinput_map.h"

TEST_CASE("xinput_to_pad maps buttons, sticks, and triggers to the abstract pad") {
    XINPUT_GAMEPAD gp{};
    gp.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_LEFT;
    gp.sThumbLX = 20000; gp.sThumbLY = -30000;
    gp.sThumbRX = 0;     gp.sThumbRY = 0;
    gp.bLeftTrigger = 255; gp.bRightTrigger = 0;

    rp_input_state s{};
    xinput_to_pad(gp, s);

    CHECK((s.pad_buttons & (1u << RP_PAD_A)));
    CHECK((s.pad_buttons & (1u << RP_PAD_START)));
    CHECK((s.pad_buttons & (1u << RP_PAD_DPAD_LEFT)));
    CHECK_FALSE((s.pad_buttons & (1u << RP_PAD_B)));
    CHECK(s.pad_axes[RP_AXIS_LEFT_X] == 20000);
    CHECK(s.pad_axes[RP_AXIS_LEFT_Y] == -30000);   // Y sign preserved (up = positive)
    CHECK(s.pad_axes[RP_AXIS_LEFT_TRIGGER] > 32000); // 255 -> near full
    CHECK(s.pad_axes[RP_AXIS_RIGHT_TRIGGER] == 0);
    CHECK(s.keys[0] == 0);                          // does not touch keys[]
}
