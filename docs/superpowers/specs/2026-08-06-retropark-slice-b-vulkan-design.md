# RetroPark — Slice B Design (Vulkan backend)

**Date:** 2026-08-06
**Status:** Approved (design), pending implementation plan
**Scope:** A second `IRenderBackend` — Vulkan — that proves the render abstraction is not
D3D-shaped, by mirroring Slice A's presenting-core pipeline entirely in Vulkan on Windows.

---

## 0. Context and goal

Slice A (merged) proved the presenting model on **D3D11/WARP**: a dynamically-loaded core
that owns its own GPU device + render thread renders into a host-owned shared texture
(shared NT handle + `IDXGIKeyedMutex`), and the host composites a blended overlay over it.

Slice B builds a **Vulkan** implementation of the same `IRenderBackend` interface, plus a
Vulkan reference core, and runs the identical end-to-end claim on Vulkan. Its purpose is to
**prove the abstraction abstracts** — that a second, unrelated GPU API drops in behind the
same interface without reshaping it. It also delivers the portable backend that later
unlocks Android/Linux.

### Decisions already made (this brainstorm)

| Decision | Choice |
|---|---|
| Scope | **All-Vulkan**: `VulkanBackend` + `refcore_present_vk`, mirroring Slice A. **No cross-API interop** (D3D11-host ↔ Vulkan-core) — that is a later slice. |
| Sync primitive | **Shared exported timeline semaphore** (`VK_KHR_external_semaphore_win32`), extending the presenting handoff. Vulkan sync is a *separate object* from the image, unlike D3D11's intrinsic keyed mutex — so the ABI must convey it. |
| Same-device rule | Host and core must use the **same `VkPhysicalDevice`** (matched by `deviceUUID`) for external-memory import — the Vulkan analog of Slice A's same-adapter rule. |
| Backend selection | Runtime gains a **backend factory**: `rp_graphics_api` → `D3D11Backend` \| `VulkanBackend`. A core's `graphics_api` must equal the runtime's backend, else `RP_ERR_UNSUPPORTED`. |
| Shaders | **GLSL → SPIR-V at build time via `glslc`** (Vulkan SDK 1.4.357.0, installed at `C:\VulkanSDK`), compiled by a CMake custom command and embedded. |
| Testing | GPU tests run on the **real GPU** (RTX 5080), **probe-guarded** to skip cleanly when no suitable Vulkan device / external-memory support is present. There is no software-Vulkan WARP equivalent, and software Vulkan generally lacks the Win32 external-memory extensions the handoff needs, so real hardware is the correct (and only viable) target. Validation layers (`VK_LAYER_KHRONOS_validation`) are enabled in Debug test runs. |
| `IRenderBackend` | **Unchanged.** This is the proof: the abstraction carries Vulkan without edits. |

---

## 1. ABI evolution (deliberate, while zero external cores exist)

Slice A's presenting handoff carried one thing per surface — the image `shared_handle` — and
sync came free from the keyed mutex baked into the D3D texture. Vulkan needs two more pieces
conveyed across the boundary: **which GPU** to import on, and a **shared semaphore** for sync.
Both are added now, cleanly, because there are no external cores yet.

### 1.1 Bundled surface handoff

Replace the flat `set_surfaces(core, descs, count)` with a bundling struct so ring-wide
information (device identity, sync object) is not smeared across per-surface descriptors:

```c
typedef struct rp_surface_set {
    uint32_t                count;        /* number of surfaces in `surfaces` */
    uint32_t                reserved;
    const rp_surface_desc*  surfaces;     /* per-surface image handles (as in Slice A) */
    void*                   sync_handle;  /* shared timeline semaphore NT handle (Vulkan); NULL for D3D11 */
    uint8_t                 device_uuid[16]; /* target VkPhysicalDevice UUID (Vulkan); all-zero for D3D11 */
} rp_surface_set;

/* Replaces the Slice A signature. */
rp_result (*set_surfaces)(rp_core* core, const rp_surface_set* set);
```

