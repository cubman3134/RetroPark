# RPCS3 Embed — Slice I Task 1 findings (linkage) + resume fork

**Date:** 2026-08-10
**Status:** Slice I **COMPLETE** — Tasks 1, 2, 3 all DONE + verified (2026-08-11). Task 1: `rp_rpcs3_host.exe`
links the full `rpcs3_lib` closure and runs `Emu::Init()` (exit 0) — resolved via **Option 1** (match RPCS3's
expected Qt, 6.10.3 on D; no `rpcs3_ui` source patches). Task 2: boots **LittleBigPlanet GOTY** headless,
`BootGame -> no_errors`, title `BCUS98208` recognized. Task 3: a **custom Vulkan `GSFrameBase`** (hidden Win32
window) — RPCS3's own `VKGSRender` builds a swapchain on our HWND and **presented 43 real Vulkan frames to our
surface** (`vk_frames_presented=43`), with a real **1280×720 BGRA frame read back** to disk. The
presenting-core pattern is proven for a SECOND heavy app.

> NB on Task 2's "stops ~3s": that was NOT the null renderer — it was the directory-boot precompile-quit path
> (see Task 3 below). BootGame's `no_errors` was real; the title just never ran. Task 3's EBOOT-path fix makes
> it actually run under both renderers.

## Task 3 — custom Vulkan GSFrameBase + real frames (2026-08-11)

The host subclasses `main_application` directly (NOT `headless_application`, which sets `Emu.SetHeadless(true)`
→ `fixup_settings` force-nulls the renderer). It keeps Emu non-headless, sets renderer=vulkan, supplies a
Vulkan `init_gs_render` (`g_fxo->init<rsx::thread, named_thread<VKGSRender>>`) and a `get_gs_frame` returning
**`rp_gs_frame`** — a `GSFrameBase` backed by a hidden `WS_POPUP` Win32 window. `VKGSRender::on_init_thread`
calls our `handle()` (the HWND) → `vkCreateWin32SurfaceKHR` → swapchain at `client_width/height` (1280×720);
`make_context/set_current/delete_context` and the window-state methods are no-ops; `flip()` tallies frames;
`take_screenshot()`/`present_frame()` are the readback egress (set `g_user_asked_for_screenshot` → RSX reads
the composited frame back from VRAM in `VKPresent`, BGRA, pitch `w*4`). No `Q_OBJECT`/AUTOMOC:
`call_from_main_thread` marshals via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; `QCoreApplication`
suffices (render_creator enumerates Vulkan fine — found the RTX 5080). No new source file → no reconfigure;
`HAVE_VULKAN` + Vulkan includes reach our TU because `rpcs3_emu` links `3rdparty::vulkan` **PUBLIC**.

Getting the title to actually RUN (not just link/boot) took four `run_rpcs3`-bypass fixes — RPCS3 assumes its
own `run_rpcs3` bootstrap that we replace:
1. **Boot the EBOOT.BIN *file*, not the `.ps3` folder.** A directory path enters RPCS3's "Special boot mode
   (directory scan)" (`System.cpp:1808`): it precompiles every module then `Emu.Kill()`s — `BootGame` returns
   `no_errors` but the game never runs. (This was also Task 2's phantom "3s stop".)
2. **Bring up the file logger** (`logs::make_file_listener` + `logs::set_init({})`) — else every log line stays
   buffered and never hits disk, leaving the boot un-debuggable.
3. **`SetProcessWorkingSetSize(GetCurrentProcess(), 0x80000000, 0xC0000000)`** (rpcs3.cpp:645) — RPCS3
   `VirtualLock`s its "sudo" fast-memory mirror; without the 2-3 GiB working set `lock_sudo` fails ("Failed to
   lock sudo memory") and LBP crashes ~4.5s in.
4. **Don't read frames back during early GPU-heavy boot** — requesting the screenshot readback in the first
   few seconds (shader-compile storm) crashes the game; holding off lets it run rock-stable (60s+).

Result: real LBP PPU threads (`bringup`/`main loading`/`main slow`/`respump`/`JobManagerWorker`), RSX shader
compilation (`RSX: Add program vp/fp`), **43 frames presented to our Vulkan surface**, BGRA readback captured.
The frames captured so far are the black loading screen — LBP then enters a long non-presenting load phase
(frame count plateaus), so a visible-content framedump needs the title to progress further (more time / input);
the render pipeline itself is fully exercised.

**Present seam (for the later shared-`VkImage` handoff, Slice-J equivalent):** RPCS3's final composited frame
is available two ways on our `GSFrameBase`, both sourced in `Emu/RSX/VK/VKPresent.cpp` (`VKGSRender::flip`):
`take_screenshot()` (gated by `g_user_asked_for_screenshot`) and `present_frame()` (gated by
`g_recording_mode != stopped && can_consume_frame()`). Both receive the pre-present source image read back from
VRAM (`copy_image_to_buffer` → host-visible buffer), `width×height` = the game's buffer dims, 4 bpp, BGRA when
`format()==VK_FORMAT_B8G8R8A8_UNORM`. That readback (or, better, a GPU-side copy of `get_present_source()`'s
image into RetroPark's shared `VkImage`) is where the Slice-J handoff hooks.

## Task 2 — headless boot (2026-08-11)

Rather than re-implement the headless callbacks, the host **instantiates RPCS3's own `headless_application`**
(now linkable via `rpcs3_ui`): its `Init()` does `InitializeCallbacks` (null renderer/camera/music, no dialogs)
+ `InitializeEmulator` (`SetHeadless`+`Emu.Init`) + `InitializeConnects` (the `call_from_main_thread`
marshalling that needs `app.exec()`). Then `Emu.BootGame(<disc>, "", /*direct=*/true)` + a bounded Qt loop.
Key gotchas found & fixed:
- **`g_headless` ≠ `Emu.SetHeadless()`.** `Emu.SetHeadless()` only selects the null renderer; the *distinct*
  global `g_headless` (rpcs3.cpp:91) is what makes `report_fatal_error` do stderr+abort instead of spinning up
  a `QApplication` for a GUI dialog (which null-derefs in our headless host — the `QGuiApplicationPrivate`
  crash + a doubled run we first saw). It's normally set only by `run_rpcs3`'s `--headless` arg, which we
  bypass — so the host sets `extern atomic_t<bool> g_headless; g_headless = true;` itself (as RPCS3's unit
  tests do). This both kills the crash and unmasks real errors on stderr.
- **`BootGame(direct=true)` already autostarts `Run()`** internally (System.cpp:2714). Calling `Emu.Run()`
  again trips `ensure(IsReady())` (System.cpp:2730) since it's already running — don't double-run.
- **Firmware** comes from RetroBat via `RPCS3_CONFIG_DIR=C:\RetroBat\emulators\rpcs3\` (trailing slash
  required — `fs::get_config_dir` truncates to the last '/'); `dev_flash` resolves there (firmware 4.90). The
  `.ps3` is a decrypted-disc **folder** (PS3_DISC.SFB + PS3_GAME) — pass the folder; RPCS3 mounts dev_bdvd.
- **No log file:** RPCS3's file-log sink is set up in `run_rpcs3` (bypassed), so the host writes no RPCS3.log;
  use stdout state-polling (`Emu.IsRunning/IsStopped/...`) for the run timeline instead.

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
