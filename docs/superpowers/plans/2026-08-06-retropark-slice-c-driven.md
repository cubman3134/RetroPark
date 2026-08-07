# RetroPark Slice C (Driven Execution Model) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the driven (libretro-style) execution model — the host calls a core's `run_frame()` once per `present()`, the core hands back a CPU RGBA8 framebuffer via a `video_refresh` callback, and the host uploads it and composites it with the blended overlay — on both D3D11 and Vulkan.

**Architecture:** Driven is the pull-mirror of presenting: no core thread, no shared GPU surface, no sync. The core emits CPU pixels (graphics-API-agnostic); the backend uploads them into an internal sampled texture and reuses the existing compositor + overlay. A reference driven core (RGBA8, animated, no GPU) runs under either backend. All ABI changes are additive; the entire A+B presenting suite stays green.

**Tech Stack:** C++17, CMake, D3D11 + Vulkan (existing backends), doctest. Reuses the Slice A/B compositor/overlay. Vulkan SDK at `C:\VulkanSDK\1.4.357.0`.

## Global Constraints

- **C++17. No Qt/EverythingBox.** MSVC `/W4 /permissive-`, warning-clean for `retropark` sources.
- **Additive ABI:** the entire existing A+B suite (presenting D3D11 + Vulkan, ~42 cases) must still pass after every task. Presenting cores leave the new `rp_core_abi` fields NULL; the new `rp_host_iface` callbacks are installed by the runtime and unused by presenting cores.
- **`RETROPARK_ABI_VERSION` bumps to `3`** (the `rp_core_abi`/`rp_host_iface` shapes grew). The three existing `core.json` files bump `abi_version` to 3; `tests/test_sanity.cpp` already compares against `RETROPARK_ABI_VERSION` (from Slice B), so no literal update needed there.
- **Pixel format:** RGBA8 only (`RP_FMT_R8G8B8A8_UNORM` = `DXGI_FORMAT_R8G8B8A8_UNORM` = `VK_FORMAT_R8G8B8A8_UNORM`). A driven core declaring another format → `RP_ERR_UNSUPPORTED`.
- **`video_refresh(data==NULL)` = duplicate the last frame** (keep the previous texture, overlay stays live). Missing `run_frame` call in a `present` is also treated as dupe.
- **Pitch:** `pitch` (bytes/row) may exceed `width*4`; uploads copy exactly `width*4` per row from a `pitch`-strided source. Validated with a padded-pitch test.
- **Timing:** one `run_frame` per `present`, host-paced (no fps pacing this slice).
- **Vulkan GPU tests** are probe-guarded (`VulkanBackend::probe_vulkan_shared()`), validation-clean in Debug. D3D11 headless uses WARP. If a fresh `cmake -S . -B build` is needed, `export VULKAN_SDK=/c/VulkanSDK/1.4.357.0` first; incremental `cmake --build build --config Debug` uses cached paths.
- **Commits:** conventional prefixes. **No AI attribution** anywhere.

---

## File Structure

```
include/retropark/retropark_abi.h        # + rp_av_info, driven core fns, host callbacks, RP_GFX_NONE, ABI v3   (Task 1)
src/loader/Manifest.cpp                   # accept "none" graphics_api                                            (Task 1)
src/loader/CoreLoader.h/.cpp              # + run_frame()/get_av_info() passthroughs                              (Task 6)
src/render/
  FramebufferCopy.h/.cpp                  # pitch-respecting RGBA8 row copy (pure)                                (Task 2)
  IRenderBackend.h                         # + composite_driven (pure virtual)                                    (Task 2)
  d3d11/D3D11Backend.h/.cpp                # composite_driven (dynamic texture upload)                             (Task 3)
  vulkan/VulkanBackend.h/.cpp              # composite_driven (staging buffer → copy → sample)                     (Task 4)
src/runtime/Runtime.h/.cpp                # driven dispatch, video_refresh trampoline, core_type_                 (Task 6)
cores/refcore_driven/
  RefCoreDriven.cpp, core.json, CMakeLists.txt   # CPU RGBA8 animated core, no GPU                                (Task 5)
harness/windowed/main.cpp                 # --driven loads the driven core                                        (Task 7)
tests/
  test_framebuffer_copy.cpp               # (Task 2)
  test_driven_upload_d3d11.cpp            # (Task 3)
  test_driven_upload_vulkan.cpp          # (Task 4)
  test_driven_e2e.cpp                     # (Task 6)
  mock_core/MockCore.cpp, test_loader.cpp # ABI field additions (null the new fns)                                (Task 1)
```

---

## Task 1: ABI evolution (driven entry points, additive)

