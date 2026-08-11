# RetroPark — RPCS3 Arc, Slice J Design (package `rpcs3_present`: shared-VkImage handoff into the Runtime)

**Date:** 2026-08-11
**Status:** Draft (design) — one open design decision (see §6), then plan → SDD.
**Scope:** Package the from-source RPCS3 embed (Slice I) as a reusable **`rpcs3_present` RetroPark core** that
renders into RetroPark's host-owned shared `VkImage`, so the Runtime loads and composites RPCS3 exactly like
`dolphin_present`. The RPCS3 analog of Dolphin Slices J+K combined. **ABI stays v5** (no bump).

---

## 0. Context — what Slice I proved, and the template

Slice I (DONE, `docs/rpcs3-embed-findings.md`): a from-source RPCS3 boots LittleBigPlanet headless and renders
real Vulkan frames under our control via a **custom `GSFrameBase`** (`rp_gs_frame`, a hidden Win32 window);
`VKGSRender` builds a swapchain on our HWND and presents (36-43 frames counted in `flip()`; BGRA readback
captured). The standalone host is `external/rpcs3/rpcs3/rp_rpcs3/rp_rpcs3.cpp`.

**Template = `dolphin_present`** (the proven second presenting core, itself modelled on `refcore_present_vk`).
The exact contract it satisfies (mapped this session):

- **ABI** (`include/retropark/retropark_abi.h`, v5): a presenting core exports
  `const rp_core_abi* rp_get_core_abi(void)` and implements `get_info / create / set_surfaces / start / stop /
  destroy / get_av_info / load_content` (+ optional `serialize*`); `run_frame` stays NULL (host doesn't drive
  presenting cores).
- **Surfaces handoff** — `set_surfaces(rp_surface_set*)` delivers: `surfaces[0].shared_handle` (OPAQUE_WIN32
  NT handle to the host's exported `VkImage`, R8G8B8A8_UNORM), `sync_handle` (exported **timeline semaphore**
  NT handle), `device_uuid[16]` (the host `VkPhysicalDevice` UUID), `width/height`, `generation`. Only
  `surfaces[0]` is used (single shared image).
- **Host callbacks** — `create(rp_host_iface*)` gives `submit_frame(host, index, generation, sync_value)`
  (the flip: "slot ready at timeline value"), `audio_sample(host, s16*, n)`, `input_state(host, port, out)`,
  `log`.
- **Handoff mechanism (zero-copy):** the core imports the host's exported image+timeline **into RPCS3's own
  VkDevice** (dedicated `VkImportMemoryWin32HandleInfoKHR` OPAQUE_WIN32 + `vkImportSemaphoreWin32HandleKHR`),
  forces RPCS3's Vulkan adapter to `device_uuid` (same physical GPU), and per-frame blits its rendered image
  into the shared image. RPCS3 and RetroPark keep **separate VkDevices** — the share is at the memory level,
  valid only on the same physical GPU. Host and core each have their own instance/device; nothing is shared
  except the imported memory + semaphore.
- **Timeline protocol (lock-step):** producer (core) signals **even** `2f+2`; host consumer signals **odd**
  `2f+1`; the producer **waits `2f+1`** before producing frame `f` (host has consumed the prior frame). Shared
  image lives in `GENERAL` its whole life; ownership moves by **QFOT** release to
  `VK_QUEUE_FAMILY_EXTERNAL_KHR` (core) / acquire from it (host).
