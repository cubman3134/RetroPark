# RetroPark — Slice D Design (libretro compatibility shim)

**Date:** 2026-08-07
**Status:** Approved (design), pending implementation plan
**Scope:** Load an unmodified libretro core (FCEUmm) and run a real NES ROM through RetroPark's
driven path — adding a content-loading concept and a pixel-format shim — on both GPU backends.

---

## 0. Context and goal

Slice C built the **driven** execution model (host pulls `run_frame`, core hands back a CPU RGBA8
framebuffer via `video_refresh`). A libretro core *is* a driven core, so a compatibility shim maps
directly onto that path.

Slice D builds a **libretro shim**: a RetroPark driven-core DLL that internally `LoadLibrary`s an
unmodified libretro core, implements the libretro API surface the core calls back into, converts
the core's pixel format to RGBA8, and forwards frames to RetroPark. It also adds RetroPark's first
**content** concept (a ROM path), since real cores need content. It is validated end to end with
**FCEUmm** (verified: exports `retro_api_version/init/run/load_game/set_environment/...`) and a real
NES ROM, on both D3D11 and Vulkan.

### Decisions already made (this brainstorm)

| Decision | Choice |
|---|---|
| Shim form | A **driven-core DLL** wrapping an unmodified libretro core; reuses the Slice-C driven path → runs on both backends, zero backend changes. |
| Target | **FCEUmm** (real, downloaded from the official libretro buildbot) + a real NES ROM from the user's collection. The shim is written generically against `libretro.h`; only NES/FCEUmm is *validated* this slice. |
| Content | **A**: add a real content-loading path — `rp_runtime_load_content` + an ABI `load_content` hook (ABI v4). Not a sidecar hack. |
| Which libretro core | named in the shim's own `core.json` (`libretro_core` field the runtime ignores, the shim reads by self-locating its DLL). |
| Pixel formats | Convert **0RGB1555, RGB565, XRGB8888 → RGBA8** (pure, unit-tested). The shim always reports RGBA8 to RetroPark. |
| Deferred | Audio (callbacks accepted + dropped), savestate/serialize, HW-render cores, core-options UI, non-NES validation. |

---

## 1. Content ABI evolution (additive, v4)

RetroPark gains its first notion of content:

- **C API** (`retropark.h`): `rp_result rp_runtime_load_content(rp_runtime* rt, const char* path);`
- **Core ABI** (`retropark_abi.h`): append to `rp_core_abi`:
  `rp_result (*load_content)(rp_core* core, const char* path);` — driven cores implement it;
  presenting cores and the reference driven core leave it `NULL` (the runtime treats a null
  `load_content` as "no content needed").
- **`RETROPARK_ABI_VERSION` → 4.** Additive: existing cores append one trailing `NULL`; the three
  existing `core.json` bump to 4.

**AV-info timing:** a libretro core's `retro_get_system_av_info` is only valid *after* the ROM is
loaded. So for a driven core, the runtime queries/validates `get_av_info` **after** a successful
`load_content` (storing geometry/`max_*`); a no-content driven core (the reference core) still
validates at `load_core` as today. The runtime tracks whether content has been loaded and rejects
`present()` on a content-requiring core that has none (`RP_ERR_INTERNAL`, "content not loaded").

`CoreLoader` gains a passthrough: `rp_result load_content(const char* path, std::string& err)`
(guard state Created/Started; call `abi_->load_content`, `RP_ERR_UNSUPPORTED` if the fn is null).

---

## 2. Architecture

The shim is a driven core; the only new *algorithm* is pixel conversion, and the only new
*infrastructure* is the content path. Components:

| Component | Responsibility |
|---|---|
| `external/libretro/libretro.h` | Vendored libretro API header (permissively licensed; committed) |
| `external/libretro-cores/fceumm_libretro.dll` | The real core (downloaded; **git-ignored**, never committed) |
| `cores/libretro_shim/PixelConvert.h/.cpp` | Pure `convert_to_rgba8(src, w, h, src_pitch, format, dst)` for the 3 libretro formats (a small static lib the shim + tests link) |
| `cores/libretro_shim/LibretroShim.cpp` | The driven core: loads the libretro core, implements its callbacks + `environment`, `retro_*` lifecycle, input mapping, format conversion |
| `cores/libretro_shim/core.json` | `type: driven`, `graphics_api: none`, `abi_version: 4`, `entry: LibretroShim.dll`, `libretro_core: fceumm_libretro.dll` |

