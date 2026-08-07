# RetroPark Slice D (libretro Compatibility Shim) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load an unmodified libretro core (FCEUmm) and run a real NES ROM through RetroPark's driven path — adding a content-loading concept and a libretro→RGBA8 pixel shim — visible on both D3D11 and Vulkan.

**Architecture:** A RetroPark driven-core DLL (`LibretroShim`) `LoadLibrary`s an unmodified libretro core, implements the libretro callback + `environment` surface, converts the core's pixel format to RGBA8, and forwards frames via `video_refresh` — plugging into the Slice-C driven pipeline (both backends, zero backend changes). RetroPark gains a content path (`rp_runtime_load_content` + an ABI `load_content` hook, ABI v4) so the shim can `retro_load_game` a ROM.

**Tech Stack:** C++17, CMake, the existing D3D11+Vulkan driven path, vendored `libretro.h`, doctest. Real core: FCEUmm (downloaded to `external/libretro-cores/fceumm_libretro.dll`, git-ignored). ROMs: `C:\RetroBat\roms\nes`.

## Global Constraints

- **C++17. No Qt/EverythingBox.** MSVC `/W4 /permissive-`, warning-clean for `retropark`/shim sources.
- **Additive ABI:** the whole A+B+C suite (~51 cases) stays green after every task. `rp_core_abi` gains one trailing `load_content` fn (existing cores append `nullptr`); `rp_host_iface` is unchanged. `RETROPARK_ABI_VERSION` → `4`; the three existing `core.json` bump `abi_version` to 4. `tests/test_sanity.cpp` already compares the C-check against `RETROPARK_ABI_VERSION`.
- **Never commit** `fceumm_libretro.dll`, any `.zip`, or any ROM. `external/libretro-cores/` and `*.nes` are git-ignored. `external/libretro/libretro.h` (the vendored header) IS committed.
- **Pixel format:** the shim always reports `RP_FMT_R8G8B8A8_UNORM` to RetroPark and converts internally. Output is tightly packed (`pitch = width*4`).
- **Content gating:** a driven core that implements `load_content` (has the fn pointer) is a "content core"; the runtime defers `get_av_info` validation until after `load_content`, and `present()` returns `RP_ERR_INTERNAL` if content isn't loaded yet. A driven core with a null `load_content` (the reference core) behaves exactly as in Slice C.
- **Gated real-core tests/harness:** anything needing FCEUmm + a ROM is guarded by "do the DLL and a `.nes` exist?" and `WARN`-skips otherwise (like the probe-guarded GPU tests). Paths via CMake compile-defs: `RP_FCEUMM_PRESENT`, `RP_NES_ROM_DIR`.
- **Vulkan** GPU paths stay validation-clean; `export VULKAN_SDK=/c/VulkanSDK/1.4.357.0` before any fresh `cmake -S . -B build`.
- **Commits:** conventional prefixes. **No AI attribution** anywhere.

---

## File Structure

```
.gitignore                              # + external/libretro-cores/, *.nes                          (Task 1)
external/libretro/libretro.h            # vendored libretro API header (committed)                    (Task 2)
external/libretro-cores/fceumm_libretro.dll  # downloaded, git-ignored (already present)
include/retropark/retropark.h           # + rp_runtime_load_content                                   (Task 1)
include/retropark/retropark_abi.h       # + load_content, ABI v4                                       (Task 1)
src/loader/CoreLoader.h/.cpp            # + load_content(), has_load_content()                         (Task 1)
src/runtime/Runtime.h/.cpp              # load_content wiring, requires_content_, av-info-after-content (Task 1)
cores/refcore_present/RefCore.cpp, refcore_present_vk/RefCoreVk.cpp, tests/mock_core, test_loader.cpp  # append nullptr (Task 1)
cores/*/core.json (x3)                  # abi_version -> 4                                             (Task 1)
cores/libretro_shim/
  PixelConvert.h/.cpp                    # 0RGB1555/RGB565/XRGB8888 -> RGBA8 (static lib)              (Task 2)
  LibretroShim.cpp                        # the shim driven core                                       (Task 3)
  core.json                               # type driven, graphics_api none, abi_version 4, libretro_core (Task 3)
  CMakeLists.txt                          # shim + PixelConvert lib + copy fceumm when present         (Task 3)
harness/windowed/main.cpp               # --content <rom>                                              (Task 5)
tests/
  test_pixel_convert.cpp                 # deterministic conversion                                    (Task 2)
  test_libretro_e2e.cpp                  # gated real-core e2e (both backends)                         (Task 4)
```

---