- **Packaging:** a `DynamicLibrary` target `rpcs3_present.dll` (one source `rp_rpcs3_present.cpp`, references
  the RPCS3 core libs, includes RetroPark's `include/`), + `cores/rpcs3_present/core.json`
  (`type:"presenting", graphics_api:"vulkan", entry:"rpcs3_present.dll", requires_content:true, abi_version:5`).
  Runtime loads via `Runtime::load_core` → `Win32CoreModule::open` → `CoreLoader` (resolves `rp_get_core_abi`,
  checks `abi_version == 5`); a `requires_content` presenting core is started **after** `load_content`.

## 1. The crux — getting RPCS3's rendered frame into the host's imported image

Dolphin blits its just-rendered **XFB** into the imported image on `after_present_event`. The RPCS3 equivalent
hooks our `rp_gs_frame`'s present path. Two sources for the frame (Slice I present-seam report):

- **GPU-side (preferred):** `VKGSRender`'s final composited image via `get_present_source()` /
  `VKGSRender::flip` in `Emu/RSX/VK/VKPresent.cpp`. Blit it (`vkCmdBlitImage`) into the imported shared image
  on RPCS3's device — a **format-converting blit** (RPCS3 `B8G8R8A8_UNORM` → host `R8G8B8A8_UNORM`; blit does
  the channel swap). Then the identical producer tail: wait `2f+1`, barrier `UNDEFINED→GENERAL`, blit, QFOT
  release `→EXTERNAL`, submit signalling `2f+2`, `rp_host.submit_frame(host, 0, generation, 2f+2)`.
- **CPU-side (fallback, already proven):** the `take_screenshot`/`present_frame` readback hands us a BGRA CPU
  buffer; upload it into the shared image via a staging buffer. Device-UUID-independent (works even if RPCS3
  can't be forced onto the host GPU) but slower, and **note the Slice-I finding: the readback crashes LBP
  during early boot** — so the CPU path inherits that instability.

Accessing RPCS3's `VkDevice`/queue/present image from our core code: `rp_gs_frame` already lives inside the
RPCS3 embed and `VKGSRender` is ours to hook; the core reaches RPCS3's `vk::get_current_renderer()` /
device/queue the way `VKPresent` does. Exact accessor to be nailed in the plan.

## 2. Components (Slice J)

### `rp_rpcs3_present.cpp` — the loadable core (new; the RPCS3 analog of `rp_dolphin.cpp`)
- The `rp_core_abi` vtable (`rp_get_core_abi`) + `dp_*`-style functions: `get_info` (id `"rpcs3_present"`,
  `RP_CORE_PRESENTING`, `RP_GFX_VULKAN`); `create` (stash `rp_host_iface`); `load_content` (store EBOOT.BIN
  path — **boot the EBOOT file, not the `.ps3` folder**, per Slice I); `set_surfaces` (stash handles + UUID +
  generation); `start` (boot RPCS3 on its own thread, non-headless, renderer=vulkan, our callbacks — the
  Slice-I `main_application` subclass, minus the standalone `main()`); `stop`/`destroy`; `get_av_info`
  (`sample_rate=48000`, geometry 0); `serialize*` optional (RPCS3 savestate — later); `run_frame` NULL.
- Force RPCS3's Vulkan adapter to the host `device_uuid` before boot (RPCS3's `render_creator` enumerates by
  name/UUID — Slice I confirmed it finds the discrete GPU); import the shared image + timeline into RPCS3's
  device; producer tail on each present.
- The four Slice-I `run_rpcs3`-bypass fixes still apply (EBOOT path, file logger, `SetProcessWorkingSetSize`,
  defer/avoid the CPU readback), carried into the core's `start`.

### Producer glue on the present seam
- A `XfbProducer`-equivalent that imports once (lazily, first present), waits/blits/QFOT/signals per frame, and
  calls `submit_frame`. Modeled line-for-line on the patch's `XfbProducer` (`docs/patches/dolphin-external-present.patch`).

