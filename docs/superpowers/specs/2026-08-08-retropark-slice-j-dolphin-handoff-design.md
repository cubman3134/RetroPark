# RetroPark — Slice J Design (Dolphin renders into RetroPark's shared VkImage)

**Date:** 2026-08-08
**Status:** Approved (design), pending implementation plan
**Scope:** Retarget standalone Dolphin's Vulkan present so it renders **directly into RetroPark's
exported shared `VkImage`** (zero copy), synchronized by the Slice B timeline, and have RetroPark's
Vulkan compositor blend an overlay and present. In-process (one process, two `VkDevice`s). The second
slice of the Dolphin arc (I = build + verified frame; **J = zero-copy handoff**; K = reusable
`dolphin_present` core + content ABI + overlay + harness).

---

## 0. Context and goal

Slice I proved standalone Dolphin (tag 2606, `external/dolphin`) builds and renders the real Billy
Hatcher ROM on its Vulkan backend (`docs/dolphin-build.md`), and documented the present + init seams
(`docs/dolphin-seam.md`). Slice B proved the *host* half — a **separate `VkDevice`**
(`refcore_present_vk`) rendering into RetroPark's **exported shared `VkImage`** + shared **timeline
semaphore**, composited by our Vulkan compositor, cross-device-clean. Slice J connects them: **Dolphin
becomes the producer in that exact handoff** — its final frame lands in our image, no copy, and our
compositor blends an overlay on top. One window (ours), native speed.

### Decisions already made (this brainstorm)

| Decision | Choice |
|---|---|
| Handoff | **Render-into-adopted-image (pure zero-copy).** Dolphin's "backbuffer" IS our imported shared `VkImage`; `RenderXFBToScreen` renders the XFB into it (Dolphin's normal final step, now targeting our image → no extra copy). Not the copy-from-XFB blit. |
| Process model | **In-process for now** (one process, two `VkDevice`s, shared image via external memory — exactly Slice B). Out-of-process crash isolation is a later slice. |
| Sync | **Reuse the Slice B protocol verbatim** — timeline `2f` produce / `2f+1` consume, image `GENERAL` throughout, `GENERAL→GENERAL` QFOT on both devices. |
| Dolphin source changes | **Captured as a committed `.patch`** on tag 2606 (`external/dolphin` is git-ignored) — reproducible. |
| Risk valve | If the swapchain-bypass surgery proves too invasive to land cleanly, fall back to **copy-from-XFB** (one negligible on-GPU blit, pixel-identical result) so the slice still lands. Flag at the wall, don't thrash. |

---

## 1. Components

### Dolphin side (patched in `external/dolphin`, tag 2606 — captured as a `.patch`)
1. **External-memory/semaphore extensions + device match** (`VideoBackends/Vulkan/VulkanContext.cpp`):
   enable `VK_KHR_external_memory` + `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore` +
   `VK_KHR_external_semaphore_win32` (device), and the `VK_KHR_external_memory_capabilities` /
   `get_physical_device_properties2` instance extensions — none are enabled today
   (`SelectDeviceExtensions` 612-675, instance selection ~135-137/347/380). Select the
   `VkPhysicalDevice` whose `deviceUUID` matches RetroPark's (Slice B); on a single-GPU host they
   coincide, but match explicitly for correctness.
2. **External present target** — a `SwapChain`-mode (or a small sibling of `VKSwapChain`) whose
   "backbuffer" is our **imported** shared `VkImage`: `VkImportMemoryWin32HandleInfoKHR` on the NT
   handle RetroPark exported → `VKTexture::CreateAdopted(config, vkimage, …)` (already the mechanism
   each swapchain image uses, `VKTexture.h:60-63`) → `VKFramebuffer::Create`. Import RetroPark's shared
   **timeline semaphore** likewise. `IsHeadless()` must stay **false** (else
   `Presenter::Present()` early-returns, `Present.cpp:906`) — i.e. Dolphin has a valid present target
   that is our image, not a `VkSwapchainKHR`.
3. **`VKGfx::BindBackbuffer`** (`VKGfx.cpp:220-307`): when in external-present mode, skip
   `m_swap_chain->AcquireNextImage()` and bind the adopted-external-image framebuffer; keep the image in
   `GENERAL` (Slice B) rather than `COLOR_ATTACHMENT_OPTIMAL` for the cross-device handoff.
4. **`VKGfx::PresentBackbuffer`** (`VKGfx.cpp:309-335`): skip the `PRESENT_SRC_KHR` transition +
   `vkQueuePresentKHR`; instead do the Slice-B `GENERAL→GENERAL` QFOT release and **signal the imported
   timeline** at value `2f` — via `CommandBufferManager::SubmitCommandBuffer` extended to accept an
   extra "signal this timeline semaphore at this value" (the follow-up read `docs/dolphin-seam.md`
   flagged).

### RetroPark side (reuse Slice B — no new host code expected)
- `VulkanBackend` exports the shared `VkImage`(s) + timeline (as it already does for
  `refcore_present_vk`); `VulkanCompositor` waits the timeline at `2f`, composites the overlay, presents
  / reads back, signals `2f+1`. Dolphin is simply a new *producer* against the identical host surface.

### In-process host / vehicle
Dolphin is driven **in-process**: link `DolphinLib` (from `external/dolphin`), init Dolphin via the
seam-report sequence (`UICommon::Init` → `WindowSystemInfo` → `BootManager::BootCore`), hand it the
external present target (our imported image + timeline), boot Billy Hatcher, run N frames. The concrete
vehicle (a `cores/dolphin_present` producer that plugs into the existing Vulkan presenting path, vs. a
dedicated gated test target that links `DolphinLib`) is a plan decision; either keeps it in-process.
Full `rp_core_abi` packaging (content ABI, lifecycle polish, harness) is **Slice K**.

