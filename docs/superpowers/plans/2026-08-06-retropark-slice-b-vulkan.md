# RetroPark Slice B (Vulkan Backend) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Vulkan implementation of `IRenderBackend` (plus a Vulkan reference core) that runs Slice A's presenting-core + blended-overlay pipeline entirely in Vulkan on Windows, proving the render abstraction carries a second, unrelated GPU API.

**Architecture:** A `VulkanBackend` mirrors `D3D11Backend`: it picks a `VkPhysicalDevice`, allocates a ring of **exported shared `VkImage`s** (`VK_KHR_external_memory_win32`) plus one **exported shared timeline semaphore** (`VK_KHR_external_semaphore_win32` + `VK_KHR_timeline_semaphore`), and composites a blended overlay. A `refcore_present_vk` core imports those on its own `VkDevice` (same physical device, matched by UUID), renders an animated frame, and drives a single-shared-timeline producer/consumer handshake (even value `2f` = core rendered frame `f`; odd `2f+1` = host consumed it). A `BackendFactory` selects D3D11 vs Vulkan by `rp_graphics_api`.

**Tech Stack:** C++17, CMake, Vulkan 1.3 (SDK 1.4.357.0 at `C:\VulkanSDK`), `glslc` (build-time GLSL→SPIR-V), `VK_LAYER_KHRONOS_validation`, doctest. GPU tests run on the real GPU (RTX 5080), probe-guarded.

## Global Constraints