`rp_surface_desc` is unchanged (`index`, `width`, `height`, `format`, `shared_handle`,
`generation`). The D3D11 backend fills `sync_handle = NULL` and `device_uuid = {0}`; its core
ignores them (keyed mutex is intrinsic). The Vulkan backend fills both; its core uses them.

### 1.2 `submit_frame` gains a sync value

The host must know which timeline value to wait on before compositing a submitted frame:

```c
/* Slice A: submit_frame(host, index, generation) */
void (*submit_frame)(rp_host* host, uint32_t index, uint64_t generation, uint64_t sync_value);
```

D3D11 cores pass `sync_value = 0` (unused). Vulkan cores pass the timeline value they signaled
(see §3). The runtime stores the latest ready frame's `sync_value` alongside its index so the
Vulkan backend can wait on it at composite time; the D3D11 backend ignores it.

These two changes ripple mechanically through the D3D11 path (refcore, mock core, `CoreLoader`,
`Runtime`) — all in-tree, all passing zeros — and are covered by the existing Slice A tests,
which must stay green.

---

## 2. Architecture (parallel to Slice A)

| Slice A (D3D11) | Slice B (Vulkan) |
|---|---|
| `D3D11Backend : IRenderBackend` | `VulkanBackend : IRenderBackend` (same interface) |
| WARP device (host + core, same adapter) | Real `VkPhysicalDevice`, host + core matched by `deviceUUID` |
| Shared NT-handle `ID3D11Texture2D` | `VkImage` bound to **exported `VkDeviceMemory`** (`VK_KHR_external_memory_win32`, `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32`) |
| `IDXGIKeyedMutex` (keys 0/1) | Shared **timeline semaphore** (`VK_KHR_timeline_semaphore` + `VK_KHR_external_semaphore_win32`) |
| HLSL via runtime `D3DCompile` | GLSL → SPIR-V via build-time `glslc`, embedded as `uint32_t[]` |
| `refcore_present.dll` | `refcore_present_vk.dll` (own `VkDevice`, own render thread) |
| WARP swapchain / offscreen readback | `VK_KHR_swapchain` + `VK_KHR_win32_surface` (windowed); offscreen `VkImage` + staging buffer readback (headless) |

`VulkanBackend` owns: a `VkInstance` (with validation + `VK_KHR_external_memory_capabilities`),
a chosen `VkPhysicalDevice` (prefer discrete GPU) whose `deviceUUID` it exposes, a `VkDevice` +
graphics/present queue, a ring of exported shared `VkImage`s, one exported shared timeline
semaphore, the compositor (pipeline + SPIR-V), and (windowed) a swapchain. Headless composite
renders into an offscreen `VkImage` and copies to a host-visible staging buffer for pixel
readback — the Vulkan analog of Slice A's staging-texture readback.

---

## 3. Synchronization protocol (the one new mechanism)

A single shared timeline semaphore `T` (initial value 0), exported by the host and imported by
the core, provides correct **bidirectional** producer/consumer sync with no CPU stalls. Values
are split even/odd so the two devices' signals stay globally monotonic:

- **Even `2f`** = "core finished rendering frame `f`" (producer signal).
- **Odd `2f+1`** = "host finished consuming frame `f`" (consumer signal).

Let `N` = ring depth, slot `i = f mod N`.

**Core (producer), for frame `f` = 1,2,3…:**
1. Its render queue submit **waits** `T >= 2*(f-N)+1` (host done consuming the frame that last
   used slot `i`; skipped for `f <= N`), **signals** `T = 2f` on completion.
2. After enqueuing, calls `submit_frame(index=i, generation, sync_value=2f)`.

**Host (consumer), when compositing the latest-ready frame (value `2f`):**
1. Its composite queue submit **waits** `T >= 2f` (core done rendering), **signals** `T = 2f+1`
   on completion, then presents / reads back.

