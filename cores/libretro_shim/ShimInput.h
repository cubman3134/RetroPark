// Pure abstract-pad -> libretro input mapping, shared by LibretroShim.cpp and the unit test.
// Maps a RetroPark rp_input_state (keys[] VK flags OR'd with the generic abstract pad
// pad_buttons/pad_axes) to a libretro RETRO_DEVICE_JOYPAD/RETRO_DEVICE_ANALOG value.
//
// NES/software cores feed only keys[] (pad_buttons==0), so the keys[] path stays byte-unchanged;
// gamepad-driven cores (N64/Mupen64Plus-Next) feed the abstract pad. ANALOG is only answered when
// a core polls RETRO_DEVICE_ANALOG (NES never does).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>            // VK_* codes
#include <cstdint>
#include <retropark/retropark_abi.h>   // rp_input_state, RP_PAD_*, RP_AXIS_*
#include "libretro.h"                  // RETRO_DEVICE_* ids

// Pure mapping: no DLL/GL state. `device`/`index`/`id` are the libretro poll parameters.
inline int16_t shim_map_input(const rp_input_state& in, unsigned device, unsigned index, unsigned id) {
    if (device == RETRO_DEVICE_ANALOG) {
        // N64 analog stick = LEFT; C-buttons = RIGHT stick (Mupen's default). The abstract pad uses
        // Y-UP positive (the Dolphin GC contract); libretro analog uses Y-DOWN positive -> negate Y.
        int16_t ax = 0, ay = 0;
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)  { ax = in.pad_axes[RP_AXIS_LEFT_X];  ay = in.pad_axes[RP_AXIS_LEFT_Y]; }
        else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) { ax = in.pad_axes[RP_AXIS_RIGHT_X]; ay = in.pad_axes[RP_AXIS_RIGHT_Y]; }
        else return 0;
        if (id == RETRO_DEVICE_ID_ANALOG_X) return ax;
        if (id == RETRO_DEVICE_ID_ANALOG_Y) return (int16_t)(-ay);
        return 0;
    }
    if (device != RETRO_DEVICE_JOYPAD) return 0;

    // JOYPAD: OR the abstract pad (pad_buttons/RP_PAD_*) with the existing keys[] NES map. NES feeds
    // only keys[] (pad_buttons==0 -> keys[] wins, byte-unchanged); N64 feeds the abstract pad.
    auto pad = [&](int b){ return (in.pad_buttons & (1u << b)) != 0; };
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_UP:     return (in.keys[VK_UP]     || pad(RP_PAD_DPAD_UP))    ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (in.keys[VK_DOWN]   || pad(RP_PAD_DPAD_DOWN))  ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (in.keys[VK_LEFT]   || pad(RP_PAD_DPAD_LEFT))  ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (in.keys[VK_RIGHT]  || pad(RP_PAD_DPAD_RIGHT)) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_A:      return (in.keys['X']       || pad(RP_PAD_A))          ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_B:      return (in.keys['Z']       || pad(RP_PAD_B))          ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_START:  return (in.keys[VK_RETURN] || pad(RP_PAD_START))      ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return (in.keys[VK_SHIFT]  || pad(RP_PAD_SELECT))     ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_L:      return pad(RP_PAD_L) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_R:      return pad(RP_PAD_R) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_L2:     return (in.pad_axes[RP_AXIS_LEFT_TRIGGER] > 8192) ? 1 : 0; // N64 Z
        default: return 0;
    }
}