**Files:**
- Modify: `include/retropark/retropark_abi.h`, `src/loader/Manifest.cpp`, `cores/refcore_present/RefCore.cpp`, `cores/refcore_present_vk/RefCoreVk.cpp`, `tests/mock_core/MockCore.cpp`, `tests/test_loader.cpp`, `cores/refcore_present/core.json`, `cores/refcore_present_vk/core.json`, `tests/test_manifest.cpp`
- Test: `tests/test_manifest.cpp` (accept "none"), plus the full existing suite

**Interfaces:**
- Produces:
  - `struct rp_av_info { double fps; double sample_rate; uint32_t base_width, base_height, max_width, max_height; uint32_t pixel_format; };`
  - `rp_core_abi` gains (appended, after `stop`): `void (*get_av_info)(rp_core*, rp_av_info*); void (*run_frame)(rp_core*); size_t (*serialize_size)(rp_core*); rp_result (*serialize)(rp_core*, void*, size_t); rp_result (*unserialize)(rp_core*, const void*, size_t);`
  - `rp_host_iface` gains (appended): `void (*video_refresh)(rp_host*, const void* data, uint32_t width, uint32_t height, uint32_t pitch); void (*audio_sample)(rp_host*, const int16_t* frames, size_t num_frames);`
  - `rp_graphics_api` gains `RP_GFX_NONE = 2`.
  - `RETROPARK_ABI_VERSION` = `3u`.

- [ ] **Step 1: Edit the ABI header**

In `include/retropark/retropark_abi.h`:
- Bump `#define RETROPARK_ABI_VERSION 2u` → `3u`.
- Add `RP_GFX_NONE = 2` to the `rp_graphics_api` enum.
- Add the `rp_av_info` struct (after `rp_surface_desc`).
- Append to `rp_host_iface` (after the existing members): `video_refresh` and `audio_sample` (signatures above).
- Append to `rp_core_abi` (after `stop`): `get_av_info`, `run_frame`, `serialize_size`, `serialize`, `unserialize` (signatures above).

- [ ] **Step 2: Write the failing manifest test**

Add to `tests/test_manifest.cpp`:
```cpp
TEST_CASE("manifest: graphics_api none is accepted (driven cores)") {
    CoreManifest m; std::string err;
    const char* j = R"({"id":"d","name":"n","type":"driven","abi_version":3,
                        "graphics_api":"none","entry":"d.dll"})";
    CHECK(parse_manifest(j, m, err) == RP_OK);
    CHECK(m.graphics_api == RP_GFX_NONE);
}
```

- [ ] **Step 3: Run — expect fail**

Run: `cmake --build build --config Debug`
Expected: the manifest test fails (`"none"` currently rejected as unknown graphics_api).

- [ ] **Step 4: Accept "none" in the manifest parser**

`src/loader/Manifest.cpp`, in the graphics_api mapping block, add before the else:
```cpp
else if (gfx_s == "none") out.graphics_api = RP_GFX_NONE;
```

- [ ] **Step 5: Null the new fields in existing cores/mocks/fakes**

The `rp_core_abi` aggregate initializers in `cores/refcore_present/RefCore.cpp`, `cores/refcore_present_vk/RefCoreVk.cpp`, `tests/mock_core/MockCore.cpp`, and the `kGoodAbi`/`kBadVersionAbi` fakes in `tests/test_loader.cpp` now have 5 more trailing fields. Append `, nullptr, nullptr, nullptr, nullptr, nullptr` (get_av_info, run_frame, serialize_size, serialize, unserialize) to each initializer so they stay well-formed. These are presenting cores; the driven fields are unused.

- [ ] **Step 6: Bump the existing manifests**

`cores/refcore_present/core.json` and `cores/refcore_present_vk/core.json`: `"abi_version": 2` → `"abi_version": 3`.

- [ ] **Step 7: Build and run the full suite — everything green**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: all existing A+B tests still pass (presenting D3D11 + Vulkan e2e both green) plus the new manifest case. Warning-clean.

- [ ] **Step 8: Commit**

```bash
git add include/retropark/retropark_abi.h src/loader/Manifest.cpp cores/ tests/mock_core/ tests/test_loader.cpp tests/test_manifest.cpp
git commit -m "feat: driven ABI entry points (av_info/run_frame/video_refresh), ABI v3"
```

---

## Task 2: Framebuffer copy helper + `composite_driven` interface

**Files:**
- Create: `src/render/FramebufferCopy.h`, `src/render/FramebufferCopy.cpp`, `tests/test_framebuffer_copy.cpp`
- Modify: `src/render/IRenderBackend.h`, `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `void rp::copy_rgba8_rows(const uint8_t* src, uint32_t width, uint32_t height, uint32_t src_pitch, uint8_t* dst, uint32_t dst_pitch);` — copies `width*4` bytes per row from a `src_pitch`-strided source to a `dst_pitch`-strided destination.
  - `IRenderBackend::composite_driven(const void* data, uint32_t width, uint32_t height, uint32_t pitch, bool dupe, uint8_t* out_rgba, std::string& err)` — pure virtual.

- [ ] **Step 1: Write the failing test**

`tests/test_framebuffer_copy.cpp`:
```cpp
#include <doctest/doctest.h>
#include "render/FramebufferCopy.h"
#include <vector>
using rp::copy_rgba8_rows;