**The shim locates its libretro core** by `GetModuleFileNameW` on its own DLL → reads the sibling
`core.json`'s `libretro_core` field → `LoadLibrary` that DLL from the shim's directory (the build
copies `fceumm_libretro.dll` next to the shim when it's present).

**Pixel conversion (the one new algorithm), each producing RGBA8 `dst[0..3]=R,G,B,255`:**
- `RETRO_PIXEL_FORMAT_0RGB1555` (u16 `0RRRRRGGGGGBBBBB`): R=`(px>>10)&0x1F`, G=`(px>>5)&0x1F`,
  B=`px&0x1F`; 5→8 bits `(v<<3)|(v>>2)`.
- `RETRO_PIXEL_FORMAT_RGB565` (u16 `RRRRRGGGGGGBBBBB`): R=`(px>>11)&0x1F`, G=`(px>>5)&0x3F`,
  B=`px&0x1F`; 5→8 `(v<<3)|(v>>2)`, 6→8 `(v<<2)|(v>>4)`.
- `RETRO_PIXEL_FORMAT_XRGB8888` (u32 `0xXXRRGGBB`): R=`(px>>16)&0xFF`, G=`(px>>8)&0xFF`, B=`px&0xFF`.
Conversion reads a `src_pitch`-strided source and writes tightly-packed RGBA8 (`pitch = w*4`).

## 3. Data flow (one NES frame)

1. **Load:** frontend `load_core(shim_dir)` → shim `create` self-locates `core.json`,
   `LoadLibrary`s the libretro core, resolves `retro_*`, calls `retro_set_environment/video_refresh/
   input_poll/input_state/audio_sample(_batch)` with the shim's own functions, then `retro_init()`.
2. **Content:** frontend `rp_runtime_load_content(rt, rom.nes)` → shim reads the file → builds a
   `retro_game_info{path, data, size}` (honoring `retro_get_system_info().need_fullpath`) →
   `retro_load_game(&info)`. The shim has by now recorded the core's pixel format (from the
   `SET_PIXEL_FORMAT` environment call); the runtime queries `get_av_info` and validates geometry.
3. **Run:** each `present()` → RetroPark `run_frame` → shim `retro_run()`; FCEUmm calls the shim's
   `video_refresh(data, w, h, pitch)`; the shim **converts → RGBA8** into its own buffer and calls
   `host->video_refresh(rgba, w, h, w*4)`; RetroPark `composite_driven` uploads + overlays it on the
   active backend. `video_refresh(NULL)` forwards as RetroPark's dupe.
4. **Input:** harness keyboard → RetroPark `input_state` → the shim's libretro `input_state` maps to
   NES `RETRO_DEVICE_JOYPAD` buttons (D-pad, A, B, Start, Select).
5. **Teardown:** `retro_unload_game` + `retro_deinit` + `FreeLibrary`.

**The `environment` callback** implements the minimal set FCEUmm needs and returns `false`
otherwise: `SET_PIXEL_FORMAT` (record), `GET_CAN_DUPE` (true), `GET_SYSTEM_DIRECTORY`/
`GET_SAVE_DIRECTORY`/`GET_CORE_ASSETS_DIRECTORY` (a writable dir), `GET_LOG_INTERFACE` (a logger),
`GET_VARIABLE`/`SET_VARIABLES`/`GET_VARIABLE_UPDATE` (defaults / no-update), `SET_GEOMETRY`/
`SET_SYSTEM_AV_INFO` (update geometry), `GET_INPUT_BITMASKS`; `SET_HW_RENDER` → **false** (forces
software).

## 4. Error handling

- **Load-time:** the `libretro_core` DLL missing/unloadable → clean failure; `retro_api_version()!=1`
  or a missing `retro_*` export → reject with a specific error.
- **Content:** ROM unreadable → `RP_ERR_NOT_FOUND`; `retro_load_game` false (bad dump / unsupported
  mapper) → `RP_ERR_UNSUPPORTED` with the reason.
- **HW-render core:** `environment(SET_HW_RENDER)` returns false; a core that *requires* HW render is
  detected (no software frames) and reported "HW-render core not supported this slice," not rendered
  as garbage.
