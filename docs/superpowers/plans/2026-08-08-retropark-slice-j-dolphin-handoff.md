# RetroPark Slice J — Dolphin zero-copy handoff Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax. **This slice is deep surgery on a third-party codebase (Dolphin) + cross-device Vulkan sync — much of it is runtime-discovery (validation errors, sync bugs surface only on the real GPU). Drive the surgery tasks with tight build/run/validate iteration, not blind transcription.**

**Goal:** Standalone Dolphin, linked in-process, renders Billy Hatcher directly into RetroPark's exported shared `VkImage` (zero copy) via the Slice B timeline handoff; RetroPark's Vulkan compositor blends an overlay and reads it back.

**Architecture:** Dolphin's Vulkan backend is patched so a **fake `SwapChain`** wraps our imported external `VkImage` (keeps `IsHeadless()==false`, reuses all of `BindBackbuffer`/`PresentBackbuffer`); its submit signals our imported **timeline semaphore** at `2f` instead of `vkQueuePresentKHR`. RetroPark's `VulkanBackend` (Slice B) exports the image+timeline and `composite_and_present` consumes at `2f`, signals `2f+1`. In-process, one process, two `VkDevice`s.

**Tech Stack:** C++17/MSVC, Vulkan (`VK_KHR_external_memory_win32` / `external_semaphore_win32` / `timeline_semaphore`), Dolphin tag 2606 (`external/dolphin`, git-ignored), RetroPark Slice B `VulkanBackend`/`VulkanCompositor`, doctest.

## Global Constraints