- **C++17. No Qt/EverythingBox.** MSVC `/W4 /permissive-`, warning-clean for `retropark` sources.
- **Vulkan SDK** is located by CMake via `find_package(Vulkan REQUIRED COMPONENTS glslc)` (env `VULKAN_SDK=C:\VulkanSDK\1.4.357.0`). Link `Vulkan::Vulkan`. Shaders are compiled to SPIR-V at build time and embedded as `uint32_t[]` — **no runtime shader compilation, no runtime file reads**.
- **Same-physical-device rule:** host and core must use the `VkPhysicalDevice` whose `VkPhysicalDeviceIDProperties.deviceUUID` matches the 16-byte `rp_surface_set.device_uuid`. External-memory import across different physical devices is invalid.
- **Timeline handshake (Vulkan):** one shared timeline semaphore `T`, init 0. Core frame `f` (1-based), slot `i=f%N`: render submit **waits** `T >= 2*(f-N)+1` (skip if `f<=N`) and **signals** `T=2f`; then `submit_frame(i, generation, 2f)`. Host composite submit **waits** `T >= 2f` and **signals** `T=2f+1`. All CPU-side fence waits use a **finite timeout** (1e9 ns = 1 s), never infinite.
- **Handle ownership:** every exported Win32 `HANDLE` (image memory, semaphore) is owned by an RAII holder that `CloseHandle`s exactly once (mirroring Slice A's move-only `Surface`). Vulkan objects are destroyed in reverse creation order in destructors.
- **Validation-clean:** Debug builds enable `VK_LAYER_KHRONOS_validation`; test/harness runs must produce **zero validation errors** (treated like a warning).
- **Probe-guarded GPU tests:** every test that needs a real Vulkan device begins with `if (!probe_vulkan_shared()) { WARN("no capable Vulkan device"); return; }` — skip, don't fail, on machines without support.
- **Slice A must stay green:** the ABI/interface changes are additive; the entire existing D3D11 suite must still pass after Task 1.
- **Commits:** conventional prefixes. **No AI attribution** of any kind anywhere.
- **Interface honesty:** `IRenderBackend` gains one parameter on `composite_and_present` (`sync_value`) and two **defaulted** accessors (`present_sync_handle`, `present_device_uuid`); `initialize`/`allocate_surfaces` signatures are unchanged and `D3D11Backend` needs no behavioral change beyond the new `composite_and_present` parameter it ignores.

---

## File Structure

```
RetroPark/
  cmake/Shaders.cmake                        # compile_shader(): glslc → .spv → embedded header  (Task 2)
  include/retropark/retropark_abi.h          # + rp_surface_set, submit_frame sync_value          (Task 1)
  src/render/
    IRenderBackend.h                         # + sync_value param, 2 defaulted accessors           (Task 1)
    SurfaceRing.h/.cpp                        # accept_submit/latest_ready carry sync_value          (Task 1)
    d3d11/D3D11Backend.h/.cpp                 # composite_and_present signature update (ignores it)  (Task 1)
    vulkan/
      VulkanCommon.h                         # VK_CHECK, RAII holders, feature structs              (Task 3)
      VulkanBackend.h/.cpp                    # IRenderBackend impl                                  (Tasks 3-5)
      VulkanCompositor.h/.cpp                 # pipeline + blended overlay                           (Task 5)
      shaders/fullscreen.vert, sample.frag, overlay.vert, overlay.frag                              (Task 5)
      VulkanShaders_generated.h              # embedded SPIR-V (build output, git-ignored)          (Task 5)
  src/runtime/
    BackendFactory.h/.cpp                     # api → IRenderBackend                                 (Task 6)
    Runtime.h/.cpp                            # factory + rp_surface_set + sync_value plumbing        (Tasks 1,6)
  src/loader/CoreLoader.h/.cpp                # set_surfaces(rp_surface_set*)                        (Task 1)
  cores/
    refcore_present/RefCore.cpp               # submit_frame sync_value=0, set_surfaces(set)          (Task 1)
    refcore_present_vk/RefCoreVk.cpp, core.json, CMakeLists.txt                                      (Task 7)
  harness/windowed/main.cpp                   # --api selects d3d11|vulkan                            (Task 9)
  tests/
    mock_core/MockCore.cpp                    # updated ABI                                          (Task 1)
    test_loader.cpp, test_surface_ring.cpp    # updated ABI                                          (Task 1)
    test_shader_embed.cpp                     # (Task 2)
    test_backend_factory.cpp                  # (Task 6)
    test_vulkan_handoff.cpp                   # (Task 4)
    test_vulkan_compositor.cpp                # (Task 5)
    test_vulkan_e2e.cpp                       # (Task 8)
```

---

## Task 1: ABI + interface evolution for sync (D3D11 path stays green)

**Files:**
- Modify: `include/retropark/retropark_abi.h`, `src/render/IRenderBackend.h`, `src/render/SurfaceRing.h/.cpp`, `src/render/d3d11/D3D11Backend.h/.cpp`, `src/loader/CoreLoader.h/.cpp`, `src/runtime/Runtime.h/.cpp`, `cores/refcore_present/RefCore.cpp`, `tests/mock_core/MockCore.cpp`, `tests/test_loader.cpp`, `tests/test_surface_ring.cpp`
- Test: `tests/test_surface_ring.cpp` (sync_value round-trip), plus the full existing suite

**Interfaces:**
- Consumes: all existing Slice A types.
- Produces:
  - `struct rp_surface_set { uint32_t count; uint32_t reserved; const rp_surface_desc* surfaces; void* sync_handle; uint8_t device_uuid[16]; };`
  - `rp_host_iface.submit_frame` → `void (*)(rp_host*, uint32_t index, uint64_t generation, uint64_t sync_value)`
  - `rp_core_abi.set_surfaces` → `rp_result (*)(rp_core*, const rp_surface_set*)`
  - `SurfaceRing::accept_submit(uint32_t index, uint64_t generation, uint64_t sync_value)`; `SurfaceRing::latest_ready(uint32_t& index_out, uint64_t& sync_value_out)`
  - `IRenderBackend::composite_and_present(uint32_t ready_index, uint64_t sync_value, bool has_frame, uint8_t* out_rgba, std::string& err)`
  - `IRenderBackend::present_sync_handle() const` (default `nullptr`), `IRenderBackend::present_device_uuid(uint8_t out[16]) const` (default zero-fill)

- [ ] **Step 1: Update the ABI header**

In `include/retropark/retropark_abi.h`, add after `rp_surface_desc`:
```c
typedef struct rp_surface_set {
    uint32_t               count;
    uint32_t               reserved;
    const rp_surface_desc* surfaces;
    void*                  sync_handle;      /* shared timeline semaphore NT handle (Vulkan); NULL for D3D11 */
    uint8_t                device_uuid[16];  /* target VkPhysicalDevice UUID (Vulkan); all-zero for D3D11 */
} rp_surface_set;
```
Change `rp_host_iface.submit_frame` to:
```c
void (*submit_frame)(rp_host* host, uint32_t index, uint64_t generation, uint64_t sync_value);
```
Change `rp_core_abi.set_surfaces` to:
```c
rp_result (*set_surfaces)(rp_core* core, const rp_surface_set* set);
```

- [ ] **Step 2: Update `SurfaceRing` to carry sync_value (write failing test first)**

Add to `tests/test_surface_ring.cpp`:
```cpp
TEST_CASE("ring: sync_value round-trips with the ready frame") {
    SurfaceRing r(3);
    uint64_t g = r.reallocate(16,16);
    uint32_t idx = r.next_producer_slot();
    CHECK(r.accept_submit(idx, g, /*sync_value=*/42));
    uint32_t out=99; uint64_t sv=0;
    CHECK(r.latest_ready(out, sv));
    CHECK(out == idx);
    CHECK(sv == 42);
}
```
Update every existing `accept_submit(a,b)` call in that file to `accept_submit(a,b,0)` and every `latest_ready(x)` to `latest_ready(x, sv)` with a local `uint64_t sv`.

- [ ] **Step 3: Run — expect compile failure**

Run: `cmake --build build --config Debug`
Expected: FAIL — `accept_submit`/`latest_ready` arity mismatch.

- [ ] **Step 4: Implement the `SurfaceRing` change**

`src/render/SurfaceRing.h`: add `std::atomic<uint64_t> ready_sync_{0};`. Change signatures:
```cpp
bool accept_submit(uint32_t index, uint64_t generation, uint64_t sync_value);
bool latest_ready(uint32_t& index_out, uint64_t& sync_value_out) const;
```
`src/render/SurfaceRing.cpp`:
```cpp
bool SurfaceRing::accept_submit(uint32_t index, uint64_t generation, uint64_t sync_value) {
    if (generation != generation_) return false;
    if (index >= slot_count_) return false;
    ready_index_.store(index, std::memory_order_relaxed);
    ready_generation_.store(generation, std::memory_order_relaxed);
    ready_sync_.store(sync_value, std::memory_order_relaxed);
    has_ready_.store(true, std::memory_order_release);
    return true;
}
bool SurfaceRing::latest_ready(uint32_t& index_out, uint64_t& sync_value_out) const {
    if (!has_ready_.load(std::memory_order_acquire)) return false;
    if (ready_generation_.load(std::memory_order_relaxed) != generation_) return false;
    index_out = ready_index_.load(std::memory_order_relaxed);
    sync_value_out = ready_sync_.load(std::memory_order_relaxed);
    return true;
}
```

- [ ] **Step 5: Update `IRenderBackend`**

`src/render/IRenderBackend.h`:
```cpp
virtual rp_result composite_and_present(uint32_t ready_index, uint64_t sync_value, bool has_frame,
                                        uint8_t* out_rgba, std::string& err) = 0;
virtual void* present_sync_handle() const { return nullptr; }
virtual void present_device_uuid(uint8_t out[16]) const { for (int i=0;i<16;++i) out[i]=0; }
```

- [ ] **Step 6: Update `D3D11Backend`**

In `src/render/d3d11/D3D11Backend.h/.cpp`, change `composite_and_present` to the new signature (add `uint64_t sync_value` as the 2nd param) and **ignore** `sync_value` (add `(void)sync_value;`). No other behavioral change. Keep the windowed+readback guard.

- [ ] **Step 7: Update `CoreLoader`**

`src/loader/CoreLoader.h/.cpp`: change `set_surfaces` to:
```cpp
rp_result set_surfaces(const rp_surface_set* set, std::string& error);
```
Impl forwards to `abi_->set_surfaces(core_, set)` (guard state == Created as before).

- [ ] **Step 8: Update `Runtime`**

`src/runtime/Runtime.cpp`:
- Host trampoline: `static void host_submit(rp_host* h, uint32_t i, uint64_t g, uint64_t sv) { reinterpret_cast<Runtime*>(h)->on_submit(i,g,sv); }` and `on_submit(index,gen,sv){ ring_.accept_submit(index,gen,sv); }`.
- `rebuild_surfaces`: after allocating descs and stamping generation, build and pass a set:
```cpp
rp_surface_set set{};
set.count = (uint32_t)descs.size();
set.surfaces = descs.data();
set.sync_handle = backend_->present_sync_handle();
backend_->present_device_uuid(set.device_uuid);
return loader_.set_surfaces(&set, err);
```
- `present`:
```cpp
uint32_t idx = 0; uint64_t sv = 0;
bool has = ring_.latest_ready(idx, sv);
return backend_->composite_and_present(idx, sv, has, out_rgba, err);
```

- [ ] **Step 9: Update `RefCore.cpp`, `MockCore.cpp`, and the fake in `test_loader.cpp`**

- `cores/refcore_present/RefCore.cpp`: `ref_set_surfaces(rp_core*, const rp_surface_set* set)` — iterate `set->surfaces[0..set->count-1]` (was descs/count); ignore `set->sync_handle`/`device_uuid`. In the render loop, call `host.submit_frame(host.host, s.index, s.generation, /*sync_value=*/0)`.
- `tests/mock_core/MockCore.cpp`: `mock_set_surfaces(rp_core* c, const rp_surface_set* set){ ((MockCore*)c)->surfaces = set->count; return RP_OK; }`.
- `tests/test_loader.cpp`: update the fake `fake_set_surfaces` to the `const rp_surface_set*` signature (read `set->count`); update the happy-path test to build an `rp_surface_set` with a 2-element desc array and pass `&set`.

- [ ] **Step 10: Build and run the full suite — everything green**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: all existing tests pass plus the new sync_value ring test. The D3D11 e2e still renders the core frame + blend. Warning-clean.

- [ ] **Step 11: Commit**

```bash
git add include/retropark/retropark_abi.h src/render/ src/loader/ src/runtime/ cores/refcore_present/ tests/
git commit -m "refactor: evolve ABI/backend interface for external sync (surface_set + sync_value)"
```

---

## Task 2: CMake Vulkan integration + build-time shader embedding

**Files:**
- Create: `cmake/Shaders.cmake`, `tests/test_shader_embed.cpp`, `tests/shaders/probe.vert`
- Modify: top-level `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: a CMake function `retropark_embed_shader(<glsl_path> <stage> <out_header> <symbol>)` that runs `glslc` and emits a header defining `static const uint32_t <symbol>[]` + `static const size_t <symbol>_len`. Vulkan is available as the `Vulkan::Vulkan` link target.

- [ ] **Step 1: Write `cmake/Shaders.cmake`**

```cmake
find_package(Vulkan REQUIRED COMPONENTS glslc)

# retropark_embed_shader(<glsl> <stage vert|frag> <out_header_abs> <symbol>)
function(retropark_embed_shader GLSL STAGE OUT_HEADER SYMBOL)
  set(SPV "${OUT_HEADER}.spv")
  add_custom_command(
    OUTPUT "${OUT_HEADER}"
    COMMAND Vulkan::glslc -fshader-stage=${STAGE} "${GLSL}" -o "${SPV}"
    COMMAND ${CMAKE_COMMAND}
            -DSPV="${SPV}" -DHEADER="${OUT_HEADER}" -DSYMBOL="${SYMBOL}"
            -P "${CMAKE_SOURCE_DIR}/cmake/EmbedSpv.cmake"
    DEPENDS "${GLSL}" "${CMAKE_SOURCE_DIR}/cmake/EmbedSpv.cmake"
    VERBATIM)
endfunction()
```

Create `cmake/EmbedSpv.cmake` (turns a `.spv` into a C header of `uint32_t`):
```cmake
file(READ "${SPV}" HEXDATA HEX)
string(LENGTH "${HEXDATA}" HEXLEN)
math(EXPR NWORDS "${HEXLEN} / 8")
set(BODY "static const unsigned int ${SYMBOL}[] = {\n")
set(i 0)
while(i LESS NWORDS)
  math(EXPR OFF "${i} * 8")
  string(SUBSTRING "${HEXDATA}" ${OFF} 8 W)
  # SPIR-V is little-endian words; swap byte order b0b1b2b3 -> b3b2b1b0
  string(SUBSTRING "${W}" 0 2 B0)
  string(SUBSTRING "${W}" 2 2 B1)
  string(SUBSTRING "${W}" 4 2 B2)
  string(SUBSTRING "${W}" 6 2 B3)
  set(BODY "${BODY}0x${B3}${B2}${B1}${B0}u,\n")
  math(EXPR i "${i} + 1")
endwhile()
set(BODY "${BODY}};\nstatic const unsigned long ${SYMBOL}_len = ${NWORDS};\n")
file(WRITE "${HEADER}" "${BODY}")
```

- [ ] **Step 2: Write the failing test**

`tests/shaders/probe.vert`:
```glsl
#version 450
void main() { gl_Position = vec4(0.0); }
```
`tests/test_shader_embed.cpp`:
```cpp
#include <doctest/doctest.h>
#include "probe_vert_generated.h"   // generated by retropark_embed_shader

TEST_CASE("shader embed: probe.vert compiles to valid SPIR-V") {
    REQUIRE(probe_vert_len > 4);
    CHECK(probe_vert[0] == 0x07230203u);   // SPIR-V magic number
}
```

- [ ] **Step 3: Wire CMake**

Top-level `CMakeLists.txt`: `include(cmake/Shaders.cmake)` and link Vulkan into `retropark`:
```cmake
target_link_libraries(retropark PUBLIC Vulkan::Vulkan)
```
`tests/CMakeLists.txt`:
```cmake
set(PROBE_HDR "${CMAKE_CURRENT_BINARY_DIR}/gen/probe_vert_generated.h")
retropark_embed_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/probe.vert" vert "${PROBE_HDR}" probe_vert)
add_custom_target(gen_probe_shader DEPENDS "${PROBE_HDR}")
add_dependencies(retropark_tests gen_probe_shader)
target_include_directories(retropark_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")
# add test_shader_embed.cpp to the sources
```

- [ ] **Step 4: Build and run — verify green**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug -R retropark_tests --output-on-failure`
Expected: the generated header exists, the SPIR-V magic assertion passes.

- [ ] **Step 5: Commit**

```bash
git add cmake/ tests/shaders/ tests/test_shader_embed.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "build: Vulkan SDK integration and build-time SPIR-V shader embedding"
```

---

## Task 3: VulkanBackend skeleton — instance, physical device, probe

**Files:**
- Create: `src/render/vulkan/VulkanCommon.h`, `src/render/vulkan/VulkanBackend.h`, `src/render/vulkan/VulkanBackend.cpp`, `tests/test_vulkan_device.cpp`
- Modify: top-level `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `IRenderBackend`.
- Produces:
  - `class rp::VulkanBackend : public IRenderBackend` — `initialize` (creates instance+device, picks physical device, exposes UUID), `present_device_uuid` override; `allocate_surfaces`/`composite_and_present` return `RP_ERR_UNSUPPORTED` stubs for now.
  - `static bool VulkanBackend::probe_vulkan_shared()` — true iff a physical device supports timeline semaphores + external memory/semaphore opaque-Win32 handles.
  - `VulkanCommon.h`: `#define VK_CHECK(x)` (returns `RP_ERR_DEVICE` on non-`VK_SUCCESS`), the required extension/feature lists.

- [ ] **Step 1: Write `VulkanCommon.h`**

```cpp
#pragma once
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <string>
#include <cstring>
#include <retropark/retropark_abi.h>

namespace rp {
#define VK_CHECK(expr, err, msg) do { if ((expr) != VK_SUCCESS) { (err) = (msg); return RP_ERR_DEVICE; } } while(0)

// Device extensions required for the presenting handoff.
inline const char* const kRequiredDeviceExts[] = {
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
};
}
```

- [ ] **Step 2: Write the failing test**

`tests/test_vulkan_device.cpp`:
```cpp
#include <doctest/doctest.h>
#include "render/vulkan/VulkanBackend.h"
using namespace rp;

TEST_CASE("vulkan: device initializes and exposes a non-zero UUID") {
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    VulkanBackend b; std::string err;
    REQUIRE(b.initialize(nullptr, 64, 64, err) == RP_OK);
    uint8_t uuid[16]; b.present_device_uuid(uuid);
    uint8_t zero[16] = {0};
    CHECK(std::memcmp(uuid, zero, 16) != 0);
}
```

- [ ] **Step 3: Write `VulkanBackend.h`**

```cpp
#pragma once
#include "render/IRenderBackend.h"
#include "render/vulkan/VulkanCommon.h"
#include <vector>

namespace rp {
class VulkanBackend : public IRenderBackend {
public:
    ~VulkanBackend() override;
    rp_result initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) override;
    rp_result allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                std::vector<rp_surface_desc>& out, std::string& err) override;
    rp_result composite_and_present(uint32_t ready_index, uint64_t sync_value, bool has_frame,
                                    uint8_t* out_rgba, std::string& err) override;
    void  present_device_uuid(uint8_t out[16]) const override { std::memcpy(out, device_uuid_, 16); }
    void* present_sync_handle() const override { return sync_handle_; }

    static bool probe_vulkan_shared();

protected:
    rp_result create_instance_and_device(std::string& err);
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint8_t device_uuid_[16] = {0};
    void* sync_handle_ = nullptr;    // set in Task 4
    uint32_t width_ = 0, height_ = 0;
};
}
```

- [ ] **Step 4: Write `VulkanBackend.cpp` (instance/device/probe; alloc+composite stubbed)**

```cpp
#include "render/vulkan/VulkanBackend.h"
#include <vector>

namespace rp {

static bool pick_physical(VkInstance inst, VkPhysicalDevice& out, uint8_t uuid[16], uint32_t& qfam) {
    uint32_t n=0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(inst, &n, devs.data());
    VkPhysicalDevice best = VK_NULL_HANDLE; bool bestDiscrete=false;
    for (auto d : devs) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
        // require a graphics queue
        uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn); vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,qs.data());
        int gfx=-1; for (uint32_t i=0;i<qn;++i) if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){ gfx=(int)i; break; }
        if (gfx<0) continue;
        bool discrete = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (best==VK_NULL_HANDLE || (discrete && !bestDiscrete)) {
            best=d; bestDiscrete=discrete; qfam=(uint32_t)gfx;
        }
    }
    if (best==VK_NULL_HANDLE) return false;
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext=&idp;
    vkGetPhysicalDeviceProperties2(best, &p2);
    std::memcpy(uuid, idp.deviceUUID, 16);
    out = best; return true;
}

rp_result VulkanBackend::create_instance_and_device(std::string& err) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    const char* instExts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo=&app;
    ici.enabledExtensionCount=3; ici.ppEnabledExtensionNames=instExts;
#ifndef NDEBUG
    ici.enabledLayerCount=1; ici.ppEnabledLayerNames=layers;
#endif
    VK_CHECK(vkCreateInstance(&ici,nullptr,&instance_), err, "vkCreateInstance");

    if (!pick_physical(instance_, phys_, device_uuid_, queue_family_)) { err="no gfx device"; return RP_ERR_UNSUPPORTED; }

    // Enable timeline semaphore feature.
    VkPhysicalDeviceTimelineSemaphoreFeatures tsf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    tsf.timelineSemaphore = VK_TRUE;
    float pri=1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=queue_family_; qci.queueCount=1; qci.pQueuePriorities=&pri;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext=&tsf;
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    dci.enabledExtensionCount=(uint32_t)(sizeof(kRequiredDeviceExts)/sizeof(char*));
    dci.ppEnabledExtensionNames=kRequiredDeviceExts;
    VK_CHECK(vkCreateDevice(phys_,&dci,nullptr,&device_), err, "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return RP_OK;
}

rp_result VulkanBackend::initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) {
    width_=w; height_=h; (void)native_window;
    return create_instance_and_device(err);
}

bool VulkanBackend::probe_vulkan_shared() {
    VulkanBackend b; std::string e;
    bool ok = (b.create_instance_and_device(e) == RP_OK);
    return ok;   // destructor tears down
}

rp_result VulkanBackend::allocate_surfaces(uint32_t, uint32_t, uint32_t,
                                           std::vector<rp_surface_desc>&, std::string& err) {
    err="allocate_surfaces not implemented until Task 4"; return RP_ERR_UNSUPPORTED;
}
rp_result VulkanBackend::composite_and_present(uint32_t, uint64_t, bool, uint8_t*, std::string& err) {
    err="composite not implemented until Task 5"; return RP_ERR_UNSUPPORTED;
}

VulkanBackend::~VulkanBackend() {
    if (device_) { vkDeviceWaitIdle(device_); vkDestroyDevice(device_, nullptr); }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}
}
```

- [ ] **Step 5: Wire CMake, build, run (probe-guarded)**

Add `src/render/vulkan/VulkanBackend.cpp` to `retropark`; add `test_vulkan_device.cpp` to tests.
Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the device test runs (real GPU present) and passes with a non-zero UUID; validation-clean. **Report explicitly whether it RAN or SKIPPED.**

- [ ] **Step 6: Commit**

```bash
git add src/render/vulkan/ tests/test_vulkan_device.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: VulkanBackend instance/device + physical-device UUID + probe"
```

---

## Task 4: Exported shared image ring + timeline semaphore + cross-device handoff

**Files:**
- Modify: `src/render/vulkan/VulkanBackend.h/.cpp`
- Create: `tests/test_vulkan_handoff.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `VulkanBackend::allocate_surfaces` creates `count` shared `VkImage`s on exported `VkDeviceMemory` (opaque Win32 NT handle in `rp_surface_desc.shared_handle`), plus one exported shared timeline semaphore (`present_sync_handle()`), init value 0. Test helper `rp_result import_and_readback_pixel(...)` not needed — the test imports directly on a second device.

- [ ] **Step 1: Write the failing handoff test**

`tests/test_vulkan_handoff.cpp`: probe-guard; host `VulkanBackend` allocates 1 shared image (8x8) + timeline. A second `VkDevice` on the **same physical device** (matched by the host's UUID) imports the image memory (`VkImportMemoryWin32HandleInfoKHR`, `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT`) and the timeline semaphore (`vkImportSemaphoreWin32HandleKHR`), records a command buffer that transitions the image to `TRANSFER_DST`, clears it red (`vkCmdClearColorImage`), submits with a **timeline signal to value 2** (`VkTimelineSemaphoreSubmitInfo`), and the host waits the timeline `>= 2` (host-side `vkWaitSemaphores`, 1s timeout) then copies the image to a host-visible staging buffer and asserts pixel (0,0) == red. (This mirrors `test_d3d11_handoff`.) Because writing the whole second-device setup is long, the test may call a shared helper `vk_test_producer_clear(host, descs[0], host.present_sync_handle(), uuid, {1,0,0,1}, /*signal=*/2)` implemented in the test file.

Assertion focus:
```cpp
if (!VulkanBackend::probe_vulkan_shared()) { WARN("skip"); return; }
// ... allocate, produce red on 2nd device signalling timeline=2, host waits >=2, readback ...
CHECK(rgba[0]==255); CHECK(rgba[1]==0); CHECK(rgba[2]==0);
```

- [ ] **Step 2: Run — expect fail (allocate stub returns UNSUPPORTED)**

Run: `cmake --build build --config Debug`; the test fails at `REQUIRE(allocate_surfaces==RP_OK)`.

- [ ] **Step 3: Implement `allocate_surfaces` (exported images) + the shared timeline semaphore**

Add members to `VulkanBackend.h`:
```cpp
struct VkSurface {
    VkImage image=VK_NULL_HANDLE; VkDeviceMemory mem=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE; void* handle=nullptr; // NT handle (owned)
};
std::vector<VkSurface> surfaces_;
VkSemaphore timeline_=VK_NULL_HANDLE;   // exported shared timeline
```
Implement in `.cpp` (create each image with `VkExternalMemoryImageCreateInfo{handleTypes=OPAQUE_WIN32}`, allocate memory with `VkExportMemoryAllocateInfo{handleTypes=OPAQUE_WIN32}` + `VkMemoryDedicatedAllocateInfo`, bind, `vkGetMemoryWin32HandleKHR` → `desc.shared_handle`, create an `VkImageView`). Create the timeline once with `VkSemaphoreTypeCreateInfo{TIMELINE, initialValue=0}` + `VkExportSemaphoreCreateInfo{OPAQUE_WIN32}`, then `vkGetSemaphoreWin32HandleKHR` → `sync_handle_`. Load the `vkGet*Win32HandleKHR` / `vkImportSemaphoreWin32HandleKHR` function pointers via `vkGetDeviceProcAddr`. Fill each `rp_surface_desc{index,w,h,RP_FMT_R8G8B8A8_UNORM, shared_handle, generation=0}` (`DXGI`-free; format is `VK_FORMAT_R8G8B8A8_UNORM`). Add reverse-order teardown in the destructor (destroy views/images/free memory/`CloseHandle` each `handle`, destroy `timeline_` and `CloseHandle(sync_handle_)`).

(The full field-by-field Vulkan struct code is written by the implementer with validation layers on; the required struct types and handle-type bits are named above and MUST be used exactly. Every `vkGetMemoryWin32HandleKHR`/`vkGetSemaphoreWin32HandleKHR` result is an NT handle the backend owns and must `CloseHandle` once.)

- [ ] **Step 4: Build and run — verify green (or SKIP)**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug -R retropark_tests --output-on-failure`
Expected: the handoff test reads back red across two devices. **Report RAN vs SKIPPED and any validation output.**

- [ ] **Step 5: Commit**

```bash
git add src/render/vulkan/ tests/test_vulkan_handoff.cpp tests/CMakeLists.txt
git commit -m "feat: Vulkan exported shared image ring + timeline semaphore handoff"
```

---

## Task 5: VulkanCompositor + shaders + headless composite/readback + blend test

**Files:**
- Create: `src/render/vulkan/VulkanCompositor.h/.cpp`, `src/render/vulkan/shaders/{fullscreen.vert,sample.frag,overlay.vert,overlay.frag}`, `tests/test_vulkan_compositor.cpp`
- Modify: `src/render/vulkan/VulkanBackend.h/.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `VulkanBackend::composite_and_present` (headless: composite into an offscreen `VkImage`, copy to staging, fill `out_rgba`; waits the shared timeline `>= sync_value` before sampling the core image, signals `sync_value+1` after). `VulkanCompositor` builds two pipelines (sample core fullscreen; blended overlay quad) against the offscreen render pass.

- [ ] **Step 1: Write the GLSL shaders**

`fullscreen.vert` (SV_VertexID-style fullscreen triangle), `sample.frag` (samples `layout(set=0,binding=0) sampler2D` at the interpolated UV), `overlay.vert` (emits a top-left-quadrant quad from a push-constant rect via `gl_VertexIndex`), `overlay.frag` (outputs a push-constant RGBA). Exact GLSL:
```glsl
// fullscreen.vert
#version 450
layout(location=0) out vec2 uv;
void main(){ uv=vec2((gl_VertexIndex<<1)&2, gl_VertexIndex&2); gl_Position=vec4(uv*2.0-1.0,0,1); }
// sample.frag
#version 450
layout(location=0) in vec2 uv; layout(location=0) out vec4 o;
layout(set=0,binding=0) uniform sampler2D coreTex;
void main(){ o=texture(coreTex, uv); }
// overlay.vert
#version 450
layout(push_constant) uniform P { vec4 rect; vec4 color; } pc;
void main(){ vec2 c[4]=vec2[4](pc.rect.xy,pc.rect.zy,pc.rect.xw,pc.rect.zw); gl_Position=vec4(c[gl_VertexIndex],0,1); }
// overlay.frag
#version 450
layout(push_constant) uniform P { vec4 rect; vec4 color; } pc;
layout(location=0) out vec4 o;
void main(){ o=pc.color; }
```
Blend for the overlay pipeline: `srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA`, `dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA`, `blendEnable=VK_TRUE`. Overlay push-constant: rect NDC top-left quadrant `(-1,-1,0,0)`, color `(0,0,1,0.5)`. Core pipeline: blend disabled.

- [ ] **Step 2: Write the failing blend test**

`tests/test_vulkan_compositor.cpp`: probe-guard; allocate 1 shared image (64x64); a producer on a second device clears it **green** and signals the timeline to `2`; then `host.composite_and_present(0, /*sync_value=*/2, true, img.data(), err)`. Assert pure green outside the overlay quadrant (e.g. px (60,60): G>200, B<60) and a blended pixel inside (px (4,4): B>80 and G < the non-overlay G). Same discrimination as the D3D11 compositor test.

- [ ] **Step 3: Embed the shaders via CMake**

In `CMakeLists.txt`, call `retropark_embed_shader` for each of the four shaders into `${CMAKE_BINARY_DIR}/gen/vk_<name>_generated.h` (symbols `vk_fullscreen_vert`, `vk_sample_frag`, `vk_overlay_vert`, `vk_overlay_frag`), add a `gen_vk_shaders` custom target, make `retropark` depend on it, and add `${CMAKE_BINARY_DIR}/gen` to `retropark`'s include dirs.

- [ ] **Step 4: Implement `VulkanCompositor` and the headless composite path**

`VulkanCompositor`: create shader modules from the embedded SPIR-V, a descriptor set layout (combined image sampler), a sampler, a render pass (single R8G8B8A8_UNORM color attachment, `LOAD_OP_CLEAR`), the two graphics pipelines, and a `render(cmd, targetView, coreView_or_null, w, h)` that: begins the render pass (clear to black), if `coreView` binds the sampler descriptor and draws the fullscreen triangle (core pipeline), then draws the overlay quad (overlay pipeline, push-constants), ends the pass.
`VulkanBackend::composite_and_present`: lazily create an offscreen `VkImage`+view (target) and a host-visible staging `VkBuffer`; if `has_frame`, the composite `vkQueueSubmit` **waits** the shared `timeline_` `>= sync_value` and **signals** `sync_value+1` (`VkTimelineSemaphoreSubmitInfo`); record: transition core image `SHADER_READ`, run `compositor_.render(...)`, transition offscreen to `TRANSFER_SRC`, `vkCmdCopyImageToBuffer` into staging; submit; `vkWaitForFences` (1s); map staging and copy into `out_rgba` (row by row). All CPU waits finite.

- [ ] **Step 5: Build and run — verify green (or SKIP)**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug -R retropark_tests --output-on-failure`
Expected: green outside the overlay, blended (blue-raised, green-reduced) inside — compositing proven in Vulkan. **Report RAN vs SKIPPED, the pixel values, and any validation output.**

- [ ] **Step 6: Commit**

```bash
git add src/render/vulkan/ tests/test_vulkan_compositor.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: Vulkan compositor with blended overlay + timeline-synced composite"
```

---

## Task 6: BackendFactory + Runtime api selection

**Files:**
- Create: `src/runtime/BackendFactory.h/.cpp`, `tests/test_backend_factory.cpp`
- Modify: `src/runtime/Runtime.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `std::unique_ptr<rp::IRenderBackend> rp::make_backend(rp_graphics_api api)` — `RP_GFX_D3D11`→`D3D11Backend`, `RP_GFX_VULKAN`→`VulkanBackend`, else `nullptr`. Runtime stores its `api_` and rejects a core whose `graphics_api != api_` with `RP_ERR_UNSUPPORTED`.

- [ ] **Step 1: Write the failing test**

`tests/test_backend_factory.cpp`:
```cpp
#include <doctest/doctest.h>
#include "runtime/BackendFactory.h"
using namespace rp;
TEST_CASE("factory: d3d11 and vulkan produce backends; unknown is null") {
    CHECK(make_backend(RP_GFX_D3D11) != nullptr);
    CHECK(make_backend(RP_GFX_VULKAN) != nullptr);
    CHECK(make_backend((rp_graphics_api)99) == nullptr);
}
```

- [ ] **Step 2: Implement the factory**

`src/runtime/BackendFactory.h`:
```cpp
#pragma once
#include <memory>
#include "render/IRenderBackend.h"
#include <retropark/retropark_abi.h>
namespace rp { std::unique_ptr<IRenderBackend> make_backend(rp_graphics_api api); }
```
`src/runtime/BackendFactory.cpp`:
```cpp
#include "runtime/BackendFactory.h"
#include "render/d3d11/D3D11Backend.h"
#include "render/vulkan/VulkanBackend.h"
namespace rp {
std::unique_ptr<IRenderBackend> make_backend(rp_graphics_api api) {
    switch (api) {
        case RP_GFX_D3D11:  return std::make_unique<D3D11Backend>();
        case RP_GFX_VULKAN: return std::make_unique<VulkanBackend>();
        default: return nullptr;
    }
}
}
```

- [ ] **Step 3: Wire Runtime to the factory**

`src/runtime/Runtime.h`: add `rp_graphics_api api_;`. `Runtime.cpp` constructor: `api_ = api; backend_ = make_backend(api); if (!backend_) { init_ok_ = false; return; }` then `init_ok_ = (backend_->initialize(...) == RP_OK);`. In `load_core`, change the graphics_api check to `if (m.graphics_api != api_) return RP_ERR_UNSUPPORTED;` (was hard-coded to `RP_GFX_D3D11`).

- [ ] **Step 4: Build and run — verify green**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: factory test passes; the D3D11 e2e still passes (Runtime now goes through the factory for d3d11).

- [ ] **Step 5: Commit**

```bash
git add src/runtime/BackendFactory.* tests/test_backend_factory.cpp src/runtime/Runtime.* CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: backend factory + runtime graphics-api selection"
```

---

## Task 7: Reference Vulkan presenting core (`refcore_present_vk.dll`)

**Files:**
- Create: `cores/refcore_present_vk/RefCoreVk.cpp`, `cores/refcore_present_vk/core.json`, `cores/refcore_present_vk/CMakeLists.txt`
- Modify: top-level `CMakeLists.txt`

**Interfaces:**
- Produces: a DLL exporting `rp_get_core_abi` with `graphics_api = RP_GFX_VULKAN`. On `set_surfaces`, creates its own `VkDevice` on the physical device whose UUID matches `set->device_uuid`, imports each surface's memory (`shared_handle`) as a `VkImage` + view + framebuffer, and imports the shared timeline semaphore (`set->sync_handle`). Its render thread runs the producer half of §3's protocol.

- [ ] **Step 1: Write the manifest**

`cores/refcore_present_vk/core.json`:
```json
{ "id":"refcore_present_vk", "name":"Reference Presenting Core (Vulkan)", "type":"presenting",
  "abi_version":1, "graphics_api":"vulkan", "entry":"refcore_present_vk.dll" }
```

- [ ] **Step 2: Write the core**

`cores/refcore_present_vk/RefCoreVk.cpp` — exports `rp_get_core_abi`. `create`: create `VkInstance` (same instance exts as the backend). `set_surfaces`: enumerate physical devices, pick the one whose `VkPhysicalDeviceIDProperties.deviceUUID == set->device_uuid` (else `RP_ERR_DEVICE`); create `VkDevice`+queue (timeline + external-memory/semaphore exts); for each `set->surfaces[i]` import memory via `VkImportMemoryWin32HandleInfoKHR{OPAQUE_WIN32, handle=shared_handle}`, create the `VkImage` (same create-info as the backend: `VkExternalMemoryImageCreateInfo`), bind, make a view + a render target (framebuffer + render pass, `LOAD_OP_CLEAR`); import the timeline semaphore via `vkImportSemaphoreWin32HandleKHR{OPAQUE_WIN32, handle=set->sync_handle}`. `start`: launch a `std::thread` running the loop; `stop`/`destroy`: signal stop, `join`, then `vkDeviceWaitIdle` + destroy in reverse order (never detach). Loop for frame `f=1,2,...`, slot `i=f % count`:
```
// wait host consumed the frame that last used slot i: T >= 2*(f-N)+1 (skip if f<=N) — as a submit wait
// render: clear framebuffer i to color (0, 1, t) with rising t; submit waits 2*(f-N)+1, signals 2f
// host.submit_frame(host, i, generation, 2*f)
// sleep ~16 ms
```
Use `VkTimelineSemaphoreSubmitInfo` with `pWaitSemaphoreValues`/`pSignalSemaphoreValues`; the wait stage is `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT`. Track `f` and `generation` (from `set->surfaces[i].generation`). Link `Vulkan::Vulkan`.

(As with Task 4, the implementer writes the field-level Vulkan setup with validation layers on; the handle-type bits, the import structs, and the timeline wait/signal *values* — `2f` signal, `2*(f-N)+1` wait — MUST match §3 exactly.)

- [ ] **Step 3: Write the core's CMake + emit the package**

`cores/refcore_present_vk/CMakeLists.txt`: `add_library(refcore_present_vk SHARED RefCoreVk.cpp)`, include `${CMAKE_SOURCE_DIR}/include`, link `Vulkan::Vulkan`, and a POST_BUILD command copying the dll + `core.json` into `$<TARGET_FILE_DIR:refcore_present_vk>/cores/refcore_present_vk/` (mirror the D3D11 core). Top-level `CMakeLists.txt`: `add_subdirectory(cores/refcore_present_vk)`.

- [ ] **Step 4: Build — confirm dll + manifest emitted**

Run: `cmake -S . -B build && cmake --build build --config Debug`
Expected: `refcore_present_vk.dll` + `core.json` under `build/.../cores/refcore_present_vk/`. (Behavior asserted in Task 8.)

- [ ] **Step 5: Commit**

```bash
git add cores/refcore_present_vk/ CMakeLists.txt
git commit -m "feat: Vulkan reference presenting core (own VkDevice, timeline producer)"
```

---

## Task 8: Vulkan end-to-end + reload/not-bricked

**Files:**
- Create: `tests/test_vulkan_e2e.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: the C API + the built `refcore_present_vk` package.

- [ ] **Step 1: Write the failing e2e test**

`tests/test_vulkan_e2e.cpp`: probe-guard. `rp_runtime_create(RP_GFX_VULKAN, nullptr)` → `resize(64,64)` → `load_core(RP_VK_CORE_DIR)` → pump `present(out)` up to 60×~16 ms until the core's green appears in the bottom-right quadrant; assert green there AND a blended (blue-raised, green-reduced) pixel in the top-left overlay quadrant. Then a reload case (load again on the same running runtime → RP_OK, frame still appears) and a not-bricked case (`load_core("no_such_dir")` → RP_ERR_NOT_FOUND, then real load → RP_OK + frame). Cleanup unload+destroy on every path. Bounded loops (`CHECK` for the poll result). Mirror `test_e2e.cpp` exactly, with `RP_GFX_VULKAN`.

- [ ] **Step 2: Wire CMake**

`tests/CMakeLists.txt`: `add_dependencies(retropark_tests refcore_present_vk)` and
`target_compile_definitions(retropark_tests PRIVATE RP_VK_CORE_DIR="$<TARGET_FILE_DIR:refcore_present_vk>/cores/refcore_present_vk")`. Add `test_vulkan_e2e.cpp`.

- [ ] **Step 3: Build and run — verify green (or SKIP)**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the Vulkan core (own `VkDevice` + thread) renders green into the host-exported image, timeline-synced, and the host composites the blended overlay. **Report RAN vs SKIPPED, the pixel values, and any validation output.** The D3D11 e2e must also still pass.

- [ ] **Step 4: Commit**

```bash
git add tests/test_vulkan_e2e.cpp tests/CMakeLists.txt
git commit -m "test: Vulkan end-to-end presenting core + overlay composite"
```

---

## Task 9: Windowed harness runs the Vulkan core

**Files:**
- Modify: `src/render/vulkan/VulkanBackend.h/.cpp` (windowed swapchain present path), `harness/windowed/main.cpp`, `harness/windowed/CMakeLists.txt`
- Modify: top-level `CMakeLists.txt`

**Interfaces:**
- Produces: `VulkanBackend::initialize(native_window!=nullptr,...)` creates a `VK_KHR_win32_surface` + `VK_KHR_swapchain`; `composite_and_present(..., out_rgba==nullptr)` acquires a swapchain image, composites into it (timeline-synced), and presents. The harness takes `--api d3d11|vulkan` (default d3d11) and loads the matching core.

- [ ] **Step 1: Add the swapchain path to VulkanBackend**

When `native_window != nullptr`: after device creation, create a `VkSurfaceKHR` via `vkCreateWin32SurfaceKHR` (enable instance ext `VK_KHR_SURFACE`/`VK_KHR_WIN32_SURFACE`), pick a `VK_FORMAT_B8G8R8A8_UNORM`/`R8G8B8A8_UNORM` surface format, create a `VkSwapchainKHR` (FIFO), get images + views. Enable device ext `VK_KHR_SWAPCHAIN`. In `composite_and_present` with `out_rgba==nullptr` and a swapchain: `vkAcquireNextImageKHR`, composite into that image's view (timeline wait `>= sync_value` if `has_frame`, signal `sync_value+1`), transition to `PRESENT_SRC`, `vkQueuePresentKHR`. Keep the headless offscreen+readback branch for `out_rgba!=nullptr`; return `RP_ERR_UNSUPPORTED` for the swapchain+readback combination (matching the D3D11 backend's guard).

- [ ] **Step 2: Parameterize the harness**

`harness/windowed/main.cpp`: parse `--api vulkan|d3d11` from the command line (default d3d11). Map to `RP_GFX_VULKAN`/`RP_GFX_D3D11`, pass to `rp_runtime_create`, and choose the core dir accordingly (`RP_HARNESS_CORE_DIR_VK` vs the existing D3D11 one, both set via `target_compile_definitions`). Everything else (window, present loop, cleanup) is unchanged.

- [ ] **Step 3: Wire CMake**

`harness/windowed/CMakeLists.txt`: `add_dependencies(retropark_harness refcore_present_vk)` and add `RP_HARNESS_CORE_DIR_VK="$<TARGET_FILE_DIR:refcore_present_vk>/cores/refcore_present_vk"`.

- [ ] **Step 4: Build, smoke-test the Vulkan window, screenshot**

Run: `cmake -S . -B build && cmake --build build --config Debug`. Launch `retropark_harness.exe --api vulkan` in the background, wait ~3 s, `PrintWindow`-capture the window (title "RetroPark Slice A") to `C:\Users\cubma\source\repos\RetroPark\.superpowers\sdd\harness-vk-shot.png`, then terminate the process. Confirm the window shows the animated green→blue core with the blue-tinted overlay (same visual as the D3D11 harness). Also confirm `--api d3d11` still works. If the window is black, diagnose (UUID match, timeline wait/signal values, swapchain format) — do not commit a black window.

- [ ] **Step 5: Commit**

```bash
git add src/render/vulkan/ harness/windowed/ CMakeLists.txt
git commit -m "feat: Vulkan windowed swapchain present path + harness --api selection"
```

---

## Self-Review

**Spec coverage:**
- §1 ABI evolution (`rp_surface_set`, `submit_frame` sync_value) → Task 1. ✓
- §2 architecture parallel (VulkanBackend, exported images, timeline, refcore_present_vk, swapchain) → Tasks 3,4,5,7,9. ✓
- §3 sync protocol (single timeline, `2f`/`2f+1`, backpressure) → producer in Task 7, consumer in Tasks 4/5, values pinned in Global Constraints + Task 5/7 steps. ✓
- §4 components → all created across Tasks 3-7. ✓
- §5 build/SDK (`find_package(Vulkan)`, build-time glslc, validation) → Task 2 + Global Constraints. ✓
- §6 error handling (no-device→UNSUPPORTED, UUID mismatch→DEVICE, import fail→DEVICE, finite fence timeout, RAII handles) → Tasks 3,4,5,7 steps + Global Constraints. ✓
- §7 testing (probe-guarded handoff/compositor/e2e on real GPU, Slice A stays green) → Tasks 3,4,5,8 + Global Constraints. ✓
- §8 scope (in: all listed; out: cross-API, driven, etc.) → the plan builds exactly the in-scope items; nothing out-of-scope is built. ✓
- §9 repo additions → File Structure matches. ✓

**Deviation from spec (noted honestly):** the spec said "IRenderBackend unchanged." The plan changes `composite_and_present` to take `sync_value` and adds two **defaulted** accessors (`present_sync_handle`, `present_device_uuid`); `initialize`/`allocate_surfaces` are unchanged and `D3D11Backend` needs no behavioral change. This is the minimal way to route the timeline value/handle to the backend; the abstraction's roles and method set are otherwise identical. Captured in Global Constraints ("Interface honesty").

**Placeholder scan:** the two crux Vulkan tasks (4, 7) describe field-level struct setup in prose rather than pasting every field of every `Vk*CreateInfo`, but they name the exact extensions, handle-type bits, struct types, and — critically — the timeline **values** (`2f` signal, `2*(f-N)+1` wait, `sync_value`/`sync_value+1` on the host) that carry the correctness. This is a deliberate granularity choice for Vulkan boilerplate (mirroring how Slice A's plan gave real D3D11 calls without every field), executed with validation layers on. All *logic-bearing* values and all non-Vulkan code are complete.

**Type consistency:** `rp_surface_set` fields, `submit_frame(...,sync_value)`, `set_surfaces(const rp_surface_set*)`, `composite_and_present(index, sync_value, has_frame, out_rgba, err)`, `SurfaceRing::accept_submit(index,gen,sync_value)`/`latest_ready(index,sync_value)`, `make_backend(api)`, and `probe_vulkan_shared()` are used identically across Tasks 1,3,4,5,6,7,8. The timeline convention (`2f`/`2f+1`) is stated once in Global Constraints and referenced by Tasks 4,5,7 consistently.
