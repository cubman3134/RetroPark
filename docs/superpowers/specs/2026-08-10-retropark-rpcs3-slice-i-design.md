# RetroPark — RPCS3 Arc, Slice I Design (build + embed + first Vulkan frame + present seam)

**Date:** 2026-08-10
**Status:** Approved (design)
**Scope:** A from-source RPCS3 that boots a real PS3 game (LittleBigPlanet GOTY) and renders real **Vulkan**
frames under our control — driven headless via a **custom `GSFrameBase`** — plus a documented Vulkan present
seam. The RPCS3 analog of Dolphin's Slice I: the foundation for wrapping RPCS3 as a `rpcs3_present` RetroPark
core. **No RetroPark-side changes** in this slice; all work is RPCS3-embed-side. Proves *our-controlled
rendering*, not yet the shared-surface handoff (that's the Slice-J equivalent).

---

## 0. Context — what the build spike already proved

RPCS3 **builds from source in this environment** (spike, 2026-08-10). The wrappable emulator core
**`rpcs3_emu.lib` compiled** (3.6 GB). The working recipe (RPCS3's CMake path is Linux/`.sln`-oriented and
fought a chain of Windows quirks — all now solved):

- **Toolchain:** VS2022 Community's **CMake 3.31** + **v143 toolset** (`C:\Program Files\Microsoft Visual
  Studio\2022\Community\...`). NOT the VS18/CMake-4.2 preview (reconfigure-strictness bugs).
- **Configure:** `-G "Visual Studio 17 2022" -A x64`, env `Qt6_ROOT=C:\Qt\6.8.3\msvc2022_64` +
  `VULKAN_SDK=C:\VulkanSDK\1.4.357.0`, `-DUSE_SYSTEM_ZLIB=OFF -DUSE_SYSTEM_CURL=OFF -DUSE_SYSTEM_OPENCV=OFF
  -DUSE_SYSTEM_SDL=OFF -DBUILD_LLVM=ON -DALSOFT_ENABLE_MODULES=OFF -DCMAKE_C_FLAGS=/I<abs>/3rdparty/zlib/zlib`.
- **git-ignored patches (`external/rpcs3`, the patch/notes are the record):** `3rdparty/llvm/CMakeLists.txt`
  LLVM targets `"AArch64;X86"` → `"X86"` (halves LLVM). Delete `build/3rdparty/libsdl-org/SDL/SDL3*Targets.cmake`
  before ANY reconfigure (else `export()`/`include()` breaks). (Qt-GUI-only quirk, not needed here:
  `config_database.cpp` needs `#include <QJsonDocument>`; the full `rpcs3.exe` GUI needs Qt 6.11 — irrelevant,
  we don't build `rpcs3qt`.)

RPCS3's own API surface (explored):
- **`Emu::BootGame(path, title_id, direct, cfg_mode, config_path)`** → `game_boot_result`; `Emu::Run()`,
  `Emu::Load()`, `Emu::Kill()` (`rpcs3/Emu/System.h`). The programmatic boot API.
- **`rpcs3/headless_application.cpp`** — a Qt-free headless app, BUT it forces `video_renderer::null` (throws
  for Vulkan/OpenGL). So it renders nothing — unusable directly for our needs; it is the adaptation template.
- **`GSFrameBase`** (`rpcs3/Emu/RSX/GSFrameBase.h`) — the render-surface the frontend supplies via the
  `get_gs_frame()` callback (`client_width/height`, `display_handle_t handle()`, `present_frame(...)`). This
  is the seam a custom Vulkan surface plugs into.
- **Vulkan present:** `VKGSRender::flip(const rsx::display_flip_info_t&)` (`Emu/RSX/VK/VKPresent.cpp:419`) +
  `get_present_source(present_surface_info*, avconf&)` (`:299`) — where RPCS3's final frame is presented to
  its swapchain. This is the copy-from-XFB-equivalent seam for the later handoff.

## 1. The crux: getting Vulkan rendering headless

Dolphin's `DolphinNoGUI` + a hidden Win32 window rendered with Vulkan out of the box. RPCS3 is different:
its headless app is **null-renderer only**. RPCS3 obtains its render window from a frontend-supplied
`GSFrameBase`. So this slice **implements a custom `GSFrameBase`** — a hidden/offscreen Vulkan render
surface (a hidden Win32 window is the simplest, mirroring the Dolphin hidden-window approach) — wires it in
via the `get_gs_frame()` callback with `renderer = Vulkan`, and drives the emu. That makes RPCS3's real
Vulkan renderer (`VKGSRender`) run against our surface, producing real frames through `VKGSRender::flip`.

## 2. Components (Slice I)

### `rp_rpcs3.cpp` — the core-host driver (new; the RPCS3 analog of `rp_dolphin.cpp`)
- Adapt `main_application`/`headless_application`'s callback structure (Qt-free) but with
  `g_cfg.video.renderer = vulkan` (not null) and a **custom `GSFrameBase`** in `get_gs_frame()`.
- Init the emu subsystems the way `main_application::InitializeCallbacks`/`Emu::Init` require (config load,
  VFS, firmware/`dev_flash`, input as stubs/null, audio as null for now).
- `Emu::BootGame(<LittleBigPlanet PS3_GAME path>)` then `Emu::Run()`; pump until frames render.
- A hidden Win32 window as the `GSFrameBase::handle()` so the Vulkan swapchain/present path runs (the RPCS3
  analog of the Dolphin hidden window — pure offscreen may skip present, TBD in the plan).
- Alert/panic handling so no modal dialog blocks boot (RPCS3's report handlers → no-op), mirroring the
  Dolphin `RegisterMsgAlertHandler` gotcha.

### Build target linking `rpcs3_emu.lib`
- A CMake target (or a small add to RPCS3's build) that compiles `rp_rpcs3.cpp` and links `rpcs3_emu.lib`
  plus RPCS3's 3rdparty/LLVM libs, using the proven recipe. For Slice I it can be an executable (like the
  Dolphin framedump exe) — the DLL vehicle packaging is Slice K.

### Firmware / config
- RPCS3 needs the PS3 firmware (`dev_flash`) to boot most games. Point the emu's config/VFS at RetroBat's
  installed firmware (`C:/RetroBat/emulators/rpcs3/dev_flash`) or install it into our config dir. Nail the
  exact mechanism in the plan.

## 3. Testing (Slice I)

- **First-frame proof (human/artifact, gated — heavy):** run the core-host on LittleBigPlanet; verify it
  renders **real Vulkan frames** via a framedump/screenshot or a readback of RPCS3's present source (the
  RPCS3 equivalent of the Dolphin framedump PNG going from near-black boot to a rendered title). Bounded
  wall-clock + kill (like the Dolphin Slice-I framedump). This is a manual/artifact proof; no doctest yet
  (the Runtime-path e2e comes with the Slice-K core).
- **Seam report:** document `VKGSRender::flip`/`get_present_source` + the `GSFrameBase` present path — the
  exact place a later slice copies RPCS3's final frame into RetroPark's shared `VkImage`.
- **Regression:** none for RetroPark (no RetroPark code changes this slice).

## 4. Scope

**In Slice I:** the core-host driver (`rp_rpcs3.cpp`) with a custom Vulkan `GSFrameBase`; a build target
linking `rpcs3_emu.lib`; firmware/config setup; boot LittleBigPlanet + render real Vulkan frames (verified
by framedump/readback); the present-seam report. **No RetroPark changes.**

**Out (later slices, reusing the Dolphin machinery):** copy RPCS3's frame into RetroPark's shared `VkImage`
(Slice-J equivalent — the timeline/QFOT handoff); package as a reusable `rpcs3_present` `rp_core` behind
`rp_core_abi` (Slice-K equivalent); audio (RPCS3 `cellAudio`/cubeb → `rp_host.audio_sample`); input
(`rp_input_state` → RPCS3 pad); savestate (RPCS3 has its own); the full `rpcs3.exe` GUI (Qt 6.11 — never
needed); non-Vulkan RPCS3 backends.

**The single provable claim:** *A from-source RPCS3, driven headless with the Vulkan renderer via a custom
`GSFrameBase`, boots LittleBigPlanet and renders real frames — proven by a framedump/readback — with the
Vulkan present seam documented for the shared-image handoff. Proves RPCS3 can be rendered under our control,
the RPCS3 analog of Dolphin Slice I; the shared-surface handoff + reusable core follow.*

## 5. Repo additions

```
external/rpcs3/Source/.../rp_rpcs3.cpp (or a new dir)   # core-host driver: custom Vulkan GSFrameBase + Emu::BootGame (patch/notes)
external/rpcs3/... build target                          # links rpcs3_emu.lib (patch)
docs/rpcs3-build.md                                      # the proven build recipe (committed — the reproducible record)
docs/rpcs3-seam.md                                       # VKGSRender::flip / GSFrameBase present-seam report (committed)
docs/patches/rpcs3-external-present.patch                # all RPCS3-side changes (external/rpcs3 git-ignored; the patch is the record)
```
