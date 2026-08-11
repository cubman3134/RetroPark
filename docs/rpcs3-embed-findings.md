# RPCS3 Embed — Slice I Task 1 findings (linkage) + resume fork

**Date:** 2026-08-10
**Status:** Arc **PAUSED** after Task 1. Feasibility proven; the linkage question is answered; the resume
decision (below) is deferred to a fresh, deliberate start.

This records the one thing Task 1 set out to learn — *does our own code link `rpcs3_emu` and run
`Emu::Init()`?* — so the next session doesn't re-derive it. `external/rpcs3` is git-ignored (the notes/patch
are the record); this committed doc is that record for the embed side.

## What was already proven (build spike, same day)

RPCS3 **builds from source** in this environment and the wrappable core **`rpcs3_emu.lib` compiles** (~3.6 GB).
Recipe is in the arc memory / Slice-I design spec (VS2022 CMake 3.31 + v143, `USE_SYSTEM_*=OFF`,
`BUILD_LLVM=ON` with LLVM targets trimmed to `X86`, `ALSOFT_ENABLE_MODULES=OFF`, libpng `/I<zlib-src>`,
delete `build/…/SDL3*Targets.cmake` before every reconfigure, build fully detached). That was the spike's
success criterion and it held.

## The Task-1 finding: RPCS3 has no lean headless-embed seam

Dolphin gave us a **Qt-free headless seam** (`DolphinNoGUI`): `dolphin_present` linked a small, self-contained
core. **RPCS3 does not.** A minimal core-host (`rp_rpcs3.cpp` = `QCoreApplication` + `Emu.Init()`) linking only
`rpcs3_emu` fails with **32 unresolved externals** — `rpcs3_emu` deliberately calls back into
frontend-provided symbols. They cluster as:

- **Version glue** — `rpcs3::get_version / get_verbose_version / get_version_and_branch / is_local_build`
  (defined in `rpcs3/rpcs3_version.cpp`).
- **App glue** — `report_fatal_error(std::string_view,bool,bool)` (`rpcs3/rpcs3.cpp:168`),
  `qt_events_aware_op(int,std::function<bool()>)` (defined in `rpcs3qt/main_window.cpp` — Qt),
  `g_cfg_input_configs` (`rpcs3qt/pad_settings_dialog.cpp`).
- **Input-layer web** — `pad::g_pad_thread` / `pad::g_pad_mutex`, `pad_thread::{AddLddPad,UnregisterLddPad,
  SetRumble,SetIntercepted}`, `sdl_instance::{get_instance,initialize,pump_events}`,
  `input::get_products_by_class`, `cfg_ps_moves g_cfg_move` + `cfg_ps_moves::load`, and a **vtable'd template
  class `ps_move_tracker<0>`** (ctor, virtual dtor, `process_image`, `set_*`, static `rgb_to_hsv`/`hsv_to_rgb`).
- **OpenGL** — `glGetString`, `glGetIntegerv`, `wglGetProcAddress` (just needs `opengl32.lib`).

**Root cause (build-graph):** RPCS3 compiles the app entry, the whole Input layer, *and* the entire Qt GUI
into a single static lib **`rpcs3_ui`** (`rpcs3/rpcs3qt/CMakeLists.txt`, sources incl. `../rpcs3.cpp`,
`../main_application.cpp`, `../headless_application.cpp`, `../rpcs3_version.cpp`, and all `../Input/*.cpp`).
Then `rpcs3_lib = rpcs3_emu (PUBLIC) + rpcs3_ui (PRIVATE) + Qt + 3rdparty`, and the real `rpcs3` executable
links `rpcs3_lib`. So the *only* off-the-shelf way to satisfy `rpcs3_emu`'s externs is to compile `rpcs3_ui`
— which pulls the full Qt GUI (`game_list_frame`, every settings dialog) and hits the **Qt-6.8-vs-6.11 compile
grind** (first wall: `game_list_frame.cpp` reports `m_scanned_iso_paths` undeclared though it *is* declared at
`game_list_frame.h:206` — a non-obvious Qt/stdlib-version interaction, not a one-line fix).

There is no clean `rpcs3_emu + tiny glue` boundary: the glue reaches into a vtable'd template and the Input
subsystem, and **more app-provided externs will surface at `BootGame`/`Run`/render** (audio backend, fs
callbacks, more input) — so a stub layer is a treadmill, not a one-shot.

## Resume fork (decide deliberately next time)

1. **Qt 6.11 + compile `rpcs3_ui` (recommended).** Install RPCS3's *supported* Qt (6.11) so `rpcs3_ui`
   compiles as-designed (few/no patches), then link `rpcs3_lib` — the exact, complete closure. Robust and
   satisfies boot/render externs up front. Costs a ~2-3 GB Qt download and carries dead GUI code in the
   vehicle (harmless for a desktop-only heavy core). The staged target already exists and is set up for this
   (`rp_rpcs3/CMakeLists.txt` links `rpcs3_lib`); it's `EXCLUDE_FROM_ALL` so it won't break default builds.
2. **Patch-grind current Qt 6.8.** No download, but an open-ended sequence of Qt-version patches on ~15-min
   build cycles (each reconfigure re-dirties LLVM). Symptom-by-symptom; least predictable.
3. **Hand-stub the ~32 externs (no GUI).** Leanest vehicle, but fragile (the `ps_move_tracker<0>` vtable, the
   Input web) and the stub list balloons at boot/render — likely a dead end for a full game.

## Staged artifacts (git-ignored `external/rpcs3`, kept for the resume)

- `rpcs3/rp_rpcs3/rp_rpcs3.cpp` — minimal core-host (`QCoreApplication` + `Emu.Init()`), mirrors
  `main_application::InitializeEmulator`.
- `rpcs3/rp_rpcs3/CMakeLists.txt` — `rp_rpcs3_host` target, `EXCLUDE_FROM_ALL`, links `rpcs3_lib` (option 1).
- `add_subdirectory(rp_rpcs3)` appended to `rpcs3/CMakeLists.txt`.