- **Pixel format:** all three formats convert; an unexpected format → `RP_ERR_UNSUPPORTED` (guarded).
- **AV-info before content:** returns zero geometry; the runtime only validates after `load_content`.
  The Slice-C `driven_frame_valid` guard already protects the upload (the shim's RGBA8 is tightly
  packed and within `max_*`).
- **Crash honesty:** a fault inside `retro_run` can still take down the host — in-process; out-of-
  process isolation remains deferred and is not claimed.

## 5. Testing

- **Unit (deterministic, portable):** `convert_to_rgba8` for `0RGB1555`, `RGB565`, `XRGB8888` —
  bit-exact against known inputs (e.g. pure red/green/blue and a padded `src_pitch`). No core needed;
  runs anywhere. This is the one real algorithm and is tested hard.
- **Gated real-core e2e (the real proof):** guarded by "do `fceumm_libretro.dll` + a configured ROM
  exist?" (paths via CMake compile-defs). If present: load the shim → `load_content(rom)` → run ~120
  `present()` frames → assert the composited readback is **(1) not uniform/near-black** (the game
  rendered) **and (2) changes between an early and a late frame** (emulation is advancing). On
  **both** backends; `WARN`-skips if the core/ROM are absent (like the probe-guarded GPU tests). It
  asserts no specific pixel (brittle per-ROM) — "non-black **and** changing" proves a real emulator
  is running, not a static blit.
- **Harness demo (human proof):** `retropark_harness --content <rom.nes>` (with `--api d3d11|vulkan`)
  loads the shim + ROM and shows the actual NES game; a screenshot is captured for verification.
- **Regression:** the whole A+B+C suite stays green — the content ABI is additive and the presenting
  and reference-driven paths are untouched (existing cores append one trailing null; core.json → 4).

## 6. Scope

**In Slice D:**
- Content ABI: `rp_runtime_load_content` + `rp_core_abi.load_content`; ABI v4; av-info validated after
  content; `CoreLoader::load_content`; runtime content-loaded gating.
- Vendored `external/libretro/libretro.h`.
- The libretro shim driven core: self-locates + loads the libretro core, minimal `environment`
  surface for software NES cores, full `retro_*` lifecycle, 3-format→RGBA8 conversion, input mapping,
  audio callbacks accepted + dropped.
- Conversion unit tests + gated real-core e2e (both backends) + harness `--content` demo.
- **FCEUmm + a real NES ROM validated end to end.**

**Explicitly out (deferred):**
- Audio output (callbacks dropped); savestate/serialize wiring; HW-render libretro cores;
  core-options/variables UI (defaults only); systems/cores beyond NES/FCEUmm (the shim is *general*
  but only NES is validated); netplay; fps pacing.
- The standing deferred list: cross-API interop, wrapping real heavy apps, out-of-process isolation,
  iOS/Android, EverythingBox integration.

**The single provable claim of Slice D:** *an unmodified libretro core (FCEUmm) loads a real NES ROM
through RetroPark's new content path, runs via the driven pull-loop, and its frames — converted to
RGBA8 — composite with the blended overlay on both D3D11 and Vulkan; proven by a non-black-and-
changing gated e2e on both backends and a harness screenshot of the actual game.*

## 7. Repo additions

```
external/
  libretro/libretro.h                 # vendored (committed)
  libretro-cores/                     # git-ignored: fceumm_libretro.dll
include/retropark/
  retropark.h                         # + rp_runtime_load_content
  retropark_abi.h                     # + load_content, ABI v4
src/loader/CoreLoader.h/.cpp          # + load_content passthrough
src/runtime/Runtime.h/.cpp            # load_content wiring, av-info-after-content, content gating
cores/libretro_shim/
  PixelConvert.h/.cpp                 # 3-format → RGBA8 (static lib, shared with tests)
  LibretroShim.cpp
  core.json                           # type driven, graphics_api none, abi_version 4, libretro_core
  CMakeLists.txt                      # builds the shim + copies fceumm dll when present
harness/windowed/main.cpp             # --content <rom>
tests/
  test_pixel_convert.cpp              # deterministic conversion
  test_libretro_e2e.cpp               # gated real-core e2e (both backends)
```