## Task 1: Content ABI (additive) + wiring

**Files:**
- Modify: `.gitignore`, `include/retropark/retropark.h`, `include/retropark/retropark_abi.h`, `src/loader/CoreLoader.h/.cpp`, `src/runtime/Runtime.h/.cpp`, `cores/refcore_present/RefCore.cpp`, `cores/refcore_present_vk/RefCoreVk.cpp`, `tests/mock_core/MockCore.cpp`, `tests/test_loader.cpp`, the three `core.json`
- Test: `tests/test_runtime_api.cpp` (load_content dispatch), plus the full suite

**Interfaces:**
- Produces:
  - C API: `rp_result rp_runtime_load_content(rp_runtime* rt, const char* path);`
  - `rp_core_abi` appends `rp_result (*load_content)(rp_core* core, const char* path);` (last field).
  - `CoreLoader`: `rp_result load_content(const char* path, std::string& err);` and `bool has_load_content() const;`
  - `RETROPARK_ABI_VERSION` = `4u`.

- [ ] **Step 1: gitignore + ABI + C API header edits**

`.gitignore`: append `external/libretro-cores/` and `*.nes`.
`include/retropark/retropark_abi.h`: bump `RETROPARK_ABI_VERSION` `3u`→`4u`; append to `rp_core_abi` (after `unserialize`): `rp_result (*load_content)(rp_core* core, const char* path);`.
`include/retropark/retropark.h`: declare `rp_result rp_runtime_load_content(rp_runtime* rt, const char* path);`.

- [ ] **Step 2: Write the failing test**

Add to `tests/test_runtime_api.cpp`:
```cpp
TEST_CASE("runtime: load_content on a core without load_content is unsupported") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    // no core loaded yet -> content load has nothing to target
    CHECK(rp_runtime_load_content(rt, "whatever.nes") == RP_ERR_INTERNAL);
    rp_runtime_destroy(rt);
}
```

- [ ] **Step 3: Run — expect fail (unresolved rp_runtime_load_content)**

Run: `cmake --build build --config Debug`
Expected: FAIL — `rp_runtime_load_content` unresolved.

- [ ] **Step 4: CoreLoader passthroughs**

`src/loader/CoreLoader.h`: declare `rp_result load_content(const char* path, std::string& error);` and `bool has_load_content() const { return abi_ && abi_->load_content != nullptr; }`.
`src/loader/CoreLoader.cpp`:
```cpp
rp_result CoreLoader::load_content(const char* path, std::string& error) {
    if (state_ != LoaderState::Created && state_ != LoaderState::Started) { error="load_content needs Created"; return RP_ERR_INTERNAL; }
    if (!abi_->load_content) { error="core has no load_content"; return RP_ERR_UNSUPPORTED; }
    return abi_->load_content(core_, path);
}
```

- [ ] **Step 5: Runtime wiring**

`src/runtime/Runtime.h`: add `bool requires_content_ = false; bool content_loaded_ = false;` and declare `rp_result load_content(const char* path);`.
`src/runtime/Runtime.cpp`:
- In `load_core`, driven branch: after `loader_.create(...)`, set `requires_content_ = loader_.has_load_content();`. If `requires_content_`, do NOT call/validate `get_av_info` yet (defer to `load_content`); else keep the existing get_av_info validation. `content_loaded_ = false;`.
- Add:
```cpp
rp_result Runtime::load_content(const char* path) {
    if (!core_loaded_ || core_type_ != RP_CORE_DRIVEN) return RP_ERR_INTERNAL;
    std::string err;
    rp_result r = loader_.load_content(path ? path : "", err);
    if (r != RP_OK) return r;
    rp_av_info av{};
    if (loader_.get_av_info(&av, err) != RP_OK) return RP_ERR_INTERNAL;
    if (av.base_width == 0 || av.base_height == 0) return RP_ERR_UNSUPPORTED;
    if (av.pixel_format != RP_FMT_R8G8B8A8_UNORM) return RP_ERR_UNSUPPORTED;
    dr_max_w_ = std::max(av.max_width, av.base_width);
    dr_max_h_ = std::max(av.max_height, av.base_height);
    content_loaded_ = true;
    return RP_OK;
}
```
- In `present`, driven branch: at the top add `if (requires_content_ && !content_loaded_) return RP_ERR_INTERNAL;`.
- In `unload_core`: reset `requires_content_ = false; content_loaded_ = false;`.
- C API shim: `rp_result rp_runtime_load_content(rp_runtime* rt, const char* path) { return reinterpret_cast<Runtime*>(rt)->load_content(path); }`.

