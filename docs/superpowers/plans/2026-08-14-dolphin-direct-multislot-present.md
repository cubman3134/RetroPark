# Dolphin direct multi-slot present (full-speed rp_core) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development to execute
> task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Run the Dolphin presenting core at ~100% real-time (like standalone Dolphin), by having Dolphin
render **directly into the host's N shared images** and signal the timeline in its **own** submit — deleting
the copy-from-XFB producer, its second queue submit, and the extra blit.

**Architecture:** Extend the existing single-image external-swapchain boot path
(`VKSwapChain::CreateExternal` / `PresentBackbuffer` external branch) to **N images** (one per host ring
slot). `AcquireNextImage` round-robins the slot and **waits on the timeline for the host to free it** (a
real swapchain acquire). `PresentBackbuffer` already submits+signals via `CommandBufferManager` (its
`rp_signal_semaphore`/`rp_signal_value` path); add a per-slot `submit_frame(slot, gen, value)` for rp_core
mode. The rp_core path boots Dolphin on this external swapchain instead of the XFB producer. With a single
submitter (Dolphin's own worker), **GFX_BACKEND_MULTITHREADING can be TRUE** with no queue-race lock.

**Why:** measured — copy-from-XFB + separate submit tops out ~87–90% regardless of overlap/async/threading
(the copy + resubmit is structural overhead standalone never pays, and it's what races the video thread).
Direct-render removes both the overhead and the race.

## Global Constraints
- `rp_core_abi` stays v5 (host side already reads `latest_ready` by slot; ring is already `SurfaceRing{3}`).
- Both the harness AND EverythingBox drive the same vehicle — do not regress the existing NES/GC paths.
- Verify speed with the audio clock: `rp_runtime_audio_stats` frames/30s ÷ 48000 ≈ real-time %. Target ≥ ~98%.
- Verify the WINDOW renders correctly (PrintWindow→PNG); verify NO GPU race (run 60s, no crash, no corruption).
- Keep the vehicle's single-image boot path (`rp_dolphin_boot`) working (it uses `image_handle`/slot 0).

---

### Task 1: N-image external swapchain
**Files:** `external/dolphin/.../VideoBackends/Vulkan/VKSwapChain.{cpp,h}`
- `SetupExternalImageAndTimeline`: loop `ext.slot_count` times, importing `ext.image_handles[i]` into a
  `SwapChainImage` (image + `VKTexture::CreateAdopted` + `VKFramebuffer`), pushed into `m_swap_chain_images`.
  Store `m_external_slot_count`. (Today it imports exactly one.)
- Keep the timeline import as-is (single shared timeline).
- Deliverable: N adopted swap images; `GetCurrentImage()/GetCurrentTexture()` index by
  `m_current_swap_chain_image_index`.

### Task 2: Round-robin acquire that waits for the host to free the slot
**Files:** `VKSwapChain.cpp` `AcquireNextImage()` external branch (~668-696)
- `m_current_swap_chain_image_index = (m_current_swap_chain_image_index + 1) % m_external_slot_count;`
- Before returning, if `present_frame_counter >= slot_count`, `vkWaitSemaphores` on the external timeline
  for value `2*(present_frame_counter - slot_count) + 3` (host's consume signal for the frame that last used
  this slot), 1s timeout. This is the back-pressure that both bounds Dolphin to `slot_count-1` frames ahead
  AND gives the produce/consume overlap → full speed. (Mirrors the producer's relaxed wait, moved to acquire.)
- Deliverable: Dolphin blocks in acquire only when it laps the host; otherwise free-runs.

### Task 3: Per-slot submit_frame in present
**Files:** `VKGfx.cpp` `PresentBackbuffer()` external branch (~320-345)
- After the existing `SubmitCommandBuffer(... GetExternalTimeline(), signal_value)` +
  `GetRetroParkLastSignaledValue().store(signal_value)`, in rp_core mode call
  `host.submit_frame(host_ptr, m_swap_chain->GetCurrentImageIndex(), rp_generation, signal_value)`.
- Plumb the `rp_host_iface` + `rp_generation` to where present runs (a Vulkan-backend global set by
  `dp_set_surfaces`, e.g. `GetRetroParkCoreHost()` alongside `GetRetroParkExternalPresent()`), so present can
  notify the ring without a DolphinNoGUI dependency. Ensure `NextExternalSignalValue()` yields `2f+2`.
- Deliverable: each rendered frame lands in the host ring at its true slot.

### Task 4: rp_core boots on the external swapchain; delete the XFB producer
**Files:** `external/dolphin/.../DolphinNoGUI/rp_dolphin.cpp`
- `dp_set_surfaces`: keep filling `ext.image_handles[0..n]`/`slot_count` (done); also stash the `rp_host` +
  `rp_generation` into the new Vulkan-backend global from Task 3.
- Boot the rp_core game so the Vulkan backend uses `SwapChain::CreateExternal(ext)` (the way `rp_dolphin_boot`
  does), NOT a hidden-window swapchain + `XfbProducer`.
- **Delete `struct XfbProducer` and its `after_present_event` hook** entirely (and the now-unused submit lock
  if nothing else needs it — but keep the `GetRetroParkSubmitLock` no-op harmless if the boot path still
  shares the queue with texture uploads; verify).
- `Config::GFX_BACKEND_MULTITHREADING = true` (safe now: Dolphin's worker is the only submitter).
- Keep audio puller + input override (Slices L/M) unchanged — they don't depend on the producer.
- Deliverable: rp_core mode renders via Dolphin's own present into the ring.

### Task 5: Measure + verify
- Harness (Release): `--api vulkan --core external/dolphin/Binary/x64 --content "<GC iso>"`, quoted path.
- Speed: audio frames / 30s ÷ 48000 ≥ ~0.98. Stability: 60s run, no crash. Render: PrintWindow→PNG clean.
- Regression: NES (libretro_shim) + the existing gated tests still pass; EB deploy still renders GC.
- Deliverable: ~100%, stable, clean.

## Notes / risks
- The acquire-wait timeout must not deadlock if the host stops consuming (harness closing) — 1s timeout + a
  disabled/`slot_count==1` fallback that reduces to the old lock-step.
- `VKTexture::CreateAdopted` layout bookkeeping per slot: each slot cycles UNDEFINED→(render)→GENERAL→QFOT
  EXTERNAL, re-acquired next time it's its turn; confirm the layout tracked matches (the boot path already
  does this for one image).
- Host side needs NO change (already multi-slot capable). If `latest_ready` ever returns a slot mid-render,
  the acquire-wait prevents it.