TEST_CASE("fbcopy: respects source pitch padding") {
    // 2x2 image, source has 4 bytes row padding (src_pitch = 2*4 + 4 = 12), dst tightly packed (8).
    const uint32_t W=2,H=2,SRCP=12,DSTP=8;
    std::vector<uint8_t> src(SRCP*H, 0xCC);   // padding filled with 0xCC
    // row0 pixels
    for (int i=0;i<8;i++) src[i] = (uint8_t)i;
    // row1 pixels
    for (int i=0;i<8;i++) src[SRCP + i] = (uint8_t)(100+i);
    std::vector<uint8_t> dst(DSTP*H, 0);
    copy_rgba8_rows(src.data(), W, H, SRCP, dst.data(), DSTP);
    for (int i=0;i<8;i++) CHECK(dst[i] == (uint8_t)i);
    for (int i=0;i<8;i++) CHECK(dst[8+i] == (uint8_t)(100+i));
    // padding must NOT have been copied
    CHECK(dst.size() == 16);
}
```

- [ ] **Step 2: Write the header + interface addition**

`src/render/FramebufferCopy.h`:
```cpp
#pragma once
#include <cstdint>
namespace rp {
void copy_rgba8_rows(const uint8_t* src, uint32_t width, uint32_t height,
                     uint32_t src_pitch, uint8_t* dst, uint32_t dst_pitch);
}
```
In `src/render/IRenderBackend.h`, add the pure virtual:
```cpp
virtual rp_result composite_driven(const void* data, uint32_t width, uint32_t height,
                                   uint32_t pitch, bool dupe, uint8_t* out_rgba,
                                   std::string& err) = 0;
```

- [ ] **Step 3: Run — expect fail (link error + D3D11Backend/VulkanBackend now abstract)**

Run: `cmake --build build --config Debug`
Expected: FAIL — `copy_rgba8_rows` unresolved AND `D3D11Backend`/`VulkanBackend` can't instantiate (missing `composite_driven`). That's expected; Tasks 3–4 implement the backends. To keep this task's build green, add a TEMPORARY `composite_driven` stub returning `RP_ERR_UNSUPPORTED` to BOTH `D3D11Backend` and `VulkanBackend` in this task (Tasks 3/4 replace them). State this in the commit.

- [ ] **Step 4: Implement the helper + backend stubs**

`src/render/FramebufferCopy.cpp`:
```cpp
#include "render/FramebufferCopy.h"
#include <cstring>
namespace rp {
void copy_rgba8_rows(const uint8_t* src, uint32_t width, uint32_t height,
                     uint32_t src_pitch, uint8_t* dst, uint32_t dst_pitch) {
    const uint32_t row_bytes = width * 4u;
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(dst + (size_t)y * dst_pitch, src + (size_t)y * src_pitch, row_bytes);
}
}
```
Add to `D3D11Backend` and `VulkanBackend` (header + cpp) a temporary:
```cpp
rp_result composite_driven(const void*, uint32_t, uint32_t, uint32_t, bool, uint8_t*, std::string& err) override {
    err = "composite_driven not implemented yet"; return RP_ERR_UNSUPPORTED;
}
```

- [ ] **Step 5: Wire CMake, build, run**

Add `src/render/FramebufferCopy.cpp` to `retropark`; add `test_framebuffer_copy.cpp` to tests.
Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: fbcopy test passes; suite green (backends compile with the stub).

- [ ] **Step 6: Commit**

```bash
git add src/render/FramebufferCopy.* src/render/IRenderBackend.h src/render/d3d11/ src/render/vulkan/ tests/test_framebuffer_copy.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: framebuffer row-copy helper + composite_driven interface (backend stubs)"
```

---

## Task 3: D3D11 `composite_driven` (CPU upload → composite)

**Files:**
- Modify: `src/render/d3d11/D3D11Backend.h/.cpp`
- Create: `tests/test_driven_upload_d3d11.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `copy_rgba8_rows`, the existing `D3D11Compositor`.
- Produces: real `D3D11Backend::composite_driven` — uploads the CPU RGBA8 buffer into an internal `DYNAMIC` sampled texture (recreated on size change), composites it + overlay via the existing compositor into the offscreen RTV (headless) or backbuffer (windowed), reads back into `out_rgba` when non-null. `dupe==true` reuses the last texture.

- [ ] **Step 1: Write the failing headless test**