## 2. Data flow (per frame f)

```
RetroPark VulkanBackend: export shared VkImage + timeline (Slice B)
   → Dolphin imports both on its own VkDevice (matched UUID)
   → boot Billy Hatcher; each frame f:
        Dolphin renders game → XFB (its normal pipeline, native speed)
        Present(): BindBackbuffer(adopted = OUR image) → RenderXFBToScreen renders XFB INTO our image
                   PresentBackbuffer(): GENERAL→GENERAL QFOT release + signal timeline = 2f
   → RetroPark VulkanCompositor: wait timeline 2f → composite overlay over our image → present/readback
                   → signal timeline 2f+1 (release back to Dolphin)
```
Zero extra copy: the XFB→backbuffer render is Dolphin's normal final scale step, retargeted to our
image. Overlay blend happens on RetroPark's device after the timeline handoff.

## 3. Error handling / honesty

- **Multi-GPU / device-UUID mismatch** — Dolphin must run on RetroPark's adapter (cross-adapter shared
  handles don't work, per the D3D11 same-adapter gotcha). Match by `deviceUUID`; if unmatchable →
  error/skip, never a silent wrong-adapter import.
- **External-memory extension unsupported** — probe on Dolphin's device; skip (WARN) if absent, like the
  Slice B Vulkan gate.
- **Timeline discipline** — signal each value at most once, strictly increasing (Slice B's
  `last_present_sync_` lesson); the `GENERAL→GENERAL` QFOT on *both* halves is mandatory — a
  layout-changing cross-device QFOT is invalid and validation can't catch it (Slice B).
- **Dolphin's "Warning" modal** — carry Slice I's `UsePanicHandlers=False` so boot never blocks.
- **Swapchain-bypass invasiveness** — the risk valve: if `IsHeadless()==false`-without-a-real-swapchain
  can't be made clean, fall back to **copy-from-XFB** (hook `after_frame_event`, one `vkCmdCopyImage`
  from the XFB `VKTexture` into our imported image, signal timeline) — pixel-identical, one negligible
  blit. Documented, decided at the wall.
- **Gated + partly manual** — real GPU + Vulkan external-memory + Dolphin build + ROM. Automated gate
  asserts the readback is non-black + changing + shows the overlay blend; the composited title-screen
  PNG is the human proof. Overclaim nothing.
- **Crash honesty unchanged** — in-process this slice, so a Dolphin fault still takes the host down
  (out-of-process isolation is the later slice).

## 4. Testing

- **Gated zero-copy handoff e2e (real-GPU):** set up RetroPark's `VulkanBackend` (export shared image +
  timeline) + `VulkanCompositor`; link `DolphinLib`; boot Billy Hatcher with the external-present patch;
  run ~a few hundred frames; RetroPark composites the overlay and reads back → assert the readback is
  **non-black**, **changes across frames** (real emulation reaching our surface), and shows the **overlay
  blend** in the overlay region (as the Slice B / D3D11 presenting e2es assert blend, not layering).
  `WARN`-skips without a capable GPU / Dolphin build / ROM. Validation-clean on both devices (Slice B
  bar).
- **Human proof:** dump the composited readback to a PNG — the Billy Hatcher title screen, now inside
  RetroPark's surface with a blended overlay. Manual confirmation.
- **Regression:** the A–H suite stays green; Slice B's `refcore_present_vk` e2e still passes (Dolphin is
  an *additional* producer against the unchanged host handoff — no RetroPark-side surface changes
  expected).

## 5. Scope

**In Slice J:** Dolphin-side external-memory/semaphore extensions + device-UUID match; the external
present target (adopt our imported image + timeline); `BindBackbuffer`/`PresentBackbuffer` retarget +
`CommandBufferManager` external-semaphore signal; in-process `DolphinLib` link + boot; the gated
zero-copy handoff e2e; the Dolphin changes captured as a committed `.patch`.

**Explicitly out (later):**
- **Out-of-process crash isolation** (its own slice — the "better libretro" robustness win).
- **The reusable `dolphin_present` `rp_core_abi` core** — content ABI (any ISO), full create/set_surfaces
  /start/stop lifecycle, harness wiring, overlay polish (**Slice K**); Dolphin **audio + input/controllers**
  (Slice K+ follow-ons).
- Copy-from-XFB is the **fallback**, not a parallel deliverable.
- Wii support specifics, multi-game validation, D3D11/D3D12 Dolphin backends (Vulkan only), savestate/
  netplay for Dolphin.

**The single provable claim of Slice J:** *Standalone Dolphin, linked in-process, renders the real Billy
Hatcher ROM directly into RetroPark's exported shared `VkImage` (zero copy) using the Slice B timeline
handoff, and RetroPark's Vulkan compositor blends an overlay and presents it — proven by a gated,
real-GPU e2e asserting a non-black, changing, overlay-blended readback, and shown by the composited
title screen in our surface. In-process, Vulkan, one window, no libretro.*

## 6. Repo additions

```
docs/patches/dolphin-external-present.patch   # all Dolphin-side changes on tag 2606 (reproducible; external/dolphin git-ignored)
docs/dolphin-vulkan-present-notes.md          # CommandBufferManager signal mechanics + external-present wiring (impl notes)
cores/dolphin_present/ (or tests/dolphin/)    # in-process producer vehicle: import shared image+timeline, boot ROM, render into it
tests/                                        # gated zero-copy handoff e2e: Dolphin -> shared image -> composite -> readback assert
```
