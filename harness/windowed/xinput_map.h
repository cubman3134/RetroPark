#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include "retropark/retropark_abi.h"

// Pure mapping: XInput gamepad -> RetroPark abstract pad (pad_buttons bits + pad_axes[]). Does not
// touch keys[]. Sticks pass through (XInput is already -32768..32767, up = positive); triggers 0..255
// scale to 0..~32640. No XInput API calls here, so this is unit-testable without a controller.
inline void xinput_to_pad(const XINPUT_GAMEPAD& gp, rp_input_state& s) {
    auto set = [&](int bit, bool on) { if (on) s.pad_buttons |= (uint16_t)(1u << bit); };
    set(RP_PAD_A,          gp.wButtons & XINPUT_GAMEPAD_A);
    set(RP_PAD_B,          gp.wButtons & XINPUT_GAMEPAD_B);
    set(RP_PAD_X,          gp.wButtons & XINPUT_GAMEPAD_X);
    set(RP_PAD_Y,          gp.wButtons & XINPUT_GAMEPAD_Y);
    set(RP_PAD_L,          gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER);
    set(RP_PAD_R,          gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER);
    set(RP_PAD_SELECT,     gp.wButtons & XINPUT_GAMEPAD_BACK);
    set(RP_PAD_START,      gp.wButtons & XINPUT_GAMEPAD_START);
    set(RP_PAD_L3,         gp.wButtons & XINPUT_GAMEPAD_LEFT_THUMB);
    set(RP_PAD_R3,         gp.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB);
    set(RP_PAD_DPAD_UP,    gp.wButtons & XINPUT_GAMEPAD_DPAD_UP);
    set(RP_PAD_DPAD_DOWN,  gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
    set(RP_PAD_DPAD_LEFT,  gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
    set(RP_PAD_DPAD_RIGHT, gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);
    s.pad_axes[RP_AXIS_LEFT_X]        = gp.sThumbLX;
    s.pad_axes[RP_AXIS_LEFT_Y]        = gp.sThumbLY;
    s.pad_axes[RP_AXIS_RIGHT_X]       = gp.sThumbRX;
    s.pad_axes[RP_AXIS_RIGHT_Y]       = gp.sThumbRY;
    s.pad_axes[RP_AXIS_LEFT_TRIGGER]  = (int16_t)(gp.bLeftTrigger * 128);
    s.pad_axes[RP_AXIS_RIGHT_TRIGGER] = (int16_t)(gp.bRightTrigger * 128);
}