`tests/test_driven_upload_d3d11.cpp`:
```cpp
#include <doctest/doctest.h>
#include "render/d3d11/D3D11Backend.h"
#include <vector>
using namespace rp;

TEST_CASE("d3d11 driven: uploaded framebuffer (padded pitch) shows + overlay blends") {
    const uint32_t W=64,H=64,PITCH=W*4 + 16;   // 16 bytes row padding
    D3D11Backend b; std::string err;
    REQUIRE(b.initialize(nullptr, W, H, err) == RP_OK);
    std::vector<uint8_t> src((size_t)PITCH*H, 0);
    for (uint32_t y=0;y<H;y++) for (uint32_t x=0;x<W;x++) {
        uint8_t* p = src.data() + (size_t)y*PITCH + (size_t)x*4;
        p[0]=0; p[1]=255; p[2]=0; p[3]=255;   // solid green, padding left 0
    }
    std::vector<uint8_t> out((size_t)W*H*4, 0);
    REQUIRE(b.composite_driven(src.data(), W, H, PITCH, /*dupe=*/false, out.data(), err) == RP_OK);
    auto at=[&](uint32_t x,uint32_t y,int c){ return out[((size_t)y*W+x)*4+c]; };
    CHECK(at(60,60,1) > 200); CHECK(at(60,60,2) < 60);      // green outside overlay
    CHECK(at(4,4,2) > 80);    CHECK(at(4,4,1) < at(60,60,1)); // blended inside overlay
}
```

- [ ] **Step 2: Run — expect fail (stub returns UNSUPPORTED)**

Run: `cmake --build build --config Debug`; the test fails at the `REQUIRE(... == RP_OK)`.

- [ ] **Step 3: Implement `composite_driven`**

In `D3D11Backend.h` add members: `Microsoft::WRL::ComPtr<ID3D11Texture2D> driven_tex_; Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> driven_srv_; uint32_t driven_w_=0, driven_h_=0;`. Replace the stub in `.cpp`:
- If `!dupe`: if `driven_tex_` is null or size changed, (re)create a `DYNAMIC`, `R8G8B8A8_UNORM`, `BIND_SHADER_RESOURCE` texture of `width×height` + its SRV. `Map(WRITE_DISCARD)` it, `copy_rgba8_rows((const uint8_t*)data, width, height, pitch, (uint8_t*)mapped.pData, mapped.RowPitch)`, `Unmap`.
- Ensure the compositor + offscreen (or backbuffer) target exist (reuse the existing `ensure`/lazy-create used by `composite_and_present`).
- Run the compositor: draw the driven SRV fullscreen + blended overlay into the target (reuse `compositor_.render(ctx_.Get(), target, driven_srv_.Get(), width, height, err)` or the existing render entry).
- If `out_rgba`: copy target → staging → map → fill `out_rgba` (reuse the existing readback path). If `swapchain_`: `Present`. `dupe` with a null `driven_srv_` (no frame yet) → composite overlay-only (pass a null core SRV).

- [ ] **Step 4: Build and run — verify green**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the driven upload test passes — green shows (proving the padded-pitch upload) and the overlay blends. D3D11 presenting e2e still green.

- [ ] **Step 5: Commit**

```bash
git add src/render/d3d11/D3D11Backend.* tests/test_driven_upload_d3d11.cpp tests/CMakeLists.txt
git commit -m "feat: D3D11 composite_driven (CPU framebuffer upload + composite)"
```

---

## Task 4: Vulkan `composite_driven` (staging upload → composite)