Monotonicity holds: the signal sequence is `2, 3, 4, 5, …` as producer and consumer alternate,
and the core's next producer signal `2(f+1)` always exceeds the host's last `2f+1`. Backpressure
(`wait 2*(f-N)+1`) prevents the core overwriting a slot the host is still reading — the
correctness the keyed mutex gave Slice A for free. All waits are GPU-side; a **finite host-side
fence timeout** guards CPU waits (readback), never infinite — mirroring Slice A's 100 ms rule.

The generation/index round-trip (ring → desc → core → `submit_frame` → `accept_submit` →
`latest_ready`) is unchanged from Slice A; `sync_value` rides alongside `index` in the ring's
"latest ready" record.

---

## 4. Components

| Component | Responsibility |
|---|---|
| `src/render/vulkan/VulkanBackend.h/.cpp` | `IRenderBackend` impl: instance/device, physical-device pick + UUID, exported shared image ring, exported timeline semaphore, composite+present/readback |
| `src/render/vulkan/VulkanCompositor.h/.cpp` | Graphics pipeline: fullscreen triangle sampling the core image (opaque) + blended overlay quad (`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`) |
| `src/render/vulkan/shaders/*.vert/.frag` | GLSL sources; `glslc` compiles to SPIR-V at build time; embedded via a generated header |
| `src/render/vulkan/VulkanShaders.h` (generated) | Embedded SPIR-V `uint32_t[]` blobs |
| `cores/refcore_present_vk/RefCoreVk.cpp` + `core.json` | Vulkan reference presenting core: own `VkDevice` on the matched UUID, imports images + timeline, animates green→blue, signals `2f`, `submit_frame` |
| `src/runtime/BackendFactory.h/.cpp` | `rp_graphics_api` → `unique_ptr<IRenderBackend>` |
| `src/runtime/Runtime.cpp` (edit) | Use the factory; enforce core `graphics_api` == runtime api |
| `include/retropark/retropark_abi.h` (edit) | `rp_surface_set`, `submit_frame` sync_value |
| `cmake/Shaders.cmake` | `glslc` compile → SPIR-V → embed function |

---

## 5. Build / SDK integration

- CMake locates the SDK via the `VULKAN_SDK` environment variable (`C:\VulkanSDK\1.4.357.0`):
  headers from `$VULKAN_SDK/Include`, import lib `$VULKAN_SDK/Lib/vulkan-1.lib`, `glslc` from
  `$VULKAN_SDK/Bin`. Prefer CMake's `find_package(Vulkan REQUIRED COMPONENTS glslc)`.
- A `compile_shader(target, shader.glsl)` helper runs `glslc` at build time, emitting a SPIR-V
  `.spv` and a generated C header embedding it as `uint32_t[]` (no runtime shader compiler, no
  filesystem dependency at runtime).
- Debug test/harness runs enable `VK_LAYER_KHRONOS_validation`; the build must be
  **validation-clean** (no validation errors/warnings) — treated like Slice A's warning-clean bar.

---

## 6. Error handling

Mirrors Slice A's structured-error discipline, plus Vulkan specifics:

- **No suitable device:** if no `VkPhysicalDevice` supports the required external-memory /
  external-semaphore / timeline features, `initialize` returns `RP_ERR_UNSUPPORTED` (and GPU
  tests skip via the probe).
- **Device-UUID mismatch:** if the core cannot find a physical device matching the host's
  `device_uuid`, `set_surfaces` returns `RP_ERR_DEVICE` (external import would be invalid).
- **Import failure:** a failed `vkImportSemaphoreWin32HandleKHR` / memory import →
  `RP_ERR_DEVICE`, clean teardown.
- **Fence-wait timeout:** finite timeout on host CPU waits → `RP_ERR_TIMEOUT`, frame skipped
  (never an infinite wait), matching Slice A.
- **Handle ownership:** every exported Win32 handle (image memory, semaphore) is owned by an
  RAII wrapper that `CloseHandle`s exactly once, mirroring the move-only `Surface` fix from
  Slice A. Vulkan objects use RAII wrappers (explicit `vkDestroy*` in destructors, reverse order).