### Audio + input egress
- **Audio:** RPCS3 null audio backend → pull mixed interleaved s16 → `rp_host.audio_sample`.
- **Input:** `rp_host.input_state(host, 0, &in)` → drive RPCS3's pad. **RPCS3 wrinkle** (Slice I): RPCS3's input
  is a large web in `rpcs3_ui` (no clean per-pad override like Dolphin's `SetInputOverrideFunction`); driving
  it needs a `pad_thread`/handler shim — scope carefully in the plan (may be a follow-up slice).

### Packaging + manifest
- `RetroParkRpcs3.vcxproj` (`DynamicLibrary`, `TargetName=rpcs3_present`) or the CMake equivalent, linking the
  RPCS3 core libs (`rpcs3_lib` closure, Qt 6.10.3 on D) + RetroPark `include/`; `AfterBuild` copies the DLL +
  `cores/rpcs3_present/core.json` into an `rp_core` dir. **Heavy:** links the full RPCS3 (LLVM etc.), unlike
  the light DLLs — packaging/link size is a real concern to validate.

## 3. Testing (Slice J)

- **Runtime-load gate:** the Runtime loads `cores/rpcs3_present`, `set_surfaces` imports succeed, `start` boots
  LBP, and `submit_frame` fires with climbing `sync_value` — proven headlessly like `dolphin_present`'s gate.
- **Handoff proof:** the host composites the shared image; a readback of the host's composited target shows the
  RPCS3 frame (even the black loading frames prove the plumbing; a visible frame is gated on the Slice-I
  LBP-content limitation — the handoff is provable regardless).
- **Timeline/QFOT correctness:** no validation errors, no deadlock, lock-step holds (producer even / host odd).
- **Regression:** `dolphin_present` + `refcore_present_vk` still load/run (shared Runtime path unchanged).

## 4. Scope

**In Slice J:** the `rpcs3_present` loadable core (ABI v5) — `get_info/create/load_content/set_surfaces/start/
stop/destroy/get_av_info`; import the host's shared image + timeline into RPCS3's device (UUID-matched); the
per-frame producer (blit → QFOT → signal → `submit_frame`); audio egress; packaging + `core.json`; the
Runtime-load + handoff gate. **No RetroPark Runtime/ABI changes** (the presenting contract already exists).

**Out (later):** full input wiring (the RPCS3 pad web — likely its own slice); savestate over the ABI;
the visible-content frame (LBP-boot limitation); the static-core/iOS shape (RPCS3 is desktop-only — JIT);
non-Vulkan.

**The single provable claim:** *RetroPark's Runtime loads a `rpcs3_present` core, hands it a shared `VkImage` +
timeline, and RPCS3 boots LittleBigPlanet and hands its rendered Vulkan frame back into that image every frame
(lock-step, QFOT, timeline-synced) — composited by the host exactly like `dolphin_present`. RPCS3 is now a
first-class RetroPark presenting core; the presenting-core pattern is proven end-to-end for a second heavy app.*

## 5. Repo additions

```
external/rpcs3/.../rp_rpcs3_present.cpp          # the loadable core (patch/notes; external/rpcs3 git-ignored)
external/rpcs3/.../RetroParkRpcs3 build target    # DynamicLibrary rpcs3_present.dll (patch)
cores/rpcs3_present/core.json                     # manifest (committed — RetroPark's own artifact)
docs/patches/rpcs3-external-present.patch          # all RPCS3-side present glue (the record)
docs/rpcs3-seam.md                                 # (optional) present-seam report, or fold into findings
```

## 6. Open design decision (needs a call before the plan)

**Frame-transfer path:** (A) **UUID-matched GPU-side blit** — zero-copy, fast, the `dolphin_present` way; needs
RPCS3 forced onto the host's GPU by `device_uuid` and a format-converting blit. (B) **CPU staging copy first**
— device-independent, reuses the already-working `present_frame` readback, but slower AND inherits the Slice-I
early-boot readback crash. Recommendation: **A** (matches Dolphin, avoids the readback-crash path, and the GPU
is a single discrete adapter here so the UUID match is straightforward), with B documented as the portable
fallback. Also flag: with LBP not presenting past its boot logos (Slice I), the handoff gate will show black
frames until the LBP-content limitation is separately solved — the plumbing is still fully provable.