**Files:**
- Modify: `src/render/vulkan/VulkanBackend.h/.cpp`
- Create: `tests/test_driven_upload_vulkan.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `copy_rgba8_rows`, the existing `VulkanCompositor`.
- Produces: real `VulkanBackend::composite_driven` — copies the CPU buffer (pitch-respecting, tightly packed) into a host-visible staging buffer, `vkCmdCopyBufferToImage` into an internal sampled `VkImage` (a NORMAL image, not shared — no external memory, no QFOT), transitions it to `SHADER_READ_ONLY_OPTIMAL`, composites it + overlay via the existing compositor, reads back (headless) or presents (windowed). `dupe` reuses the last image.

- [ ] **Step 1: Write the failing headless test (probe-guarded)**

`tests/test_driven_upload_vulkan.cpp`: mirror the D3D11 driven test but with `VulkanBackend`, guarded by `if (!VulkanBackend::probe_vulkan_shared()) { WARN("skip"); return; }`. Same padded pitch (W*4+16), solid green source, assert green outside the overlay + blended inside.

- [ ] **Step 2: Run — expect fail (stub)**

Run: `cmake --build build --config Debug`; the test fails at `REQUIRE(... == RP_OK)` (or skips if no device — it should RUN on the RTX 5080).

- [ ] **Step 3: Implement `composite_driven`**

In `VulkanBackend.h` add members for a driven upload path: a host-visible staging `VkBuffer`+memory, an internal sampled `VkImage`+view+memory, and `driven_w_/driven_h_`. Replace the stub in `.cpp`:
- If `!dupe`: (re)create the staging buffer (size `width*height*4`) and the sampled image (`R8G8B8A8_UNORM`, `TRANSFER_DST|SAMPLED`, DEVICE_LOCAL) on size change. Map the staging buffer; `copy_rgba8_rows((const uint8_t*)data, width, height, pitch, mapped, width*4)` (tightly packed dst); unmap/flush. Record: transition image `UNDEFINED→TRANSFER_DST` (QUEUE_FAMILY_IGNORED — this is a normal single-device image, NOT a QFOT), `vkCmdCopyBufferToImage`, transition `TRANSFER_DST→SHADER_READ_ONLY_OPTIMAL`; submit; fence-wait (finite 1e9 ns).
- Ensure the compositor + offscreen target exist (reuse the `ensure_composite_resources` used by `composite_and_present`). The descriptor image layout for the driven image is `SHADER_READ_ONLY_OPTIMAL` (a normal sampled image — unlike the shared images which stay GENERAL).
- Run the compositor sampling the driven image view into the offscreen (or swapchain) target; read back into `out_rgba` (headless) or present (windowed). `dupe` with no image yet → overlay-only.
- No timeline semaphore, no `last_present_sync_`, no external memory — this is entirely host-local.

- [ ] **Step 4: Build and run — verify green (or SKIP), validation-clean**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the Vulkan driven upload test RUNS and passes — green outside, blend inside, validation-clean. **Report RAN vs SKIPPED + any validation output.** Vulkan presenting e2e still green.

- [ ] **Step 5: Commit**

```bash
git add src/render/vulkan/VulkanBackend.* tests/test_driven_upload_vulkan.cpp tests/CMakeLists.txt
git commit -m "feat: Vulkan composite_driven (staging upload + composite)"
```

---

## Task 5: Reference driven core (`refcore_driven.dll`)

**Files:**
- Create: `cores/refcore_driven/RefCoreDriven.cpp`, `cores/refcore_driven/core.json`, `cores/refcore_driven/CMakeLists.txt`
- Modify: top-level `CMakeLists.txt`

**Interfaces:**
- Produces: a DLL exporting `rp_get_core_abi` with `type = RP_CORE_DRIVEN`, `graphics_api = RP_GFX_NONE`. `get_av_info` reports 64×64 RGBA8, fps 60, sample_rate 0. `run_frame` fills a CPU RGBA8 buffer (green with rising blue) and calls `host->video_refresh(buf, 64, 64, 64*4)`. Touches no GPU API — links only the ABI header.

- [ ] **Step 1: Write the manifest**

`cores/refcore_driven/core.json`:
```json
{ "id":"refcore_driven", "name":"Reference Driven Core", "type":"driven",
  "abi_version":3, "graphics_api":"none", "entry":"refcore_driven.dll" }
```

- [ ] **Step 2: Write the core**

`cores/refcore_driven/RefCoreDriven.cpp`:
```cpp
#include <retropark/retropark_abi.h>
#include <vector>
#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {
struct DrivenCore {
    rp_host_iface host{};
    std::vector<uint8_t> fb;   // 64*64*4
    uint32_t frame = 0;
    static const uint32_t W = 64, H = 64;
};

void dc_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_DRIVEN;
    out->graphics_api = RP_GFX_NONE;
    out->id = "refcore_driven";
}
void dc_get_av_info(rp_core*, rp_av_info* out) {
    out->fps = 60.0; out->sample_rate = 0.0;
    out->base_width = DrivenCore::W; out->base_height = DrivenCore::H;
    out->max_width = DrivenCore::W; out->max_height = DrivenCore::H;
    out->pixel_format = RP_FMT_R8G8B8A8_UNORM;
}
rp_core* dc_create(const rp_host_iface* host) {
    auto* c = new DrivenCore();
    c->host = *host;
    c->fb.assign((size_t)DrivenCore::W * DrivenCore::H * 4, 0);
    return reinterpret_cast<rp_core*>(c);
}
void dc_destroy(rp_core* core) { delete reinterpret_cast<DrivenCore*>(core); }
void dc_run_frame(rp_core* core) {
    auto* c = reinterpret_cast<DrivenCore*>(core);
    uint8_t t = (uint8_t)((c->frame++ % 120) * 255 / 120);   // rising blue
    for (uint32_t i = 0; i < DrivenCore::W * DrivenCore::H; ++i) {
        uint8_t* p = c->fb.data() + (size_t)i * 4;
        p[0] = 0; p[1] = 255; p[2] = t; p[3] = 255;          // green with rising blue
    }
    c->host.video_refresh(c->host.host, c->fb.data(), DrivenCore::W, DrivenCore::H, DrivenCore::W * 4);
}