- [ ] **Step 6: Append nullptr in existing cores/mocks/fakes + bump manifests**

Append one trailing `nullptr` (the `load_content` slot) to the `rp_core_abi` initializers in `cores/refcore_present/RefCore.cpp`, `cores/refcore_present_vk/RefCoreVk.cpp`, `tests/mock_core/MockCore.cpp`, and both fakes in `tests/test_loader.cpp`. Bump `abi_version` 3→4 in all three existing `core.json` (`refcore_present`, `refcore_present_vk`, and — created later in Task 3 — `libretro_shim` will be 4 from the start).

- [ ] **Step 7: Build and run the full suite — green**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: all A+B+C tests still pass (presenting + driven e2e on both backends) plus the new load_content dispatch case. Warning-clean.

- [ ] **Step 8: Commit**

```bash
git add .gitignore include/retropark/ src/loader/ src/runtime/ cores/ tests/mock_core/ tests/test_loader.cpp tests/test_runtime_api.cpp
git commit -m "feat: content-loading path (rp_runtime_load_content + load_content ABI, v4)"
```

---

## Task 2: Vendor libretro.h + pixel conversion (deterministic)

**Files:**
- Create: `external/libretro/libretro.h` (vendored), `cores/libretro_shim/PixelConvert.h`, `cores/libretro_shim/PixelConvert.cpp`, `tests/test_pixel_convert.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - Vendored `libretro.h` (the standard libretro API header; defines `RETRO_PIXEL_FORMAT_0RGB1555=0`, `_XRGB8888=1`, `_RGB565=2`, `retro_*` typedefs, `retro_system_av_info`, `retro_game_info`, `RETRO_ENVIRONMENT_*`, `RETRO_DEVICE_JOYPAD`, etc.).
  - `void rp::convert_to_rgba8(const void* src, uint32_t width, uint32_t height, uint32_t src_pitch, unsigned libretro_format, uint8_t* dst);` — writes tightly-packed RGBA8 (`dst[0..3]=R,G,B,255`), a static lib `retropark_libretro_convert` linked by the shim + tests.

- [ ] **Step 1: Vendor libretro.h**

Fetch the canonical header (permissively licensed) into `external/libretro/libretro.h`:
```bash
curl -sL -o external/libretro/libretro.h https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h
head -5 external/libretro/libretro.h   # confirm it's the libretro API header
```
(If offline, copy from any local RetroArch/libretro checkout. The file is committed.)

- [ ] **Step 2: Write the failing conversion test**

`tests/test_pixel_convert.cpp`:
```cpp
#include <doctest/doctest.h>
#include "PixelConvert.h"
#include "libretro.h"
#include <vector>
#include <cstdint>
using rp::convert_to_rgba8;

