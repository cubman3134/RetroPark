# RPCS3 Arc — Slice I Implementation Plan (build + embed + first Vulkan frame)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. This slice is deep RPCS3-embed R&D — several tasks mirror RPCS3's own canonical files (cited exactly), the way integration code mirrors a reference implementation. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A from-source RPCS3 core-host that boots LittleBigPlanet and renders real **Vulkan** frames under our control via a custom `GSFrameBase`, with the Vulkan present seam documented.

**Architecture:** Add a build target linking the (proven-buildable) `rpcs3_emu` core; drive the emu through `EmuCallbacks` (mirroring `headless_application`, but with `renderer=vulkan` and a **custom `GSFrameBase`** — a hidden Win32 window whose HWND RPCS3's WSI turns into a `VkSurfaceKHR`). Boot via `Emu::BootGame` + `Emu::Run`; RPCS3's `VKGSRender` renders and `VKGSRender::flip` presents. Staged: (1) link + `Emu::Init`, (2) boot with the null renderer, (3) custom Vulkan surface + first frame.

**Tech Stack:** RPCS3 (`external/rpcs3`, from source), `rpcs3_emu.lib`, Qt6Core (RPCS3's headless app is a `QCoreApplication`), Vulkan, CMake 3.31 / VS2022 v143.

## Global Constraints

- **`external/rpcs3` is git-ignored** — all RPCS3-side changes are captured to `docs/patches/rpcs3-external-present.patch` (`git -C external/rpcs3 diff > docs/patches/rpcs3-external-present.patch 2>/dev/null`, stdout-only). New files: `git -C external/rpcs3 add -N <path>` first.
- **No RetroPark-side changes this slice** (no `src/`, no `include/`, no RetroPark tests). This is purely the RPCS3 embed.
- **Never commit cores/ROMs/firmware.** Game: `C:/RetroBat/roms/ps3/LittleBigPlanet - Game of the Year Edition (USA, Canada) (En,Ja,Fr,De,Es,It,Nl,Pt,Sv,No,Da,Fi,Zh,Ko,Ru).ps3/PS3_GAME/USRDIR/EBOOT.BIN`. Firmware: RetroBat's `C:/RetroBat/emulators/rpcs3/dev_flash`.
- **Build recipe (from the spike — use verbatim):** the CMake-3.31 exe `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`; env `Qt6_ROOT=C:\Qt\6.8.3\msvc2022_64` + `VULKAN_SDK=C:\VulkanSDK\1.4.357.0`; configure flags `-G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DUSE_SYSTEM_ZLIB=OFF -DUSE_SYSTEM_CURL=OFF -DUSE_SYSTEM_OPENCV=OFF -DUSE_SYSTEM_SDL=OFF -DBUILD_LLVM=ON -DALSOFT_ENABLE_MODULES=OFF "-DCMAKE_C_FLAGS=/IC:/Users/cubma/source/repos/RetroPark/external/rpcs3/3rdparty/zlib/zlib"`. Patches already applied to the tree: `3rdparty/llvm/CMakeLists.txt` X86-only; `config_database.cpp` += `#include <QJsonDocument>`. **Before any reconfigure**, delete `build/3rdparty/libsdl-org/SDL/SDL3*Targets.cmake`. **Run builds FULLY DETACHED** (PowerShell `Start-Process <cmake> -ArgumentList '--build','build','--config','Release','--parallel' -RedirectStandardOutput <log> -WindowStyle Hidden`) and poll — Bash `run_in_background` is killed at the 10-min tool ceiling; a detached build survives.
- **Canonical RPCS3 files to mirror** (the "spec" for the intricate parts): `rpcs3/main_application.cpp` + `.h` (Emu init + `CreateCallbacks`), `rpcs3/headless_application.cpp` + `.h` (Qt-free `EmuCallbacks`, null-renderer path — the template), `rpcs3/rpcs3qt/gs_frame.h/.cpp` (the real `QWindow`+`GSFrameBase` Vulkan surface — mirror sans-Qt), `rpcs3/Emu/System.h` (`Emu::Init/BootGame/Run/Kill`), `rpcs3/Emu/RSX/GSFrameBase.h` (the interface to implement), `rpcs3/Emu/RSX/VK/VKPresent.cpp` (`VKGSRender::flip`), `rpcs3/Emu/RSX/VK/vkutils/swapchain_core.h` (`make_WSI_surface(instance, display_handle_t)`).

---

### Task 1: Build target linking `rpcs3_emu` + minimal `Emu::Init` (prove the embed links)

Prove our own code links the RPCS3 core and initializes the emulator — the foundational embed step, before any boot/render.

**Files:**
- Create: `external/rpcs3/rpcs3/rp_rpcs3/rp_rpcs3.cpp` (the core-host driver; new dir under `rpcs3/`)
- Create: `external/rpcs3/rpcs3/rp_rpcs3/CMakeLists.txt` (the target)
- Modify: `external/rpcs3/rpcs3/CMakeLists.txt` (add_subdirectory of `rp_rpcs3`) OR `external/rpcs3/CMakeLists.txt` — whichever includes `rpcs3/CMakeLists.txt`; add the subdir so it builds.
- Modify: `docs/patches/rpcs3-external-present.patch` (regenerate)

**Interfaces:**
- Consumes: `rpcs3_emu` target + its transitive libs (mirror `rpcs3/CMakeLists.txt`'s `target_link_libraries(rpcs3_lib ... rpcs3_emu ...)`); `Emulator` class / global `Emu` (`Emu/System.h`); `QCoreApplication` (Qt6Core).
- Produces: an executable `rp_rpcs3_host.exe` (in `build/bin/`) that constructs a `QCoreApplication`, runs `Emu.Init()`, logs success, exits 0.

- [ ] **Step 1: Read the reference.** Read `rpcs3/main_application.cpp` (the `Emu.Init()` call site ~line 101, and `main_application::CreateCallbacks()`), `rpcs3/main.cpp` (how the app object + `Emu` are set up around `QCoreApplication`), and `rpcs3/CMakeLists.txt:47-95` (how `rpcs3_lib` links `rpcs3_emu` + platform libs). This is the structure to mirror minimally.

- [ ] **Step 2: Write the minimal core-host.** Create `external/rpcs3/rpcs3/rp_rpcs3/rp_rpcs3.cpp`: a `main(argc,argv)` that (a) constructs a `QCoreApplication` (RPCS3 assumes one exists for `call_from_main_thread`), (b) sets `Emu` up minimally and calls `Emu.Init()` (no boot yet), (c) prints `[rp_rpcs3] Emu.Init OK`, (d) returns 0. Use the exact includes/setup `main_application.cpp` uses for the init (copy its `Emu.Init()` preamble — logging/config-dir setup — as minimally as compiles). Do NOT set up callbacks or boot yet.

- [ ] **Step 3: Write the target CMake.** Create `external/rpcs3/rpcs3/rp_rpcs3/CMakeLists.txt` defining `add_executable(rp_rpcs3_host rp_rpcs3.cpp)` and `target_link_libraries(rp_rpcs3_host PRIVATE rpcs3_emu Qt6::Core <the same platform libs rpcs3_lib links: ws2_32 Iphlpapi Winmm Psapi gdi32 setupapi>)`, `target_include_directories` for `rpcs3/` + `Emu/`, and set the output to `build/bin/`. Mirror the include/link setup from `rpcs3/CMakeLists.txt`. Add `add_subdirectory(rp_rpcs3)` to `rpcs3/CMakeLists.txt` (near where other subdirs are added).

- [ ] **Step 4: Reconfigure + build the target (detached).** Delete the SDL3 targets, reconfigure (CMake 3.31), then build ONLY the new target: `cmake --build build --config Release --target rp_rpcs3_host --parallel` (detached via Start-Process; poll). Since `rpcs3_emu` is already built, this compiles `rp_rpcs3.cpp` + links → fast.
Expected: `build/bin/rp_rpcs3_host.exe` produced, no link errors (proves our code links `rpcs3_emu`). Fix any missing-lib link errors by adding the lib to the target (mirror `rpcs3_lib`'s full link list).

- [ ] **Step 5: Run it.** `cd build/bin && ./rp_rpcs3_host.exe` → expect `[rp_rpcs3] Emu.Init OK` and exit 0. (If it crashes in `Emu.Init`, compare against `main_application.cpp`'s exact preamble — a missing config-dir/log setup is the usual cause.)

- [ ] **Step 6: Refresh patch + commit.**
```bash
cd C:/Users/cubma/source/repos/RetroPark && git -C external/rpcs3 add -N rpcs3/rp_rpcs3/rp_rpcs3.cpp rpcs3/rp_rpcs3/CMakeLists.txt 2>/dev/null; git -C external/rpcs3 diff > docs/patches/rpcs3-external-present.patch 2>/dev/null; grep -c "rp_rpcs3\|Emu.Init" docs/patches/rpcs3-external-present.patch
git add docs/patches/rpcs3-external-present.patch && git commit -m "feat(rpcs3): Slice I task 1 — core-host target links rpcs3_emu + Emu::Init (embed proven)"
```

---

### Task 2: Boot LittleBigPlanet with the null renderer (prove the emu runs)

Wire `EmuCallbacks` (mirroring `headless_application`) + firmware/config, and boot the game with the **null** renderer — proving RPCS3's emu actually boots + runs the game (no video yet). Isolates the boot pipeline from the rendering.

**Files:**
- Modify: `external/rpcs3/rpcs3/rp_rpcs3/rp_rpcs3.cpp`
- Modify: `docs/patches/rpcs3-external-present.patch`

**Interfaces:**
- Consumes: `EmuCallbacks` (`Emu/system_config`/`main_application.h`); `headless_application::InitializeCallbacks` (the Qt-free callback set — mirror it); `Emu.BootGame(path, ...)`/`Emu.Run(true)`/`Emu.Kill()` (`Emu/System.h`); `g_cfg.video.renderer` (`Emu/system_config.h`); the firmware dir.
- Produces: `rp_rpcs3_host.exe <game-path>` boots the game headless (null renderer), runs N frames/seconds, logs progress, exits cleanly.

- [ ] **Step 1: Read the reference.** Read `rpcs3/headless_application.cpp` in full (its `InitializeCallbacks` — `try_to_quit`, `call_from_main_thread`, `init_gs_render` with the null-renderer gate, `get_gs_frame` returning empty) and `main_application.cpp`'s `CreateCallbacks` + the `Emu.BootGame`/config-mode path. This is the exact template to mirror.

- [ ] **Step 2: Mirror the headless callbacks + boot.** In `rp_rpcs3.cpp`: build the `EmuCallbacks` as `headless_application` does (a `call_from_main_thread` that runs the func — a simple queue pumped on the main thread is fine; `init_gs_render` selecting the renderer; `get_gs_frame` returning empty for null; stub the kb/mouse/pad/audio handlers). Set `g_cfg.video.renderer = video_renderer::null` for THIS task. Point the emu's config/VFS at the firmware (`dev_flash`) — mirror how `main_application`/`rpcs3::utils` resolve the emu dir + `dev_flash`; set the config's firmware/VFS to RetroBat's `C:/RetroBat/emulators/rpcs3/dev_flash` (or copy it into our config dir). No-op the alert/report handlers so no modal blocks boot. Then `Emu.BootGame(argv[1])`, check `game_boot_result::no_errors`, `Emu.Run(true)`, pump `call_from_main_thread` + sleep for ~20s, log frame/emulation progress (e.g. via an RSX frame counter or `Emu` state), then `Emu.Kill()`.

- [ ] **Step 3: Build (detached) + run on the game.**
Build `rp_rpcs3_host` (detached, as Task 1). Run: `cd build/bin && ./rp_rpcs3_host.exe "C:/RetroBat/roms/ps3/LittleBigPlanet - Game of the Year Edition (USA, Canada) (En,Ja,Fr,De,Es,It,Nl,Pt,Sv,No,Da,Fi,Zh,Ko,Ru).ps3/PS3_GAME/USRDIR/EBOOT.BIN"` (bounded ~30s wall-clock + kill).
Expected: logs show `BootGame` → `no_errors`, the emu enters the running state, and RSX/PPU activity advances (the game boots on the null renderer — no window, no video, but the VM runs). If boot fails: the usual causes are missing firmware (`dev_flash` not found → set the VFS), or a missing config-dir. Compare against `headless_application`'s working boot.

- [ ] **Step 4: Refresh patch + commit.**
```bash
cd C:/Users/cubma/source/repos/RetroPark && git -C external/rpcs3 diff > docs/patches/rpcs3-external-present.patch 2>/dev/null; grep -c "BootGame\|renderer::null\|dev_flash" docs/patches/rpcs3-external-present.patch
git add docs/patches/rpcs3-external-present.patch && git commit -m "feat(rpcs3): Slice I task 2 — boot LittleBigPlanet headless (null renderer) via mirrored EmuCallbacks + firmware"
```

---

### Task 3: Custom Vulkan `GSFrameBase` + first frame + seam report (GREEN)

Implement a custom `GSFrameBase` (hidden Win32 window) so RPCS3's **Vulkan** renderer runs against it; get a real rendered frame out; document the present seam.

**Files:**
- Create: `external/rpcs3/rpcs3/rp_rpcs3/rp_gs_frame.h/.cpp` (custom `GSFrameBase`)
- Modify: `external/rpcs3/rpcs3/rp_rpcs3/rp_rpcs3.cpp` (renderer=vulkan, wire `get_gs_frame`)
- Create: `docs/rpcs3-build.md`, `docs/rpcs3-seam.md`
- Modify: `docs/patches/rpcs3-external-present.patch`

**Interfaces:**
- Consumes: `GSFrameBase` (`Emu/RSX/GSFrameBase.h` — implement `client_width/height`, `display_handle_t handle()`, `present_frame(...)`, and the other pure-virtuals); `display_handle_t` (Windows = the HWND type the WSI expects — see `swapchain_core.h`/`swapchain_windows.hpp`); `make_WSI_surface` (called by RPCS3's VK backend from our `handle()`); `VKGSRender::flip` (`VKPresent.cpp`) as the present point.
- Produces: `rp_rpcs3_host.exe` renders LittleBigPlanet with Vulkan into a hidden window; a framedump/readback confirms real (non-black, changing) frames.

- [ ] **Step 1: Read the reference.** Read `rpcs3/rpcs3qt/gs_frame.h/.cpp` (the real `QWindow`+`GSFrameBase` — how `handle()` returns the platform `display_handle_t`, what the render-surface contract is) and `rpcs3/Emu/RSX/VK/vkutils/swapchain_windows.hpp` (how `make_WSI_surface` uses the handle to `vkCreateWin32SurfaceKHR`). Confirm the `display_handle_t` shape on Windows (HWND / `std::pair<HWND,HINSTANCE>`).

- [ ] **Step 2: Implement the custom `GSFrameBase`.** Create `rp_gs_frame.h/.cpp`: a class implementing `GSFrameBase` that owns a **hidden Win32 window** (`WS_POPUP`, off-screen, `SW_HIDE`, sized to the game's output e.g. 1280x720 — the Dolphin hidden-window pattern). `handle()` returns the `display_handle_t` built from that HWND (match the exact shape `swapchain_windows.hpp` expects). Implement `client_width()/client_height()` from the window size, `present_frame()` as a no-op or a readback sink (the Vulkan path presents via the swapchain, not `present_frame`), and every other pure-virtual of `GSFrameBase` as a minimal stub (read the header for the full list). No Qt.

- [ ] **Step 3: Switch to Vulkan + wire the frame.** In `rp_rpcs3.cpp`: set `g_cfg.video.renderer = video_renderer::vulkan`; make `get_gs_frame` return a `std::make_unique<rp_gs_frame>()` (instead of empty). Remove/gate the null-only assertion path. Keep boot + run + pump as Task 2.

- [ ] **Step 4: Build (detached) + run + verify a frame.**
Build `rp_rpcs3_host` (detached). Run on LittleBigPlanet (bounded ~40s + kill). Verify a real Vulkan frame renders. RPCS3 can dump frames — enable its screenshot/framedump (check `g_cfg`/`rsx` for a frame-capture; RPCS3 writes screenshots to its config dir) OR add a readback of the present source in `rp_gs_frame`/via `VKGSRender::flip`. Assert the captured frame is non-black + advances over time (near-black boot → rendered content, mirroring the Dolphin Slice-I framedump proof). **This is the human/artifact proof; save a captured frame.** If the Vulkan surface fails to create: verify `handle()`'s `display_handle_t` shape matches `swapchain_windows.hpp`, and that the hidden window is valid (a real HWND, message pump running).

- [ ] **Step 5: Write the docs.** `docs/rpcs3-build.md` = the proven build recipe (from Global Constraints). `docs/rpcs3-seam.md` = the present-seam report: `Emu::BootGame`/`Run`, the custom-`GSFrameBase` render-surface mechanism, and `VKGSRender::flip`/`get_present_source` (`VKPresent.cpp`) as the exact point a later slice copies RPCS3's final frame into RetroPark's shared `VkImage` (the Slice-J equivalent).

- [ ] **Step 6: Refresh patch + commit.**
```bash
cd C:/Users/cubma/source/repos/RetroPark && git -C external/rpcs3 add -N rpcs3/rp_rpcs3/rp_gs_frame.h rpcs3/rp_rpcs3/rp_gs_frame.cpp 2>/dev/null; git -C external/rpcs3 diff > docs/patches/rpcs3-external-present.patch 2>/dev/null; grep -c "GSFrameBase\|renderer::vulkan\|make_WSI" docs/patches/rpcs3-external-present.patch
git add docs/patches/rpcs3-external-present.patch docs/rpcs3-build.md docs/rpcs3-seam.md && git commit -m "feat(rpcs3): Slice I task 3 — custom Vulkan GSFrameBase renders LittleBigPlanet headless; build+seam docs"
```

---

## Post-plan: verify + memory

After Task 3: a real Vulkan frame from our from-source RPCS3 core-host is captured (the Slice-I proof); the build + seam are documented. No merge gate needed beyond the commits (no RetroPark code changed). Update `rpcs3-arc.md` marking Slice I done + noting the next slice (J-equivalent: copy RPCS3's frame into RetroPark's shared VkImage via the Dolphin timeline/QFOT machinery). **Execution note:** this slice is best executed INLINE (the accumulated RPCS3 context matters; fresh subagents would re-derive it), iterating against the cited RPCS3 files.