const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, dc_get_info, dc_create, dc_destroy,
    /*set_surfaces*/nullptr, /*start*/nullptr, /*stop*/nullptr,
    dc_get_av_info, dc_run_frame,
    /*serialize_size*/nullptr, /*serialize*/nullptr, /*unserialize*/nullptr
};
}
RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
```

(Adjust the initializer order to match the exact `rp_core_abi` field order from Task 1.)

- [ ] **Step 3: Write the core's CMake + emit the package**

`cores/refcore_driven/CMakeLists.txt`: `add_library(refcore_driven SHARED RefCoreDriven.cpp)`, `target_include_directories(refcore_driven PRIVATE ${CMAKE_SOURCE_DIR}/include)` (NO Vulkan/D3D11 link — it's pure CPU), and a POST_BUILD copy of the dll + `core.json` into `$<TARGET_FILE_DIR:refcore_driven>/cores/refcore_driven/` (mirror the other cores). Top-level `CMakeLists.txt`: `add_subdirectory(cores/refcore_driven)`.

- [ ] **Step 4: Build — confirm dll + manifest emitted**

Run: `cmake -S . -B build && cmake --build build --config Debug`
Expected: `refcore_driven.dll` + `core.json` under `build/.../cores/refcore_driven/`. Suite still green.

- [ ] **Step 5: Commit**

```bash
git add cores/refcore_driven/ CMakeLists.txt
git commit -m "feat: reference driven core (CPU RGBA8 animated framebuffer)"
```

---

## Task 6: Runtime driven dispatch + driven end-to-end

**Files:**
- Modify: `src/loader/CoreLoader.h/.cpp`, `src/runtime/Runtime.h/.cpp`
- Create: `tests/test_driven_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `composite_driven` (both backends), `refcore_driven` package.
- Produces:
  - `CoreLoader`: `rp_result run_frame(std::string& err);` (calls `abi_->run_frame(core_)` when state is `Created` or `Started`) and `rp_result get_av_info(rp_av_info* out, std::string& err);`.
  - `Runtime`: tracks `rp_core_type core_type_`; installs `video_refresh`/`audio_sample` trampolines; `load_core` skips the api-match for driven cores and calls `get_av_info`; `present()` dispatches driven (`run_frame` → `composite_driven`) vs presenting.

- [ ] **Step 1: Write the failing e2e test**

`tests/test_driven_e2e.cpp` — a `RP_DRIVEN_CORE_DIR` compile-def points at the emitted package. For BOTH backends (Vulkan probe-guarded):
```cpp
#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/vulkan/VulkanBackend.h"
#include <vector>
#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif
static void run_driven(rp_graphics_api api) {
    const uint32_t W=64,H=64;
    rp_runtime* rt = rp_runtime_create(api, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);   // driven core loads on ANY api
    std::vector<uint8_t> img((size_t)W*H*4, 0);
    bool sawGreen=false;
    for (int i=0;i<10 && !sawGreen;i++) {
        if (rp_runtime_present(rt, img.data()) != RP_OK) continue;
        if (img[(((size_t)(H-4))*W + (W-4))*4 + 1] > 150) sawGreen = true;
    }
    CHECK(sawGreen);
    auto at=[&](uint32_t x,uint32_t y,int c){ return img[((size_t)y*W+x)*4+c]; };
    CHECK(at(4,4,2) > 80); CHECK(at(4,4,1) < at(W-4,H-4,1));   // overlay blends
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
TEST_CASE("driven e2e: D3D11") { run_driven(RP_GFX_D3D11); }
TEST_CASE("driven e2e: Vulkan") {
    if (!rp::VulkanBackend::probe_vulkan_shared()) { WARN("no vulkan"); return; }
    run_driven(RP_GFX_VULKAN);
}
TEST_CASE("driven e2e: reload + not-bricked (D3D11)") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr); REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt,64,64)==RP_OK);
    CHECK(rp_runtime_load_core(rt,"no_such_dir")==RP_ERR_NOT_FOUND);
    CHECK(rp_runtime_load_core(rt,RP_DRIVEN_CORE_DIR)==RP_OK);       // recovers
    CHECK(rp_runtime_load_core(rt,RP_DRIVEN_CORE_DIR)==RP_OK);       // reload while loaded
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
```

- [ ] **Step 2: Add the loader passthroughs**

`src/loader/CoreLoader.h/.cpp`:
```cpp
rp_result run_frame(std::string& error);
rp_result get_av_info(rp_av_info* out, std::string& error);
```
Impl (guard state is `Created` or `Started`; call the abi fn; error if the fn ptr is null):
```cpp
rp_result CoreLoader::run_frame(std::string& error) {
    if (state_ != LoaderState::Created && state_ != LoaderState::Started) { error="run_frame needs Created"; return RP_ERR_INTERNAL; }
    if (!abi_->run_frame) { error="core has no run_frame"; return RP_ERR_UNSUPPORTED; }
    abi_->run_frame(core_); return RP_OK;
}
rp_result CoreLoader::get_av_info(rp_av_info* out, std::string& error) {
    if (state_ != LoaderState::Created && state_ != LoaderState::Started) { error="get_av_info needs Created"; return RP_ERR_INTERNAL; }
    if (!abi_->get_av_info) { error="core has no get_av_info"; return RP_ERR_UNSUPPORTED; }
    abi_->get_av_info(core_, out); return RP_OK;
}
```

