// Unit test for the pure abstract-pad -> libretro JOYPAD+ANALOG mapping (ShimInput.h).
// No DLL/GL: exercises shim_map_input directly, proving the abstract pad maps correctly, the
// N64 Z-from-left-trigger + Y-negated analog rules hold, and the NES keys[] OR path stays intact.
#include <doctest/doctest.h>
#include <retropark/retropark_abi.h>
#include "../cores/libretro_shim/ShimInput.h"
#include "libretro.h"

TEST_CASE("shim input: abstract pad -> libretro JOYPAD+ANALOG, NES keys[] OR intact") {
    rp_input_state in{};
    // NES-OR: keys only
    in.keys['X'] = 1;
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 1);
    in = rp_input_state{};
    // abstract pad buttons
    in.pad_buttons = (1u<<RP_PAD_A) | (1u<<RP_PAD_DPAD_LEFT);
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 1);
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) == 1);
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) == 0);
    // L/R shoulders
    in = rp_input_state{}; in.pad_buttons = (1u<<RP_PAD_L) | (1u<<RP_PAD_R);
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L) == 1);
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R) == 1);
    // Z from left trigger
    in = rp_input_state{}; in.pad_axes[RP_AXIS_LEFT_TRIGGER] = 20000;
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) == 1);
    // analog (Y negated), LEFT stick
    in = rp_input_state{}; in.pad_axes[RP_AXIS_LEFT_X] = 30000; in.pad_axes[RP_AXIS_LEFT_Y] = 30000;
    CHECK(shim_map_input(in, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) == 30000);
    CHECK(shim_map_input(in, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) == -30000);
    // analog RIGHT index reads the right axes
    in = rp_input_state{}; in.pad_axes[RP_AXIS_RIGHT_X] = -12345; in.pad_axes[RP_AXIS_RIGHT_Y] = 5000;
    CHECK(shim_map_input(in, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) == -12345);
    CHECK(shim_map_input(in, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) == -5000);
    // all zero -> nothing
    in = rp_input_state{};
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 0);
    CHECK(shim_map_input(in, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) == 0);
}