TEST_CASE("convert: RGB565 pure colors -> RGBA8") {
    // one pixel each: red=0xF800, green=0x07E0, blue=0x001F
    uint16_t src[3] = {0xF800, 0x07E0, 0x001F};
    uint8_t dst[3*4] = {0};
    convert_to_rgba8(src, 3, 1, 3*2, RETRO_PIXEL_FORMAT_RGB565, dst);
    CHECK(dst[0]==255); CHECK(dst[1]==0);   CHECK(dst[2]==0);   CHECK(dst[3]==255); // red
    CHECK(dst[4]==0);   CHECK(dst[5]==255); CHECK(dst[6]==0);   CHECK(dst[7]==255); // green
    CHECK(dst[8]==0);   CHECK(dst[9]==0);   CHECK(dst[10]==255);CHECK(dst[11]==255);// blue
}
TEST_CASE("convert: 0RGB1555 pure colors -> RGBA8") {
    uint16_t src[3] = {0x7C00, 0x03E0, 0x001F}; // R,G,B in 5-5-5
    uint8_t dst[3*4] = {0};
    convert_to_rgba8(src, 3, 1, 3*2, RETRO_PIXEL_FORMAT_0RGB1555, dst);
    CHECK(dst[0]==255); CHECK(dst[1]==0);   CHECK(dst[2]==0);
    CHECK(dst[5]==255); CHECK(dst[10]==255);
}
TEST_CASE("convert: XRGB8888 -> RGBA8, respects source pitch padding") {
    // 2x2 XRGB8888 with 8 bytes row padding
    const uint32_t W=2,H=2,SP=2*4+8;
    std::vector<uint8_t> src(SP*H, 0xAA);
    auto put=[&](uint32_t x,uint32_t y,uint32_t v){ *reinterpret_cast<uint32_t*>(&src[y*SP+x*4])=v; };
    put(0,0,0x00FF0000); put(1,0,0x0000FF00); put(0,1,0x000000FF); put(1,1,0x00FFFFFF);
    uint8_t dst[W*H*4]={0};
    convert_to_rgba8(src.data(), W, H, SP, RETRO_PIXEL_FORMAT_XRGB8888, dst);
    CHECK(dst[0]==255); CHECK(dst[1]==0); CHECK(dst[2]==0);      // (0,0) red
    CHECK(dst[(2)*4+1]==255);                                    // (0,1) green channel
    CHECK(dst[(3)*4+2]==255);                                    // (1,1) blue channel of white
}
```

- [ ] **Step 3: Write the header**

`cores/libretro_shim/PixelConvert.h`:
```cpp
#pragma once
#include <cstdint>
namespace rp {
// libretro_format is a RETRO_PIXEL_FORMAT_* value. Writes width*height*4 RGBA8 bytes,
// tightly packed, dst[0..3] = R,G,B,255. Unknown format -> fills opaque black.
void convert_to_rgba8(const void* src, uint32_t width, uint32_t height,
                      uint32_t src_pitch, unsigned libretro_format, uint8_t* dst);
}
```

- [ ] **Step 4: Run — verify fail; then implement**

Run: `cmake --build build --config Debug` → FAIL (unresolved). Then `cores/libretro_shim/PixelConvert.cpp`:
```cpp
#include "PixelConvert.h"
#include "libretro.h"
namespace rp {
static inline uint8_t e5(uint32_t v){ return (uint8_t)((v<<3)|(v>>2)); }
static inline uint8_t e6(uint32_t v){ return (uint8_t)((v<<2)|(v>>4)); }
void convert_to_rgba8(const void* src, uint32_t w, uint32_t h, uint32_t src_pitch,
                      unsigned fmt, uint8_t* dst) {
    for (uint32_t y=0; y<h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(src) + (size_t)y*src_pitch;
        uint8_t* out = dst + (size_t)y*w*4;
        for (uint32_t x=0; x<w; ++x) {
            uint8_t r=0,g=0,b=0;
            if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p = reinterpret_cast<const uint32_t*>(row)[x];
                r=(p>>16)&0xFF; g=(p>>8)&0xFF; b=p&0xFF;
            } else if (fmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p = reinterpret_cast<const uint16_t*>(row)[x];
                r=e5((p>>11)&0x1F); g=e6((p>>5)&0x3F); b=e5(p&0x1F);
            } else if (fmt == RETRO_PIXEL_FORMAT_0RGB1555) {
                uint16_t p = reinterpret_cast<const uint16_t*>(row)[x];
                r=e5((p>>10)&0x1F); g=e5((p>>5)&0x1F); b=e5(p&0x1F);
            }
            out[x*4+0]=r; out[x*4+1]=g; out[x*4+2]=b; out[x*4+3]=255;
        }
    }
}
}
```

- [ ] **Step 5: Wire CMake**

Top-level `CMakeLists.txt`: add a static lib and expose the include dirs:
```cmake
add_library(retropark_libretro_convert STATIC cores/libretro_shim/PixelConvert.cpp)
target_include_directories(retropark_libretro_convert PUBLIC cores/libretro_shim external/libretro)
```
`tests/CMakeLists.txt`: add `test_pixel_convert.cpp`; `target_link_libraries(retropark_tests PRIVATE retropark_libretro_convert)`.

- [ ] **Step 6: Build and run — verify green**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: all conversion cases pass bit-exact; suite green.

- [ ] **Step 7: Commit**

```bash
git add external/libretro/libretro.h cores/libretro_shim/PixelConvert.* tests/test_pixel_convert.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: vendor libretro.h + libretro pixel-format -> RGBA8 conversion"
```

---

## Task 3: The libretro shim driven core

**Files:**
- Create: `cores/libretro_shim/LibretroShim.cpp`, `cores/libretro_shim/core.json`, `cores/libretro_shim/CMakeLists.txt`
- Modify: top-level `CMakeLists.txt`

**Interfaces:**
- Consumes: `retropark_abi.h`, `libretro.h`, `convert_to_rgba8`.
- Produces: `LibretroShim.dll` exporting `rp_get_core_abi` with `type=RP_CORE_DRIVEN`, `graphics_api=RP_GFX_NONE`, and implementing `get_info, create, destroy, get_av_info, run_frame, load_content` (presenting fns + serialize null). On `create` it loads the libretro core named in its `core.json`; `load_content` calls `retro_load_game`; `run_frame` calls `retro_run` and converts frames to RGBA8.

- [ ] **Step 1: Write the manifest**

`cores/libretro_shim/core.json`:
```json
{ "id":"libretro_shim", "name":"libretro Shim (FCEUmm)", "type":"driven",
  "abi_version":4, "graphics_api":"none", "entry":"LibretroShim.dll",
  "libretro_core":"fceumm_libretro.dll" }