- Dolphin source changes live in `external/dolphin` (git-ignored) and MUST be captured as a committed **`docs/patches/dolphin-external-present.patch`** (`git -C external/dolphin diff` against tag 2606) — reproducibility.
- **The producer's `VkImageCreateInfo` must byte-match the host's** (`VulkanBackend.cpp:241-268`): `VK_FORMAT_R8G8B8A8_UNORM`, extent `{w,h,1}`, mips 1, layers 1, samples 1, `VK_IMAGE_TILING_OPTIMAL`, usage `TRANSFER_DST|TRANSFER_SRC|COLOR_ATTACHMENT|SAMPLED`, `VkExternalMemoryImageCreateInfo{OPAQUE_WIN32}` — or the import is invalid.
- **Timeline protocol (Slice B, verbatim):** producer signals `2f`; host waits `2f`, signals `2f+1`; the shared image stays **GENERAL** for its whole life; the cross-device QFOT is **GENERAL→GENERAL** on BOTH halves (a layout-changing cross-device QFOT is invalid and validation can't catch it). Signal each timeline value at most once, strictly increasing.
- **Carry Slice I's runtime flags:** `-C Dolphin.Interface.UsePanicHandlers=False` (else a modal "Warning" blocks boot). Build via `DolphinNoGUI.vcxproj` recipe in `docs/dolphin-build.md` (dash MSBuild switches; `-p:SolutionDir=...Source\`).
- Real-GPU gated (RTX 5080 here); WARN-skip via `VulkanBackend::probe_vulkan_shared()`. Validation-clean on both devices (Slice B bar). C++17, warning-clean. No `strncpy`. Conventional commits. **NO AI attribution.** The A–H suite + Slice B `refcore_present_vk` e2e stay green (RetroPark host code is unchanged — Dolphin is an additional producer).
- No core-ABI change (ABI stays v5).

---

## Key facts from recon (cite these; don't re-derive)

**RetroPark host (reuse as-is, `src/render/vulkan/VulkanBackend.*`):**
- `VulkanBackend::allocate_surfaces(count,w,h,std::vector<rp_surface_desc>& descs,err)` → exports image ring + timeline; each `descs[i].shared_handle` (OPAQUE_WIN32 memory handle), `.format=RP_FMT_R8G8B8A8_UNORM`. `present_sync_handle()` (timeline NT handle), `present_device_uuid(uint8_t[16])`.
- `composite_and_present(uint32_t ready_index, uint64_t sync_value, bool has_frame, uint8_t* out_rgba, err)` → waits `sync_value`, QFOT-acquires GENERAL→GENERAL, composites overlay (blue @0.5α, top-left quadrant, `VulkanCompositor.cpp:231-233`), signals `sync_value+1`, reads back full `w*h` RGBA8 into `out_rgba` (headless only — `native_window=nullptr`).
- Template test to mirror: `tests/test_vulkan_compositor.cpp` (in-process producer `vk_test_producer_clear` at :55-204, drive+assert at :208-240). Gate: `probe_vulkan_shared()`. `test_vulkan_handoff.cpp:54` shows the `protected`-member test-subclass escape hatch.

**Dolphin (surgery, `external/dolphin/Source/Core/VideoBackends/Vulkan/`):**
- `VulkanContext::SelectDeviceExtensions` (`VulkanContext.cpp:612-675`) + `SelectInstanceExtensions` (:273-396) use an `AddExtension(name, required)` lambda — add the external-memory/semaphore + props2 + timeline extensions there (NONE enabled today). Device features: `device_info.pNext` is never set (:753-830) — must chain `VkPhysicalDeviceVulkan12Features{timelineSemaphore=VK_TRUE}` (query support via `vkGetPhysicalDeviceFeatures2` first).
- Adapter selection is by index `g_Config.iAdapter` at `VKMain.cpp:47-51` and `:161-171` — add a `deviceUUID` match (query `VkPhysicalDeviceIDProperties` via `vkGetPhysicalDeviceProperties2`) at both sites.
- Swapchain built at `VKMain.cpp:199-221` (gated `surface != VK_NULL_HANDLE`) → the injection point. `SwapChain::Create(wsi,surface,vsync)` (`VKSwapChain.cpp:131-139`); `SetupSwapChainImages` (:407-463) already does `VKTexture::CreateAdopted(cfg,image,view,UNDEFINED)` (:445) + `VKFramebuffer::Create(tex,nullptr,{})` (:452). `TextureConfig(w,h,1,layers,1,fmt,AbstractTextureFlag_RenderTarget,Texture_2DArray)`.
- `VKGfx m_swap_chain` is `unique_ptr<SwapChain>` (`VKGfx.h:99`); `IsHeadless()` = `m_swap_chain==nullptr` (`VKGfx.cpp:45-48`). `BindBackbuffer` (:220-307) transitions to `COLOR_ATTACHMENT_OPTIMAL`; `PresentBackbuffer` (:309-335) transitions `PRESENT_SRC_KHR` + `SubmitCommandBuffer(true,false,true,swapchain,image_index)`.
- Submit choke point: `CommandBufferManager::SubmitCommandBuffer` (`CommandBufferManager.cpp:392-472`) — binary semaphores only, `submit_info.pNext=nullptr` (:401), `vkQueuePresentKHR` at :451. Convert the aggregate-init to field assignment to chain `VkTimelineSemaphoreSubmitInfo` + add the external `VkSemaphore` to `pSignalSemaphores`.
- `AbstractTextureFormat::RGBA8` → `VK_FORMAT_R8G8B8A8_UNORM` (matches host).

---

## Task 1: Dolphin device bring-up — external-memory/semaphore/timeline exts + feature + deviceUUID match

**Files (all in `external/dolphin/Source/Core/VideoBackends/Vulkan/`):** `VulkanContext.cpp/.h`, `VKMain.cpp`.

**Interfaces produced:** Dolphin's `VkDevice` created with the external-memory/semaphore + timeline extensions and `timelineSemaphore` feature enabled, on the physical device whose `deviceUUID` matches a target 16-byte UUID (config-provided). A new `VulkanContext` accessor for the chosen device's UUID.

**Deliverable/verification:** Dolphin still boots Billy Hatcher on Vulkan (Slice I windowed-framedump path) with the modified device — proving the added extensions/feature/selection don't break normal operation. This is the de-risk before any present surgery.

- [ ] **Step 1: Add the instance extensions** in `VulkanContext::SelectInstanceExtensions` (after the existing `AddExtension(...props2...)` at ~:379): `AddExtension(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,false); AddExtension(VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,false);` and make `VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME` **required** (change its `false`→`true` at :379) since UUID query + external-memory caps depend on it.

- [ ] **Step 2: Add the device extensions** in `VulkanContext::SelectDeviceExtensions` (after :662): `AddExtension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,false); AddExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,false); AddExtension(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,false); AddExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,false); AddExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,false);` Record which succeeded (an `m_external_present_supported` bool) so Task 2 can gate.

- [ ] **Step 3: Enable the `timelineSemaphore` feature** in `VulkanContext::CreateDevice` (`VulkanContext.cpp:753-830`): before `vkCreateDevice`, build `VkPhysicalDeviceTimelineSemaphoreFeaturesKHR tsf{...,.timelineSemaphore=VK_TRUE};` query it via `VkPhysicalDeviceFeatures2` + `vkGetPhysicalDeviceFeatures2`; if supported, set `device_info.pNext = &tsf;` (currently never set). Guard so a device lacking it still builds (external-present disabled).

- [ ] **Step 4: Add deviceUUID selection.** In `VulkanContext` add `static int FindDeviceIndexByUUID(const std::vector<VkPhysicalDevice>&, const uint8_t uuid[16])` (query `VkPhysicalDeviceIDProperties` via `vkGetPhysicalDeviceProperties2`; return matching index or -1) and an accessor `const uint8_t* GetDeviceUUID() const`. At `VKMain.cpp:47-51` and `:161-171`, when a target UUID is configured (see Step 5), select that index instead of `g_Config.iAdapter`.

- [ ] **Step 5: Config plumbing for the target UUID.** The external present target (Task 2) needs (a) a target device UUID and (b) the shared image + timeline handles. Add a small process-global struct `RetroParkExternalPresent { bool enabled; uint8_t device_uuid[16]; void* image_handle; void* timeline_handle; uint32_t width, height; }` in a new `external/dolphin/Source/Core/VideoBackends/Vulkan/RetroParkExternal.h/.cpp` (a singleton the in-process host sets before boot). Task 1 only uses `enabled` + `device_uuid` for selection; Task 2 uses the handles.

- [ ] **Step 6: Build + regression-run.** Rebuild `DolphinNoGUI` (recipe: `docs/dolphin-build.md`). With `RetroParkExternalPresent::enabled=false` (default), run the Slice I framedump command — confirm Billy Hatcher still renders (non-black, changing framedump). Expected: unchanged behavior; the device just has extra extensions available.

- [ ] **Step 7: Commit** (capture the patch after each Dolphin task): `git -C external/dolphin diff > docs/patches/dolphin-external-present.patch` in the RetroPark repo, then `git add docs/patches/dolphin-external-present.patch && git commit -m "feat(dolphin): external-memory/semaphore/timeline exts + deviceUUID selection (Slice J task 1)"`.

---

## Task 2: External-present target — fake SwapChain wrapping our imported VkImage

**Files:** `external/dolphin/.../VKSwapChain.h/.cpp` (external-image mode), `VKMain.cpp` (injection at :199-221), `RetroParkExternal.*` (import helpers).

**Interfaces:** `SwapChain::CreateExternal(const RetroParkExternalPresent&, VkFormat) -> unique_ptr<SwapChain>` that imports the external `VkImage` (matching the host's create-info byte-for-byte) + the timeline semaphore, wraps the image with `CreateAdopted`+`VKFramebuffer`, and provides `GetCurrentTexture/Framebuffer/ImageIndex` like the real swapchain — but `AcquireNextImage()` is a no-op returning `VK_SUCCESS` (single fixed image; ring index handling deferred — use slot 0, count 1 for the first proof). Exposes the imported `VkSemaphore timeline` + a `uint64_t next_signal_value()`.

- [ ] **Step 1:** In `SwapChain`, add an `m_external` bool + the imported `VkImage m_ext_image`, `VkDeviceMemory m_ext_memory`, `VkSemaphore m_ext_timeline`, and reuse the existing `SwapChainImage` vector (one entry). `CreateExternal`: build the byte-matching `VkImageCreateInfo` (Global Constraints) with `VkExternalMemoryImageCreateInfo{OPAQUE_WIN32}`, `vkCreateImage`; import memory via `VkMemoryDedicatedAllocateInfo{image}` → `VkImportMemoryWin32HandleInfoKHR{OPAQUE_WIN32, handle=image_handle}` → `vkAllocateMemory` → `vkBindImageMemory`; wrap with `VKTexture::CreateAdopted(cfg, m_ext_image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_LAYOUT_UNDEFINED)` + `VKFramebuffer::Create(tex,nullptr,{})`. Import the timeline: create `VkSemaphore`(TIMELINE) then `vkImportSemaphoreWin32HandleKHR{OPAQUE_WIN32, handle=timeline_handle}` (resolve the fn via `vkGetDeviceProcAddr`, mirror `RefCoreVk.cpp:260-262,330-340`).

- [ ] **Step 2:** `CreateSwapChain()`/`SetupSwapChainImages()`/`AcquireNextImage()`/`GetSwapChain()` guard on `m_external`: skip `vkCreateSwapchainKHR`/`vkGetSwapchainImagesKHR`; `AcquireNextImage` returns `VK_SUCCESS`; `GetSwapChain()` returns `VK_NULL_HANDLE` (so present code can detect external mode). `IsCurrentImageValid()` returns true.

- [ ] **Step 3:** Inject at `VKMain.cpp:199-221`: when `RetroParkExternalPresent::enabled`, set `surface = VK_NULL_HANDLE` (no real surface) but build `swap_chain = SwapChain::CreateExternal(ext, VK_FORMAT_R8G8B8A8_UNORM)` instead of the headless `nullptr`. So `IsHeadless()` stays **false** (swap_chain non-null) and `Present()` runs the present path. Command-buffer manager `swapchain_image_count` = 1 in this mode.

- [ ] **Step 4: Build + smoke.** Rebuild DolphinNoGUI. Task 2 can't fully verify alone (consuming is Task 4) — smoke: with a *host-allocated* image handle wired in via a throwaway harness (or defer the run to Task 4). At minimum confirm Dolphin boots without validation errors up to the first `BindBackbuffer` (add a temporary log). Expected: device+import succeed, image adopted.

- [ ] **Step 5: Commit** (refresh the patch): `git -C external/dolphin diff > docs/patches/dolphin-external-present.patch`; `git commit -am "feat(dolphin): external-image fake-swapchain present target (Slice J task 2)"`.

---

## Task 3: Present-path signal — GENERAL transition + QFOT release + timeline signal (skip vkQueuePresentKHR)

**Files:** `external/dolphin/.../VKGfx.cpp` (`PresentBackbuffer`), `CommandBufferManager.h/.cpp` (external-timeline signal).

**Interfaces:** `CommandBufferManager::SubmitCommandBuffer(...)` gains an optional `{VkSemaphore ext_timeline; uint64_t signal_value;}` (default none). When set, the submit's `VkSubmitInfo` includes the external timeline in `pSignalSemaphores` with a chained `VkTimelineSemaphoreSubmitInfo` (value = `signal_value`), and — in external-present mode — **skips `vkQueuePresentKHR`**.

- [ ] **Step 1:** In `PresentBackbuffer` (`VKGfx.cpp:309-335`), when `m_swap_chain->GetSwapChain()==VK_NULL_HANDLE` (external mode): replace the `PRESENT_SRC_KHR` transition with a transition of the current image `COLOR_ATTACHMENT_OPTIMAL → GENERAL` (own device), then record a **QFOT release** barrier `GENERAL→GENERAL, srcQueueFamilyIndex=graphics_qfam, dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL_KHR` (mirror `RefCoreVk.cpp:198-208`), then call `SubmitCommandBuffer(true,false,true, /*ext_timeline=*/m_swap_chain->GetTimeline(), /*signal_value=*/2*frame)` — no swapchain/image-index. `frame` is a monotonic counter owned by the external SwapChain (`next_signal_value()` returns `2*frame` and increments).

- [ ] **Step 2:** In `CommandBufferManager::SubmitCommandBuffer` (`CommandBufferManager.cpp:392-472`): convert the `VkSubmitInfo` aggregate-init (:400-408) to field assignment; when an external timeline + value are supplied, set `signalSemaphoreCount` to include it (alongside/instead of `m_present_semaphores[...]` — in external mode there's no present semaphore, so just the timeline), and chain `VkTimelineSemaphoreSubmitInfo{.signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&value}` via `submit_info.pNext`. Skip the `vkQueuePresentKHR` block when `present_swap_chain==VK_NULL_HANDLE` (already the case) — the external path just needs the signal added to the submit.

- [ ] **Step 3:** Thread the external-timeline args through the public + private `SubmitCommandBuffer` overloads and the worker-thread submit queue struct (`CommandBufferManager.cpp:330-335`) so worker-thread submits carry the signal too.

- [ ] **Step 4: Build.** Rebuild DolphinNoGUI. Full verification is Task 4 (needs the host consuming). Confirm it compiles + boots to first present without validation errors (temporary logging).

- [ ] **Step 5: Commit** (refresh patch): `git commit -am "feat(dolphin): external-present submit signals RetroPark timeline, skips vkQueuePresentKHR (Slice J task 3)"`.

---

## Task 4: In-process host vehicle — link DolphinLib, export surfaces, boot ROM, drive compositor

**Files (RetroPark repo):** a new gated test target `tests/dolphin/test_dolphin_handoff.cpp` + its `CMakeLists` wiring (links `DolphinLib` from `external/dolphin/Build/x64/Release/DolphinLib/bin/DolphinLib.lib` + RetroPark libs); a small `tests/dolphin/dolphin_host.{h,cpp}` driving Dolphin's init.

**Interfaces:** a host routine that (a) constructs `VulkanBackend host; host.initialize(nullptr,W,H,err); host.allocate_surfaces(1,W,H,descs,err)`; (b) fills `RetroParkExternalPresent{enabled=true, device_uuid=host.present_device_uuid(), image_handle=descs[0].shared_handle, timeline_handle=host.present_sync_handle(), W,H}` before Dolphin init; (c) boots Billy Hatcher via `UICommon::Init`→`WindowSystemInfo{Headless}`→`BootManager::BootCore` (seam report `docs/dolphin-seam.md`); (d) as Dolphin renders, hooks `after_present_event`/`after_frame_event` to learn the frame count `f`, then calls `host.composite_and_present(0, 2*f, true, out.data(), err)` and returns the readback.

- [ ] **Step 1: CMake — gated Dolphin test target.** Add `tests/dolphin/` as a SEPARATE test executable (do NOT bloat `retropark_tests` with all of Dolphin). Link `DolphinLib.lib` + its transitive Externals `.lib`s (from `external/dolphin/Build/x64/Release/*/bin/`) + RetroPark's vulkan render lib. Guard the whole target behind a CMake option `RP_ENABLE_DOLPHIN` (default OFF) so the normal build/CI is unaffected; document enabling it. If linking the full transitive Externals set proves impractical, fall back to a thin `dolphin_present.dll` (built beside DolphinNoGUI) that the test `LoadLibrary`s — decide at the wall and note it.

- [ ] **Step 2: Boot glue** (`dolphin_host.cpp`): implement the init sequence (seam report §A) with `-C Dolphin.Interface.UsePanicHandlers=False` applied via `Config::SetBase` before boot; set `RetroParkExternalPresent` before `BootCore`; register an `after_present_event` callback incrementing a frame counter + storing the latest `f`.

- [ ] **Step 3: Coordination.** Dolphin renders on its own thread signalling `2f`; the host thread must call `composite_and_present(0, 2*f, ...)` only for a frame Dolphin has signalled (`sync_value` the host waits on must have been signalled or the 1s fence times out). Drive: after boot, spin until the frame counter ≥ some N (past boot/black), then for a few sampled frames call `composite_and_present(0, 2*f_seen, true, out, err)` where `f_seen` is a frame the callback has confirmed signalled. Respect the Slice B `last_present_sync_` guard (monotonic sync_value). Use count=1 (single shared image) for this first proof — Dolphin re-renders into the same image each frame; sample between frames.

- [ ] **Step 4: Build + run (gated).** Enable `RP_ENABLE_DOLPHIN`, build, run the target. This is the integration bring-up — expect Vulkan validation iteration (cross-device QFOT, timeline values, image create-info match). Deliverable: `composite_and_present` returns a **non-black** readback that **changes** across sampled frames.

- [ ] **Step 5: Commit** the RetroPark-side vehicle (test + host glue + CMake) — NOT the Dolphin tree. `git commit -m "feat: in-process Dolphin producer vehicle into RetroPark shared VkImage (Slice J task 4)"`.

---

## Task 5: Gated zero-copy handoff e2e + overlay-blend proof + patch capture

**Files:** `tests/dolphin/test_dolphin_handoff.cpp` (the assertions), `docs/patches/dolphin-external-present.patch` (final), `docs/dolphin-vulkan-present-notes.md`.

- [ ] **Step 1: The gated e2e** (mirror `tests/test_vulkan_compositor.cpp:208-240`): gate on `VulkanBackend::probe_vulkan_shared()` AND the Dolphin build + ROM present (WARN-skip otherwise). Run the Task-4 vehicle for ~a few hundred emulated frames; sample readbacks. **Assert:** (a) a sampled readback is **non-black** (a spread of pixel values, not all ~0); (b) two readbacks at different frame counts **differ** (real emulation reaching our surface); (c) the **overlay blend** is present — top-left quadrant pixels are shifted by the compositor's blue @0.5α overlay vs the same-content bottom-right (mirror the blend-proof at `test_vulkan_compositor.cpp:233-239`, adapted: compare top-left vs bottom-right of the *composited* Dolphin frame — the overlay must measurably raise B / alter the region).

- [ ] **Step 2: Human proof.** Dump one composited readback to `scratchpad/dolphin_composited.png`; confirm the Billy Hatcher title screen is visible **inside RetroPark's surface with the overlay quad blended** in the corner. (Manual — the automated gate is (a)-(c).)

- [ ] **Step 3: Finalize the Dolphin patch + notes.** `git -C external/dolphin diff > docs/patches/dolphin-external-present.patch` (all Task 1-3 changes); write `docs/dolphin-vulkan-present-notes.md` (the fake-swapchain + timeline-signal + QFOT specifics, for Slice K / future maintenance).

- [ ] **Step 4: Regression.** Confirm the A–H suite + Slice B `refcore_present_vk` e2e (`test_vulkan_compositor.cpp`, `test_vulkan_e2e.cpp`) still pass (RetroPark host untouched). `git status`: no cores/ROMs/Dolphin-tree committed.

- [ ] **Step 5: Commit.** `git commit -m "feat: gated Dolphin zero-copy handoff e2e + overlay-blend proof + patch (Slice J task 5)"`.

---

## Self-Review (author checklist, completed)

**Spec coverage:** external-mem/semaphore/timeline exts + deviceUUID (T1) ✓; external present target / fake swapchain adopting our imported image (T2) ✓; PresentBackbuffer GENERAL+QFOT+timeline-signal skipping vkQueuePresentKHR + CommandBufferManager (T3) ✓; in-process DolphinLib link + boot + drive compositor (T4) ✓; gated zero-copy e2e non-black+changing+overlay-blend + PNG (T5) ✓; Dolphin `.patch` capture (T1/T5) ✓; Slice B protocol 2f/2f+1, GENERAL/QFOT (Global + T3/T4) ✓; regression A–H + refcore_present_vk (T5) ✓.

**Risk valve (from spec §3):** if the fake-swapchain / `IsHeadless()==false`-without-surface can't be made validation-clean, fall back to **copy-from-XFB** (hook `after_frame_event`, `vkCmdCopyImage` the XFB `VKTexture` → our imported image, signal timeline) — pixel-identical, one negligible blit, no `BindBackbuffer`/`PresentBackbuffer` surgery. Decide at the Task-2/3 wall; note it in the report; the e2e (T5) is unchanged either way.

**Known runtime-discovery seams (flagged, not placeholders):** cross-device QFOT + timeline values are validated on the real GPU (Task 4 iteration); the DolphinLib transitive-link vs thin-`dolphin_present.dll` decision (T4 step 1); the single-image (count=1) simplification for the first proof (ring/multi-slot deferred). These are integration realities on a third-party codebase, with the fallback and the exact API facts (recon) in hand.
