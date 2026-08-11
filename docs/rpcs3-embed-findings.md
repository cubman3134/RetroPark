# RPCS3 Embed — Slice I Task 1 findings (linkage) + resume fork

**Date:** 2026-08-10
**Status:** Task 1 **DONE + verified** (2026-08-11). The fork below was resolved via **Option 1** (match
RPCS3's expected Qt). `rp_rpcs3_host.exe` links the full `rpcs3_lib` closure, runs `QCoreApplication` +
`Emu::Init()`, prints `[rp_rpcs3] Emu.Init OK (stopped=1)`, and exits 0 — no source patching of `rpcs3_ui`
was needed once Qt matched. Next: Task 2 (boot LittleBigPlanet, null renderer).

This records the one thing Task 1 set out to learn — *does our own code link `rpcs3_emu` and run
`Emu::Init()`?* — so the next session doesn't re-derive it. `external/rpcs3` is git-ignored (the notes/patch
are the record); this committed doc is that record for the embed side.

## RESOLUTION (2026-08-11): Option 1 — Qt 6.10.3 on the D drive

The `game_list_frame.cpp` / `config_database.cpp` errors were **purely a Qt-version mismatch**: RPCS3 master's
GUI code uses APIs from the Qt the devs build against (6.10+), while the box had 6.8.3. RPCS3's floor is
`QT_MIN_VER 6.7.0` with a `>= 6.10.0` code branch, so matching 6.10.x is the clean fix — **no source patches**.

Recipe (repeatable, no Qt account, installs to D so it's off the ~90%-full C drive):
- `pip install aqtinstall` (used v3.3.0). aqt config `settings.ini` needed
  `[requests] INSECURE_NOT_FOR_PRODUCTION_ignore_hash = True` (the default mirror fails the Updates.xml
  checksum download); pass it as a **top-level** flag: `python -m aqt -c settings.ini install-qt ...`.
- `python -m aqt -c settings.ini install-qt windows desktop 6.10.3 win64_msvc2022_64 -m qtmultimedia qtimageformats --outputdir D:\Qt`
  → `D:\Qt\6.10.3\msvc2022_64`. NB: **`qtsvg` is NOT a separate module in 6.10** (Svg/SvgWidgets ship in
  base); only `qtmultimedia` + `qtimageformats` are add-ons. (6.11.1's metadata failed to resolve under aqt
  3.3.0 — 6.10.3 pulled fine and satisfies the `>= 6.10.0` branch.)
- Reconfigure pointing at it, unsetting the cached 6.8.3 paths:
  `Qt6_ROOT=D:\Qt\6.10.3\msvc2022_64  cmake -S . -B build -U "Qt6*_DIR" -DQt6_ROOT=D:/Qt/6.10.3/msvc2022_64`
  (still delete `build/…/SDL3*Targets.cmake` first). Then build `--target rp_rpcs3_host`.
- To RUN: put `D:\Qt\6.10.3\msvc2022_64\bin` on PATH (needs Qt6*.dll). The harness `quick_exit(0)`s right
  after the proof print — `Emu.Init()` leaves RPCS3 background threads alive and letting `main()` return
  null-derefs in Qt's static teardown (`QCoreApplicationPrivate`); RPCS3 itself hard-exits for the same
  reason. Clean lifecycle (Kill/shutdown) is Task 2's job.

Net: `rpcs3_ui` compiles clean on 6.10.3; `rp_rpcs3_host` links + inits. The vehicle embeds RPCS3 Qt-and-all,
as expected for a desktop-only heavy core.

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