```

- [ ] **Step 2: Write the shim**

`cores/libretro_shim/LibretroShim.cpp` — key structure (the implementer fills libretro-header detail; the `retro_*` names, the environment commands handled, and the callback wiring below are load-bearing and must be used exactly):
```cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <retropark/retropark.h>
#include "PixelConvert.h"
#include "libretro.h"
#include <vector>
#include <string>
#include <fstream>
#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {
struct Shim {
    rp_host_iface host{};
    HMODULE lib = nullptr;
    // libretro fn pointers
    void (*retro_init)() = nullptr;
    void (*retro_deinit)() = nullptr;
    unsigned (*retro_api_version)() = nullptr;
    void (*retro_get_system_info)(retro_system_info*) = nullptr;
    void (*retro_get_system_av_info)(retro_system_av_info*) = nullptr;
    void (*retro_set_environment)(retro_environment_t) = nullptr;
    void (*retro_set_video_refresh)(retro_video_refresh_t) = nullptr;
    void (*retro_set_audio_sample)(retro_audio_sample_t) = nullptr;
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
    void (*retro_set_input_poll)(retro_input_poll_t) = nullptr;
    void (*retro_set_input_state)(retro_input_state_t) = nullptr;
    bool (*retro_load_game)(const retro_game_info*) = nullptr;
    void (*retro_unload_game)() = nullptr;
    void (*retro_run)() = nullptr;
    // state
    unsigned pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;   // libretro default
    std::vector<uint8_t> rgba;      // converted frame
    std::vector<uint8_t> rom;       // content buffer
    std::string sys_dir;            // returned to the core
    bool game_loaded = false;
    rp_input_state input{};         // last input snapshot from RetroPark
};
Shim* g = nullptr;   // single active shim instance (libretro callbacks are global C fns)

// --- libretro callbacks (C, must be global) ---
bool env_cb(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            g->pixel_format = *static_cast<const enum retro_pixel_format*>(data); return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            *static_cast<const char**>(data) = g->sys_dir.c_str(); return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* v = static_cast<retro_variable*>(data); v->value = nullptr; return false; }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *static_cast<bool*>(data) = false; return true;
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            return false;   // force software
        default: return false;
    }
}
void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data) { g->host.video_refresh(g->host.host, nullptr, w, h, 0); return; } // dupe
    g->rgba.assign((size_t)w*h*4, 0);
    rp::convert_to_rgba8(data, w, h, (uint32_t)pitch, g->pixel_format, g->rgba.data());
    g->host.video_refresh(g->host.host, g->rgba.data(), w, h, w*4);
}
void input_poll_cb() { if (g) g->host.input_state(g->host.host, &g->input); }
int16_t input_state_cb(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || !g) return 0;
    // Map RetroPark rp_input_state.keys[] (VK codes) to NES buttons. Example mapping:
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_UP:    return g->input.keys[VK_UP]    ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:  return g->input.keys[VK_DOWN]  ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:  return g->input.keys[VK_LEFT]  ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT: return g->input.keys[VK_RIGHT] ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_A:     return g->input.keys['X']      ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_B:     return g->input.keys['Z']      ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_START: return g->input.keys[VK_RETURN]? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_SELECT:return g->input.keys[VK_SHIFT] ? 1 : 0;
        default: return 0;
    }
}
void audio_cb(int16_t, int16_t) {}                         // dropped
size_t audio_batch_cb(const int16_t*, size_t frames) { return frames; }  // dropped

std::wstring shim_dir() {  // directory of THIS dll
    wchar_t path[MAX_PATH]; HMODULE self=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&shim_dir, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring s(path); return s.substr(0, s.find_last_of(L"\\/"));
}

template<class T> void load_fn(Shim* s, T& fn, const char* name) {
    fn = reinterpret_cast<T>(GetProcAddress(s->lib, name));
}