- [ ] **Step 3: Runtime — trampolines, dispatch, load branch**

`src/runtime/Runtime.h`: add `rp_core_type core_type_ = RP_CORE_PRESENTING;` and driven-frame capture members: `const void* dr_data_=nullptr; uint32_t dr_w_=0, dr_h_=0, dr_pitch_=0; bool dr_dupe_=false, dr_have_=false;`. Declare `void on_video_refresh(const void*, uint32_t,uint32_t,uint32_t);`.
`src/runtime/Runtime.cpp`:
- Trampolines: `static void host_video_refresh(rp_host* h, const void* d, uint32_t w, uint32_t hh, uint32_t p){ reinterpret_cast<Runtime*>(h)->on_video_refresh(d,w,hh,p);}` and a no-op `host_audio_sample`. Install both in the constructor's `host_iface_` (`.video_refresh = host_video_refresh; .audio_sample = host_audio_sample;`).
- `on_video_refresh(d,w,h,p){ dr_have_=true; dr_dupe_ = (d==nullptr); dr_data_=d; dr_w_=w; dr_h_=h; dr_pitch_=p; }`
- `load_core`: after parsing the manifest, set `core_type_ = m.type;`. Change the api-match check to `if (m.type == RP_CORE_PRESENTING && m.graphics_api != api_) return RP_ERR_UNSUPPORTED;` (driven skips it). After `loader_.create(...)`: if `core_type_ == RP_CORE_DRIVEN`, call `loader_.get_av_info(&av, err)` and validate `av.base_width>0 && av.base_height>0` (else `RP_ERR_UNSUPPORTED`) and `av.pixel_format == RP_FMT_R8G8B8A8_UNORM` (else `RP_ERR_UNSUPPORTED`); do NOT call `rebuild_surfaces`/`start` (those are presenting-only). Presenting path unchanged.
- `present`:
```cpp
rp_result Runtime::present(uint8_t* out_rgba) {
    if (!backend_) return RP_ERR_DEVICE;
    std::string err;
    if (core_loaded_ && core_type_ == RP_CORE_DRIVEN) {
        dr_have_ = false;
        rp_result r = loader_.run_frame(err);
        if (r != RP_OK) return r;
        bool dupe = !dr_have_ || dr_dupe_;
        return backend_->composite_driven(dr_dupe_ ? nullptr : dr_data_,
                                          dr_w_, dr_h_, dr_pitch_, dupe, out_rgba, err);
    }
    uint32_t idx=0; uint64_t sv=0;
    bool has = ring_.latest_ready(idx, sv);
    return backend_->composite_and_present(idx, sv, has, out_rgba, err);
}
```
- `unload_core`: reset `core_type_ = RP_CORE_PRESENTING;` and clear the dr_* capture.

- [ ] **Step 4: Wire CMake**

`tests/CMakeLists.txt`: `add_dependencies(retropark_tests refcore_driven)` and `target_compile_definitions(retropark_tests PRIVATE RP_DRIVEN_CORE_DIR="$<TARGET_FILE_DIR:refcore_driven>/cores/refcore_driven")`. Add `test_driven_e2e.cpp`.

- [ ] **Step 5: Build and run — verify green (or SKIP for Vulkan)**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: driven e2e passes on D3D11 and Vulkan (probe-guarded) — the driven core, pulled one `run_frame` per `present`, renders green and the host composites the blended overlay; reload + not-bricked pass. The entire A+B presenting suite still green. **Report Vulkan RAN vs SKIPPED + validation output.**

- [ ] **Step 6: Commit**

```bash
git add src/loader/CoreLoader.* src/runtime/Runtime.* tests/test_driven_e2e.cpp tests/CMakeLists.txt
git commit -m "feat: runtime driven dispatch (run_frame per present) + driven e2e"
```

---

## Task 7: Harness runs the driven core

**Files:**
- Modify: `harness/windowed/main.cpp`, `harness/windowed/CMakeLists.txt`, top-level `CMakeLists.txt`

**Interfaces:**
- Produces: the harness accepts `--driven` (in addition to `--api d3d11|vulkan`); when present it loads `refcore_driven` (which runs under either backend). Default stays the presenting core for the chosen api.

- [ ] **Step 1: Parameterize the harness**

`harness/windowed/main.cpp`: parse a `--driven` flag. When set, load the core dir `RP_HARNESS_DRIVEN_CORE_DIR` (regardless of `--api`); otherwise keep the existing behavior (presenting core matching `--api`). The window/present loop/cleanup are unchanged — `present()` drives the driven core automatically.

- [ ] **Step 2: Wire CMake**