- **Crash honesty:** as in Slice A, a bad core can still fault the host; out-of-process isolation
  remains a later slice. Not claimed here.

---

## 7. Testing

- **Pure logic:** `BackendFactory` selection, ABI struct round-trips, any new bookkeeping —
  unit-tested, software-only.
- **Vulkan handoff (real GPU, probe-guarded):** a producer on a second `VkDevice` (same physical
  device) imports a host-exported shared image + timeline, clears it a known colour, signals the
  timeline; the host waits on the timeline and reads the pixel back. Asserts the exact colour —
  the direct analog of Slice A's `test_d3d11_handoff`. Skips with a `WARN` if
  `probe_vulkan_shared()` reports no suitable device.
- **Vulkan compositor (real GPU):** producer fills the shared image green; host composites; read
  back asserts pure green outside the overlay and a **blended** (blue-raised, green-reduced)
  pixel inside — proving compositing, not layering, exactly as Slice A's `test_compositor`.
- **Vulkan e2e (real GPU):** load `refcore_present_vk` through the C API, pump frames, assert the
  animated green core frame + blended overlay. Plus reload + not-bricked cases, as in Slice A.
- **Regression:** the entire Slice A D3D11 suite must remain green after the ABI changes (proving
  the changes are additive and the D3D11 path still works).
- **Probe pattern:** `probe_vulkan_shared()` creates a throwaway instance/device and checks for
  the external-memory + external-semaphore + timeline features and opaque-Win32 handle support;
  GPU tests early-return with `WARN` if it fails, so the suite stays green on machines without a
  capable Vulkan GPU (CI portability), exactly like `probe_shared_keyed_mutex`.

---

## 8. Scope line

**In Slice B:**
- ABI: `rp_surface_set` + `submit_frame` sync_value; D3D11 path updated to pass zeros.
- CMake Vulkan/SDK integration + build-time `glslc` shader embedding.
- `VulkanBackend` (instance/device/physical-device pick, exported shared image ring, exported
  timeline semaphore, headless composite + readback, windowed swapchain present).
- `VulkanCompositor` + SPIR-V shaders (fullscreen core + blended overlay).
- `refcore_present_vk` core (own device on matched UUID, timeline protocol).
- `BackendFactory` + Runtime api-match enforcement.
- Vulkan handoff / compositor / e2e tests (real GPU, probe-guarded) + reload/not-bricked.
- Windowed harness runs the Vulkan core (harness selects api).

**Explicitly out (later slices):**
- **Cross-API interop** (D3D11 host ↔ Vulkan core, GL cores, etc.).
- The **driven** execution model, libretro shim, wrapping real heavy apps, out-of-process
  isolation, real network download/catalog, audio, iOS/Android, EverythingBox integration.
- MoltenVK / macOS, Android surface integration (the portable backend enables these later, but
  they are not built here).

**The single provable claim of Slice B:** *the same `IRenderBackend` abstraction carries a
second, unrelated GPU API — a Vulkan presenting core with its own `VkDevice` and render thread
renders into a host-exported shared `VkImage`, synchronized by a shared timeline semaphore, and
the host composites a blended overlay over it — proven headless on the real GPU and shown in the
windowed harness — with the entire Slice A D3D11 path still green.*

## 9. Repo additions

```
RetroPark/
  cmake/Shaders.cmake                      # glslc → SPIR-V → embedded header
  src/render/vulkan/
    VulkanBackend.h/.cpp
    VulkanCompositor.h/.cpp
    VulkanShaders.h                        # generated (embedded SPIR-V)
    shaders/fullscreen.vert / sample.frag / overlay.vert / overlay.frag
  src/runtime/BackendFactory.h/.cpp
  cores/refcore_present_vk/
    RefCoreVk.cpp
    core.json                              # graphics_api: vulkan
    CMakeLists.txt
  tests/
    test_vulkan_handoff.cpp
    test_vulkan_compositor.cpp
    test_vulkan_e2e.cpp
    test_backend_factory.cpp
```