// --- RetroPark ABI ---
void sh_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION; out->type = RP_CORE_DRIVEN;
    out->graphics_api = RP_GFX_NONE; out->id = "libretro_shim";
}
rp_core* sh_create(const rp_host_iface* host) {
    auto* s = new Shim(); s->host = *host; g = s;
    // read core.json sibling for "libretro_core"
    std::wstring dir = shim_dir();
    // (read dir\core.json, parse the "libretro_core" value; default "fceumm_libretro.dll")
    std::wstring core_dll = dir + L"\\fceumm_libretro.dll";   // implementer: parse from core.json
    s->lib = LoadLibraryW(core_dll.c_str());
    if (!s->lib) { delete s; g=nullptr; return nullptr; }
    load_fn(s, s->retro_api_version, "retro_api_version");
    load_fn(s, s->retro_init, "retro_init");  load_fn(s, s->retro_deinit, "retro_deinit");
    load_fn(s, s->retro_get_system_info, "retro_get_system_info");
    load_fn(s, s->retro_get_system_av_info, "retro_get_system_av_info");
    load_fn(s, s->retro_set_environment, "retro_set_environment");
    load_fn(s, s->retro_set_video_refresh, "retro_set_video_refresh");
    load_fn(s, s->retro_set_audio_sample, "retro_set_audio_sample");
    load_fn(s, s->retro_set_audio_sample_batch, "retro_set_audio_sample_batch");
    load_fn(s, s->retro_set_input_poll, "retro_set_input_poll");
    load_fn(s, s->retro_set_input_state, "retro_set_input_state");
    load_fn(s, s->retro_load_game, "retro_load_game");
    load_fn(s, s->retro_unload_game, "retro_unload_game");
    load_fn(s, s->retro_run, "retro_run");
    if (!s->retro_api_version || s->retro_api_version() != 1 || !s->retro_run || !s->retro_load_game) {
        FreeLibrary(s->lib); delete s; g=nullptr; return nullptr;
    }
    s->sys_dir = ".";
    s->retro_set_environment(env_cb);
    s->retro_set_video_refresh(video_cb);
    s->retro_set_input_poll(input_poll_cb);
    s->retro_set_input_state(input_state_cb);
    s->retro_set_audio_sample(audio_cb);
    s->retro_set_audio_sample_batch(audio_batch_cb);
    s->retro_init();
    return reinterpret_cast<rp_core*>(s);
}
rp_result sh_load_content(rp_core* core, const char* path) {
    auto* s = reinterpret_cast<Shim*>(core);
    std::ifstream f(path, std::ios::binary);
    if (!f) return RP_ERR_NOT_FOUND;
    s->rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    retro_system_info si{}; s->retro_get_system_info(&si);
    retro_game_info gi{}; gi.path = path;
    if (!si.need_fullpath) { gi.data = s->rom.data(); gi.size = s->rom.size(); }
    if (!s->retro_load_game(&gi)) return RP_ERR_UNSUPPORTED;
    s->game_loaded = true; return RP_OK;
}
void sh_get_av_info(rp_core*, rp_av_info* out) {
    retro_system_av_info av{}; g->retro_get_system_av_info(&av);
    out->fps = av.timing.fps; out->sample_rate = av.timing.sample_rate;
    out->base_width = av.geometry.base_width; out->base_height = av.geometry.base_height;
    out->max_width = av.geometry.max_width; out->max_height = av.geometry.max_height;
    out->pixel_format = RP_FMT_R8G8B8A8_UNORM;   // shim always outputs RGBA8
}
void sh_run_frame(rp_core* core) { auto* s=reinterpret_cast<Shim*>(core); if (s->game_loaded) s->retro_run(); }
void sh_destroy(rp_core* core) {
    auto* s = reinterpret_cast<Shim*>(core);
    if (s->game_loaded && s->retro_unload_game) s->retro_unload_game();
    if (s->retro_deinit) s->retro_deinit();
    if (s->lib) FreeLibrary(s->lib);
    if (g==s) g=nullptr; delete s;
}
const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, sh_get_info, sh_create, sh_destroy,
    nullptr, nullptr, nullptr,          // set_surfaces, start, stop
    sh_get_av_info, sh_run_frame,
    nullptr, nullptr, nullptr,          // serialize_size, serialize, unserialize
    sh_load_content
};
}
RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
```
Implementer notes: parse the actual `libretro_core` value from `core.json` (don't hardcode) using the same JSON lib the loader uses; set `sys_dir` to a real writable path (the shim dir); ensure the `kAbi` initializer order matches the current `rp_core_abi` field order exactly (get_info, create, destroy, set_surfaces, start, stop, get_av_info, run_frame, serialize_size, serialize, unserialize, load_content).

- [ ] **Step 3: Shim CMake (+ copy fceumm when present)**

`cores/libretro_shim/CMakeLists.txt`:
```cmake
add_library(LibretroShim SHARED LibretroShim.cpp)
target_include_directories(LibretroShim PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/external/libretro)
target_link_libraries(LibretroShim PRIVATE retropark_libretro_convert)
set(SHIM_OUT $<TARGET_FILE_DIR:LibretroShim>/cores/libretro_shim)
add_custom_command(TARGET LibretroShim POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHIM_OUT}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:LibretroShim> ${SHIM_OUT}/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/core.json ${SHIM_OUT}/)
# Copy the real core next to the shim IF it was downloaded (never required for build).
if (EXISTS ${CMAKE_SOURCE_DIR}/external/libretro-cores/fceumm_libretro.dll)
  add_custom_command(TARGET LibretroShim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/external/libretro-cores/fceumm_libretro.dll ${SHIM_OUT}/)