`harness/windowed/CMakeLists.txt`: `add_dependencies(retropark_harness refcore_driven)` and `target_compile_definitions(retropark_harness PRIVATE RP_HARNESS_DRIVEN_CORE_DIR="$<TARGET_FILE_DIR:refcore_driven>/cores/refcore_driven")`.

- [ ] **Step 3: Build + smoke both backends with the driven core**

Run: `cmake -S . -B build && cmake --build build --config Debug`. Launch `retropark_harness.exe --api d3d11 --driven` (background ~3s), `PrintWindow`-capture to `C:\Users\cubma\source\repos\RetroPark\.superpowers\sdd\harness-driven-shot.png`, terminate. Confirm the window shows the animated green→blue driven core with the blue-tinted overlay. Repeat with `--api vulkan --driven` (capture to `harness-driven-vk-shot.png`) — the SAME driven core binary under the Vulkan backend. If black, diagnose (upload path, compositor target) — do not commit a black window.

- [ ] **Step 4: Commit**

```bash
git add harness/windowed/ CMakeLists.txt
git commit -m "feat: harness --driven runs the reference driven core on either backend"
```

---

## Self-Review

**Spec coverage:**
- §1.1 rp_av_info + get_av_info/run_frame/serialize* → Task 1. ✓
- §1.2 video_refresh/audio_sample host callbacks → Task 1 (declared) + Task 6 (video_refresh trampoline wired; audio_sample no-op). ✓
- §1.3 RP_GFX_NONE + api-match presenting-only + ABI v3 → Task 1 (enum, manifest, version) + Task 6 (runtime skips api-match for driven). ✓
- §2 composite_driven on both backends → Task 2 (interface) + Task 3 (D3D11) + Task 4 (Vulkan). ✓
- §3 data flow (run_frame per present → composite_driven; pointer lifetime) → Task 6. ✓
- §4 error handling: missing run_frame/get_av_info, zero geometry, non-RGBA8 → Task 6 (load validation) + Task 2/3/4 (backend); video_refresh(NULL)=dupe → Task 6 (`dr_dupe_`) + Tasks 3/4 (dupe reuses last texture); pitch → Task 2 helper + Tasks 3/4 tests. ✓
- §5 testing: fbcopy pitch (Task 2), backend upload both (Tasks 3/4), reference core (Task 5), driven e2e both + reload/not-bricked (Task 6), regression A+B green (every task). The dupe case: covered by the `dr_dupe_` path; note the e2e doesn't force a NULL video_refresh (the reference core always refreshes) — the dupe path is exercised by unit reasoning + the "no run_frame call" fallback, not a dedicated e2e assertion. **Gap addressed:** add a dupe assertion is optional; the spec lists it under testing — see note below.
- §6 scope: all in-scope built; nothing out-of-scope. ✓

**Deviation/gap noted honestly:** the spec's §5 mentions "a `video_refresh(NULL)` dupe case asserts the last frame persists." The reference driven core always calls `video_refresh` with real data, so the e2e can't trivially force a NULL. The dupe path IS implemented (Task 6 `dupe = !dr_have_ || dr_dupe_`, Tasks 3/4 reuse the last texture). To actually assert it, Task 6's e2e should add: after a normal frame, call a second `present()` in a way that yields dupe — simplest is a tiny dedicated test using a mock driven core whose `run_frame` calls `video_refresh(NULL)`. **Task 6 Step 1 should include** a `dupe` case via such a mock (a 2nd tiny driven core, or extend the reference core with an env/flag). Implementer: add a minimal dupe assertion (mock driven core that refreshes real data on frame 1, NULL on frame 2; assert frame-2 readback still shows the frame-1 green). If a mock is too heavy, document that dupe is covered by the backend-level `dupe=true` path in Tasks 3/4 (add a `dupe=true` assertion to the Task 3/4 upload tests: upload green, then call `composite_driven(nullptr,…,dupe=true)`, assert green persists). **Prefer the latter** — add a `dupe=true` reuse assertion to the Task 3 and Task 4 upload tests; it's the cleanest place to prove dupe.

**Placeholder scan:** the two backend tasks (3, 4) describe the upload path in prose with the exact texture usage/format/layout and the `copy_rgba8_rows` call; all logic-bearing values (format, pitch handling, dupe semantics) are explicit. No TBD/TODO. Non-backend code is complete.

**Type consistency:** `composite_driven(const void*, uint32_t, uint32_t, uint32_t, bool, uint8_t*, std::string&)`, `copy_rgba8_rows(...)`, `rp_av_info` fields, `CoreLoader::run_frame`/`get_av_info`, and the `rp_core_abi`/`rp_host_iface` field additions are used identically across Tasks 1–6. The `rp_core_abi` initializer order (get_info, create, destroy, set_surfaces, start, stop, get_av_info, run_frame, serialize_size, serialize, unserialize) is stated in Task 1 and matched by the reference core in Task 5.
```
