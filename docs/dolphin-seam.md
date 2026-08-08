# Dolphin integration seam report (feeds Slice J)

Where standalone Dolphin (pinned tag `2606`, `external/dolphin`) exposes the two seams RetroPark needs:
(A) the **in-process init/boot sequence** to drive Dolphin as an embedded emulator, and (B) the **Vulkan
present point** to retarget from Dolphin's own swapchain into RetroPark's exported shared `VkImage` +
timeline semaphore (the Slice B handoff). All citations are `external/dolphin/...` at tag 2606.

## A. In-process init / boot / frame-count / shutdown

Standalone driving sequence (from `Source/Core/DolphinNoGUI/MainNoGUI.cpp`, the files it calls):

1. `UICommon::SetUserDirectory(path)` then `UICommon::Init()` — `Source/Core/UICommon/UICommon.h:32,15`
   (`MainNoGUI.cpp:271-272`).
2. Build a `WindowSystemInfo` (`Source/Core/Common/WindowSystemInfo.h:18-46`). For the eventual
   shared-surface path we will NOT use `WindowSystemType::Headless` (it makes `Present()` early-return —
   see §B); we provide a surface/hooked present instead.
3. `UICommon::InitControllers(wsi)` — `UICommon.h:22` (`MainNoGUI.cpp:273`).
4. `BootParameters::GenerateFromFile(rom_path, BootSessionData(...))` — `Source/Core/Core/Boot/Boot.h`
   (`MainNoGUI.cpp:229-230`) — pass the `.rvz`.
5. `BootManager::BootCore(Core::System&, std::unique_ptr<BootParameters>, const WindowSystemInfo&) -> bool`
   — `Source/Core/Core/BootManager.h:17-18`; internally calls `Core::Init(system, boot, wsi)`
   (`Core.h:123`, `BootManager.cpp:192,200`). `system = Core::System::GetInstance()`.
6. **Emulation runs on its own threads** — `s_emu_thread` (`Core.cpp:99,249`) + a `gpu_thread` in
   dual-core (`Core.cpp:472`). `Init` returns immediately; the CPU/GPU run async.
7. Host thread pumps `Core::HostDispatchJobs(system)` in a loop (`Core.h:184`; used in both
   `PlatformHeadless::MainLoop` and `PlatformWin32::MainLoop`).
8. **"Run N frames":** no built-in primitive — register a callback on
   `GetVideoEvents().after_frame_event` (per GPU XFB copy) or `.after_present_event` (per presentation,
   carries `PresentInfo` with `frame_count`/`present_count`) — `Source/Core/VideoCommon/VideoEvents.h:80,94,17-65`.
   Count to N, then `Core::Stop(system)` (`Core.h:124`).
9. **Grab a frame** (Slice I used framedump-to-PNG; Slice J wants zero-copy): hook
   `after_present_event`/`after_frame_event` and read the `VKTexture*` behind the current XFB entry —
   the same texture `FrameDumper::DumpCurrentFrame` uses (`Source/Core/VideoCommon/FrameDumper.cpp:41-71`).
10. Shutdown: `Core::Stop(system)` → `Core::Shutdown(system)` (`Core.h:124,125`) →
    `UICommon::ShutdownControllers()`/`UICommon::Shutdown()` (`UICommon.h:23,16`).

## B. Vulkan present seam (the retarget point)

**Swapchain owner / final image:** `Vulkan::SwapChain` — `Source/Core/VideoBackends/Vulkan/VKSwapChain.h:21-114`
(holds `VkSwapchainKHR`, per-image `VKTexture`+`VKFramebuffer`; `GetCurrentTexture/Framebuffer`,
`AcquireNextImage`).

**Acquire → render → present:**
- `VKGfx::BindBackbuffer()` — `VideoBackends/Vulkan/VKGfx.cpp:220-307` — `m_swap_chain->AcquireNextImage()`
  (line 240), transition to `COLOR_ATTACHMENT_OPTIMAL`, bind `GetCurrentFramebuffer()`.
- `VKGfx::PresentBackbuffer()` — `VKGfx.cpp:309-335` — transition to `PRESENT_SRC_KHR`, then
  `g_command_buffer_mgr->SubmitCommandBuffer(true, false, true, m_swap_chain->GetSwapChain(),
  image_index)` (lines 325-326) — the `vkQueuePresentKHR` call site (inside
  `CommandBufferManager::SubmitCommandBuffer`).
- Both invoked from `VideoCommon::Presenter::Present()` — `Source/Core/VideoCommon/Present.cpp:906-969`
  (`BindBackbuffer` → `RenderXFBToScreen` draws the XFB into the acquired image → `PresentBackbuffer`).
  **`Present()` early-returns when `g_gfx->IsHeadless()`** (`Present.cpp:906`) — why pure headless dumps
  nothing (Slice I §3.2 in `dolphin-build.md`).

**Device/queue:** `VulkanContext::CreateDevice(surface, enable_validation)` —
`VideoBackends/Vulkan/VulkanContext.cpp:688-831` (`vkCreateDevice` at 813; queues at 825,828); factory
`VulkanContext::Create(...)` at 584-610; called from `VKMain.cpp:169-171`.

**⚠️ External-memory extensions are NOT enabled today.** `VulkanContext::SelectDeviceExtensions()`
(`VulkanContext.cpp:612-675`) enables only swapchain/fullscreen-exclusive/props2/memory-budget/depth-clamp
— **no `VK_KHR_external_memory*` / `VK_KHR_external_semaphore*`**. Slice J must add these there (and to
instance-extension selection near `VulkanContext.cpp:135-137,347,380`) to import RetroPark's shared image
+ timeline.

### Two retarget strategies for Slice J

1. **Render-into-external-image (true zero-copy, preferred).** `VKTexture::CreateAdopted(config, VkImage,
   view_type, layout)` — `VideoBackends/Vulkan/VKTexture.h:60-63` — *already exists* and is how each
   swapchain image becomes a `VKTexture` (so "adopt a VkImage I don't own the memory of" is already
   plumbed). Wrap our imported shared `VkImage` with `CreateAdopted` + `VKFramebuffer::Create`
   (`VKTexture.h:155-157`); in a modified `BindBackbuffer`, skip `AcquireNextImage` and bind that
   framebuffer; in a modified `PresentBackbuffer`, skip `vkQueuePresentKHR` and instead **signal our
   imported timeline semaphore** after `RenderXFBToScreen` writes the adopted image
   (`CommandBufferManager::SubmitCommandBuffer` needs an extra "signal this semaphore" arg).
2. **Copy-from-XFB (simpler, one extra blit).** Stay windowless-ish and, on `after_present_event`/
   `after_frame_event`, `vkCmdCopyImage`/blit from the internal XFB `VKTexture` (source of
   `FrameDumper::DumpCurrentFrame`, `FrameDumper.cpp:41-71`) into our imported `VkImage`, signalling the
   external semaphore ourselves — no changes to `BindBackbuffer`/`PresentBackbuffer`.

Strategy 1 matches the presenting thesis (Dolphin renders *into* our surface, zero copy); strategy 2 is a
lower-risk first step. Slice J decides.

### Follow-up reads before writing Slice J code
- `CommandBufferManager.cpp` — exact `vkQueuePresentKHR` + submit/signal mechanics (to add an
  external-semaphore signal).
- `VulkanContext::SelectInstanceExtensions` full body — where to add the external-memory-capable
  instance extensions.
- How Dolphin picks its `VkPhysicalDevice` (`VulkanContext.cpp`) — Slice J must match RetroPark's
  `deviceUUID` (Slice B) so the imported image is on the same adapter.