endif()
```
Top-level `CMakeLists.txt`: `add_subdirectory(cores/libretro_shim)`.

- [ ] **Step 4: Build — confirm shim (+ fceumm) emitted**

Run: `cmake -S . -B build && cmake --build build --config Debug`
Expected: `LibretroShim.dll` + `core.json` (+ `fceumm_libretro.dll`, since it's present) under `build/.../cores/libretro_shim/`. Full suite still green (this task adds no test; behavior proven in Task 4).

- [ ] **Step 5: Commit**

```bash
git add cores/libretro_shim/LibretroShim.cpp cores/libretro_shim/core.json cores/libretro_shim/CMakeLists.txt CMakeLists.txt
git commit -m "feat: libretro shim driven core (loads a libretro core, converts frames to RGBA8)"
```

---

## Task 4: Gated real-core end-to-end (both backends)

**Files:**
- Create: `tests/test_libretro_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: the public C API + the emitted `libretro_shim` package (with `fceumm_libretro.dll`) + a NES ROM.

- [ ] **Step 1: Write the gated e2e test**

`tests/test_libretro_e2e.cpp` — compile-defs `RP_SHIM_DIR` (the emitted shim package dir) and `RP_NES_ROM_DIR`. Helper: find the first `*.nes` in `RP_NES_ROM_DIR`; check `RP_SHIM_DIR/fceumm_libretro.dll` exists. Skip (WARN) if either is absent.
```cpp
static void run_libretro(rp_graphics_api api) {
    std::string rom = first_nes(RP_NES_ROM_DIR);
    if (rom.empty() || !file_exists(std::string(RP_SHIM_DIR) + "/fceumm_libretro.dll")) { WARN("no core/rom; skip"); return; }
    const uint32_t W=256, H=240;   // NES resolution
    rp_runtime* rt = rp_runtime_create(api, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_SHIM_DIR) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, rom.c_str()) == RP_OK);
    std::vector<uint8_t> early((size_t)W*H*4,0), late((size_t)W*H*4,0);
    for (int i=0;i<10;i++) rp_runtime_present(rt, early.data());     // warm up + early frame
    for (int i=0;i<110;i++) rp_runtime_present(rt, late.data());     // advance ~110 more frames
    // (1) not near-black: some pixel is meaningfully bright
    uint64_t sum=0; for (uint8_t v : late) sum += v;
    CHECK(sum > (uint64_t)W*H);        // not an all-black frame
    // (2) changed across frames: early != late
    CHECK(early != late);
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
TEST_CASE("libretro e2e: D3D11 runs a real NES ROM") { run_libretro(RP_GFX_D3D11); }
TEST_CASE("libretro e2e: Vulkan runs a real NES ROM") {
    if (!rp::VulkanBackend::probe_vulkan_shared()) { WARN("no vulkan"); return; }
    run_libretro(RP_GFX_VULKAN);
}
```
(Implementer: write `first_nes`/`file_exists` with `<filesystem>`; the ROM readback buffers are display-sized W*H per the driven readback.)

- [ ] **Step 2: Wire CMake**

`tests/CMakeLists.txt`: `add_dependencies(retropark_tests LibretroShim)`,
`target_compile_definitions(retropark_tests PRIVATE RP_SHIM_DIR="$<TARGET_FILE_DIR:LibretroShim>/cores/libretro_shim" RP_NES_ROM_DIR="C:/RetroBat/roms/nes")`. Add `test_libretro_e2e.cpp`.

- [ ] **Step 3: Build and run — verify (or SKIP), both backends**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the libretro e2e RUNS (fceumm + a ROM are present on this machine), a real NES game advances — the frame is non-black and changes across ~120 frames — on D3D11 and Vulkan (validation-clean). **Report RAN vs SKIPPED, the brightness sum, and that early≠late.** The whole A+B+C suite stays green.

- [ ] **Step 4: Commit**

```bash
git add tests/test_libretro_e2e.cpp tests/CMakeLists.txt
git commit -m "test: gated libretro e2e — real NES ROM via FCEUmm on both backends"
```

---

## Task 5: Harness `--content` (see the game)

**Files:**
- Modify: `harness/windowed/main.cpp`, `harness/windowed/CMakeLists.txt`, top-level `CMakeLists.txt`

**Interfaces:**
- Produces: the harness accepts `--content <rom.nes>`; when set it loads the `libretro_shim` core and then `rp_runtime_load_content(rt, <rom>)`, so the window shows the real game. `--api d3d11|vulkan` selects the backend as before.

- [ ] **Step 1: Parameterize the harness**

`harness/windowed/main.cpp`: parse `--content <path>` (the next arg is the ROM path). When present: load core dir `RP_HARNESS_SHIM_DIR` and, after `rp_runtime_load_core`, call `rp_runtime_load_content(rt, rom)`. When absent, keep existing behavior (`--driven` / presenting). Resize to 256×240 (NES) when running content. Present loop unchanged.

- [ ] **Step 2: Wire CMake**

`harness/windowed/CMakeLists.txt`: `add_dependencies(retropark_harness LibretroShim)` and `target_compile_definitions(retropark_harness PRIVATE RP_HARNESS_SHIM_DIR="$<TARGET_FILE_DIR:LibretroShim>/cores/libretro_shim")`.

- [ ] **Step 3: Build + smoke the game on both backends**

Run: `cmake -S . -B build && cmake --build build --config Debug`. Pick a recognizable ROM path from `C:\RetroBat\roms\nes`. Launch `retropark_harness.exe --api d3d11 --content "<rom>"` (background ~3s), `PrintWindow`-capture to `C:\Users\cubma\source\repos\RetroPark\.superpowers\sdd\harness-nes-d3d11-shot.png`, terminate. Repeat `--api vulkan --content "<rom>"` → `harness-nes-vk-shot.png`. Confirm BOTH windows show the actual NES game (title/gameplay), with the overlay quad. If black, diagnose (shim core load, content load, conversion, the driven windowed present) — do not commit a black window.

- [ ] **Step 4: Commit**

```bash
git add harness/windowed/ CMakeLists.txt
git commit -m "feat: harness --content runs a real NES ROM through the libretro shim"
```

---

## Self-Review

**Spec coverage:**
- §1 content ABI (rp_runtime_load_content, load_content, v4, av-info-after-content, gating) → Task 1. ✓
- §2 architecture (shim as driven core; self-locates + loads libretro core; env surface; conversion) → Task 3 (shim) + Task 2 (conversion + vendored header). ✓
- §2 pixel conversion formulas → Task 2 (bit-exact, unit-tested). ✓
- §3 data flow (load → load_content → run_frame→retro_run→video_cb→convert→host; input; teardown) → Task 3. ✓
- §4 error handling: missing core dll / api!=1 / missing exports → Task 3 (create rejects); ROM unreadable → RP_ERR_NOT_FOUND, retro_load_game false → RP_ERR_UNSUPPORTED (Task 3 load_content); SET_HW_RENDER→false (Task 3 env_cb); dupe video_refresh(NULL) forwarded (Task 3 video_cb); av-info-after-content + gating (Task 1). ✓
- §5 testing: conversion unit (Task 2), gated real-core e2e both backends non-black+changing (Task 4), harness demo (Task 5), A+B+C regression (every task). ✓
- §6 scope: all in-scope built; audio dropped (audio_cb/batch no-op), serialize null, HW-render false, only NES validated — nothing out-of-scope built. ✓

**Placeholder scan:** Task 3 leaves two implementer specifics explicitly called out (parse `libretro_core` from `core.json` rather than the hardcoded default shown; set a real `sys_dir`) — these are noted as required, not vague TODOs, and the rest of the shim (callback wiring, env commands, lifecycle, the exact `retro_*` names, the `kAbi` order) is concrete. No "add error handling"-style placeholders.

**Type consistency:** `load_content(rp_core*, const char*)` on the ABI, `CoreLoader::load_content`/`has_load_content`, `rp_runtime_load_content`, `convert_to_rgba8(const void*, uint32_t, uint32_t, uint32_t, unsigned, uint8_t*)`, and the `kAbi` field order (…, get_av_info, run_frame, serialize_size, serialize, unserialize, load_content) are used identically across Tasks 1–5. The reference/mock cores append exactly one trailing `nullptr` (the load_content slot), matching the appended field.
```
