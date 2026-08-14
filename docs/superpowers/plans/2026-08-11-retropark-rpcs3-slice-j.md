# RPCS3 Slice J — `rpcs3_present` loadable core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development or executing-plans
> to implement task-by-task. Steps use `- [ ]` checkboxes.

**Goal:** Package the Slice-I RPCS3 embed as a loadable `rpcs3_present` RetroPark core (ABI **v7** — was v5 at
authoring; see the SUPERSEDING UPDATE below) that renders into the Runtime's host-owned shared `VkImage`
exactly like `dolphin_present` — which now means multi-slot direct present + consume-timeline back-pressure +
an adaptive `audio_want` feeder, not the single-slot lock-step this plan originally specced.

> **STATUS 2026-08-11: FUNCTIONALLY COMPLETE.** The cross-process zero-copy frame handoff works end-to-end under
> the real Runtime: the producer imports the host's shared VkImage + timeline into RPCS3's VkDevice, per-frame
> blits `get_present_source()` into it (QFOT + even/odd timeline), relays `submit_frame` over the pipe, and the
> host consumes + signals back — the producer advances frame after frame (verified: `pipe=1`, frame counter
> climbing to ~24 then plateauing exactly as LBP stops presenting). Composited frames are black (LBP's
> boot-loading — the documented Slice-I content limitation), so a *visible game* frame awaits LBP progressing
> past its load; the handoff pipeline itself is complete. Tasks 1-4 all done. Remaining polish: BGRA→RGBA
> swizzle (colors swapped), the adapter-UUID force (unneeded on this single-GPU box), and a headless doctest
> gate (can't assert non-empty until LBP renders content).

**Architecture:** A `rpcs3_present.dll` exports `rp_get_core_abi()`; the Runtime loads it, hands it a shared
`VkImage` + external timeline semaphore; the core imports them into RPCS3's own VkDevice (UUID-matched to the
host GPU) and per-frame blits RPCS3's present-source image into the shared image with QFOT + even/odd timeline
lock-step. **Frame-transfer path chosen: A — zero-copy GPU blit.**

---

> # ⚠️ SUPERSEDING UPDATE — Dolphin-parity lessons (2026-08-14)
>
> Everything below was designed 2026-08-11 against the **single-slot lock-step + no-audio** patterns that
> `dolphin_present` also used at the time. On 2026-08-14 those patterns were **replaced** in `dolphin_present`
> after they proved to cap speed at ~87%, tear without real back-pressure, and (once audio landed) crackle.
> RPCS3 is a sibling of *pre-fix* Dolphin, so it needs the same three upgrades **plus a mandatory ABI rebuild**.
> Read this section as the authority; where it conflicts with Tasks 3–4 / Global Constraints below, THIS wins.
> The proven reference is now the FIXED `dolphin_present` (RetroPark commits `6224a55` speed+back-pressure,
> `816899f` audio) — see [[retropark-project]] "Dolphin speed + correctness follow-up" / "audio adaptive feeder".
>
> ## 0. ABI v5 → v7 — REBUILD IS MANDATORY (do this first)
> The header is now `RETROPARK_ABI_VERSION 7u`. The built `rpcs3_present.dll` reports **v5**, and the Runtime's
> loader gates on **strict equality** (`CoreLoader`: `abi_version != RETROPARK_ABI_VERSION → RP_ERR_ABI_MISMATCH`),
> so the current DLL **will no longer load** — the RPCS3 dev loop is blocked until rebuilt. Both source files
> already init `abi_version` from the macro, so a plain rebuild makes it v7. **But rebuilding only fixes
> *loadability*** — the v6/v7 *features* (multi-slot back-pressure, `audio_want`) are the upgrades in §1–§2 and
> must be *implemented*, not just recompiled. `cores/rpcs3_present/core.json` is already bumped to 7 (cosmetic —
> the loader reads the compiled struct, not the JSON).
>
> ## 1. Present — SUPERSEDES Task 3: multi-slot direct present + a consume timeline
> **Old (Task 3 below):** import ONE shared image (`surfaces[0]`), blit into it, single produce timeline —
> producer signals `2f`, waits `2(f-1)+1`; the reuse wait is satisfied by the producer's OWN signal. That is
> exactly what capped Dolphin at ~87% and let it lap the host into a tear.
> **New (match fixed `dolphin_present`):**
> - **N images, round-robin.** The host's `SurfaceRing` is `slot_count = 3`. `set_surfaces` must import **all
>   `set->count` images** (`surfaces[0..count-1].shared_handle`), not just `surfaces[0]`. On present frame `f`,
>   blit into `image[f % slot_count]` and call `submit_frame(host, f % slot_count, generation, 2f+2)`. This lets
>   RPCS3 render frame N+1 while the host still composites N (the overlap that unlocks full speed).
> - **A second, host-owned CONSUME timeline (the real back-pressure).** A single produce timeline *cannot*
>   express "the host finished reading slot X" once the producer also advances it. Read the new field
>   `rp_surface_set.consume_sync_handle` (added in ABI v6) and import it as a second timeline. Before REUSING a
>   slot (i.e. before blitting frame `f` into `image[f % slot_count]`), **wait the consume timeline for
>   `2*(f - slot_count) + 3`** — the value the host signals after its composite's GPU read of that slot
>   completes. RPCS3 never signals the consume timeline, so it cannot self-satisfy the wait → genuine
>   back-pressure, no tear. **Gate the whole two-timeline path on `slot_count >= 2`**; `slot_count == 1` keeps
>   the old single-produce-timeline lock-step (a valid single-image fallback).
> - **Timeline convention:** produce signal `2f+2`, consume wait `2*(f-slot_count)+3` (match `dolphin_present`
>   exactly; drop the old `2f` / `2(f-1)+1` even/odd single-timeline scheme). The producer no longer waits its
>   OWN produce timeline for pacing — the consume timeline is the only back-pressure.
> - **Cross-process wrinkle (RPCS3-specific, RPCS3 is out-of-process, Dolphin is in-process):** the handoff must
>   `DuplicateHandle` **all N image handles + the consume-timeline handle** into the child (extend the
>   `--rp-image`/`--rp-timeline` cmdline passing to a list + the new consume handle). The `submit_frame` pipe
>   relay already carries a slot `index` in `rp_submit_msg` — populate it with `f % slot_count` (it is currently
>   hard-wired to 0). Model the producer on the FIXED `dolphin_present` (`VKSwapChain` multi-slot +
>   `VKGfx::PresentBackbuffer` external branch), NOT `RefCoreVk.cpp` (which is still single-slot).
>
> ## 2. Audio — SUPERSEDES Task 4: build the adaptive `audio_want` feeder from the start
> Do **not** ship a free-running fixed-sleep puller (pull 480 / `sleep(10ms)` / push) — that is precisely the
> Dolphin design that crackled (its supply rate was set by the OS timer, not the 48 kHz playback clock, so the
> host output queue periodically starved). Build it adaptive from day one:
> - The host exposes `rp_host_iface.audio_want(host) → frames the output can accept now` (ABI v7). Pull exactly
>   that many from RPCS3's mixer (null audio backend, like Dolphin's Null + `Mixer::Mix`), and submit it as
>   several **~512-frame chunks** (multiple small buffers stay in flight → overlap; one big buffer leaves the
>   queue one-deep and pops every drain — a bug the Dolphin test caught).
> - **Cross-process wrinkle:** `audio_want`/`audio_sample` live in the DLL's process (it holds `rp_host`), but
>   the mixer is in the child. Cleanest flow: the DLL each tick calls `rp_host.audio_want`, sends that frame
>   count to the child over the pipe; the child pulls exactly that from RPCS3's mixer and relays the s16 stereo
>   frames back; the DLL calls `rp_host.audio_sample`. So the pipe carries a want request (DLL→child) + audio
>   frames (child→DLL) alongside the existing `submit_frame` messages.
>
> ## 3. Data dir — NO change needed (unlike Dolphin)
> RPCS3 already resolves firmware/config from the **`RPCS3_CONFIG_DIR` env var** the host sets — it does *not*
> need Dolphin's `EnsureDolphinSysDir` self-location. The only follow-up is at EB-integration time: EB must set
> `RPCS3_CONFIG_DIR` and provide **PS3 firmware (`dev_flash`)**, which — unlike Dolphin's shippable `Sys` — is
> licensing-sensitive and USER-PROVIDED (point at the user's install; never bundle it).
>
> ## 4. Sequencing — these are NOT RPCS3's first blocker
> RPCS3 still does not render real content (frames are black — LBP boot-loading — and there is a known
> **BGRA→RGBA swizzle** bug, colors swapped). Fix *rendering real content* and the swizzle FIRST; the §1–§2
> parity work is speed/correctness that only matters once a real frame is on screen. Order: **(a) rebuild at v7
> [§0, mandatory to load] → (b) real content + swizzle → (c) multi-slot present [§1] → (d) consume-timeline
> back-pressure [§1] → (e) adaptive audio [§2].** (a) is cheap and unblocks the dev loop; (c)–(e) are the actual
> Dolphin-parity slice.

---

**Tech Stack:** RPCS3-from-source (`rpcs3_lib` closure, Qt 6.10.3 at `D:\Qt\6.10.3\msvc2022_64`), RetroPark
`include/retropark/retropark_abi.h`, Vulkan external memory/semaphore (OPAQUE_WIN32), CMake/VS build.

## Global Constraints

- ABI **v7** (was v5 when this plan was written; the header is now `RETROPARK_ABI_VERSION 7u`). Export symbol
  `rp_get_core_abi`; `abi_version == RETROPARK_ABI_VERSION (7)`. **The v5 DLL no longer loads — rebuild first
  (see the SUPERSEDING UPDATE §0).**
- `external/rpcs3` is git-ignored — the `.patch`/notes + committed `core.json`/docs are the record.
- Reuse the Slice-I `main_application` subclass + the four `run_rpcs3`-bypass fixes (EBOOT-file path, file
  logger, `SetProcessWorkingSetSize`, avoid early CPU readback). Boot the **EBOOT.BIN file**, not the folder.
- Build detached; SDL3-targets delete before any reconfigure; any reconfigure re-dirties LLVM (~15 min).
- No AI attribution in commits/PRs.

---

### Task 1: Core skeleton — DLL exports the ABI, Runtime can load it

**Files:**
- Create: `external/rpcs3/rpcs3/rp_rpcs3/rp_rpcs3_present.cpp` (the loadable core; supersedes the standalone
  `rp_rpcs3.cpp` `main()` — keep `rp_rpcs3.cpp` as the standalone diag harness, add a second target)
- Modify: `external/rpcs3/rpcs3/rp_rpcs3/CMakeLists.txt` (add a `SHARED` target `rpcs3_present` linking
  `rpcs3_lib` + `Qt6::Core`, `RUNTIME_OUTPUT` to a core dir; include RetroPark `include/`)
- Create: `cores/rpcs3_present/core.json` (committed)

**Interfaces:**
- Produces: `const rp_core_abi* rp_get_core_abi(void)` (C export, `extern "C" __declspec(dllexport)`); a
  `kAbi` static `rp_core_abi{ .abi_version=5, .get_info=rp_get_info, .create=rp_create, ... }`.
- Consumes: `include/retropark/retropark_abi.h` (`rp_core_abi`, `rp_core_info`, `rp_host_iface`,
  `rp_surface_set`, `RETROPARK_ABI_VERSION`, `RP_CORE_PRESENTING`, `RP_GFX_VULKAN`).

- [ ] **Step 1: `get_info` + the ABI table + a stub `rp_core`.** Define an opaque `struct rp_core {}` holder;
  `rp_get_info` fills `{abi_version:5, type:RP_CORE_PRESENTING, graphics_api:RP_GFX_VULKAN, id:"rpcs3_present"}`;
  stub `create` (allocate `rp_core`, stash `rp_host_iface`), `destroy` (free), `get_av_info`
  (`sample_rate=48000`, geometry 0), and no-op `set_surfaces`/`start`/`stop`/`load_content` returning `RP_OK`;
  `run_frame=NULL`. `extern "C"` export `rp_get_core_abi` returning `&kAbi`. Mirror `rp_dolphin.cpp`'s `dp_*`.
- [ ] **Step 2: CMake target.** `add_library(rpcs3_present SHARED EXCLUDE_FROM_ALL rp_rpcs3_present.cpp)`,
  `target_link_libraries(rpcs3_present PRIVATE rpcs3_lib Qt6::Core)`,
  `target_include_directories(rpcs3_present PRIVATE <RetroPark>/include)`,
  `set_target_properties(... RUNTIME_OUTPUT_DIRECTORY <build>/cores/rpcs3_present OUTPUT_NAME rpcs3_present)`.
  Delete SDL3 targets, reconfigure (expect LLVM rebuild), build `--target rpcs3_present`.
- [ ] **Step 3: `core.json`.** `{"id":"rpcs3_present","type":"presenting","abi_version":5,
  "graphics_api":"vulkan","entry":"rpcs3_present.dll","requires_content":true}`; AfterBuild/post-build copies it
  next to the DLL (or copy by hand for the gate).
- [ ] **Step 4: Load gate.** Point the Runtime (or a tiny loader test) at `cores/rpcs3_present`; verify
  `CoreLoader` resolves `rp_get_core_abi`, `abi_version==5`, `get_info` returns the manifest values, `create`
  succeeds. Expected: core loads, no `abi_version` mismatch. Fixes: missing exports / lib link errors → mirror
  `rpcs3_lib`'s list; DLL entry (no `main`/`WinMain` needed for a DLL).
- [ ] **Step 5: Commit** (`feat: rpcs3_present core skeleton — exports ABI v5, Runtime loads it`).

### Task 2: Boot RPCS3 under the core (`load_content` + `start`)

**Files:** Modify `rp_rpcs3_present.cpp` (fold in the Slice-I boot logic).

**Interfaces:**
- Consumes: the Slice-I `main_application` subclass + `rp_gs_frame` (moved/shared into this TU), the four
  bypass fixes.
- Produces: `rp_create`→boots nothing yet; `rp_load_content(core, path)` stores the EBOOT path;
  `rp_set_surfaces` stashes `surfaces[0].shared_handle`, `sync_handle`, `device_uuid[16]`, `w/h`, `generation`;
  `rp_start` spins RPCS3 on its own thread (QCoreApplication + our app + `Emu.BootGame(eboot, "", true)`).

- [ ] **Step 1:** Move the Slice-I harness core (the `main_application` subclass, `rp_gs_frame`, `g_headless`,
  logger + `SetProcessWorkingSetSize` bring-up) into `rp_rpcs3_present.cpp` behind an internal
  `start_rpcs3(const std::string& eboot)` that runs on a dedicated `std::thread` (its own `QCoreApplication`).
- [ ] **Step 2:** `rp_load_content` stores the path; `rp_start` launches the thread; `rp_stop`/`rp_destroy`
  signal shutdown (`QCoreApplication::quit` + join). Force renderer=vulkan; **defer the device-UUID match to
  Task 3** (boots on the default adapter for now).
- [ ] **Step 3: Boot gate.** Runtime `load_core` + `load_content(eboot)` + `start`; verify RPCS3 boots LBP
  (log shows `running`, `vk_frames_presented` climbs — reuse the `flip()` counter). Expected: boots like the
  standalone host, just driven by the Runtime. Fix threading/`call_from_main_thread` marshalling as needed.
- [ ] **Step 4: Commit** (`feat: rpcs3_present boots the title under the Runtime (load_content/start)`).

### Task 3: Zero-copy frame transfer (Path A) — the producer

> ⚠️ **Approach SUPERSEDED — see SUPERSEDING UPDATE §1.** The single-image / single-slot lock-step described
> here (import `surfaces[0]`, signal `2f`, wait `2(f-1)+1`) is the pre-fix pattern. Build multi-slot direct
> present (import all N images, round-robin `image[f%slot_count]`, produce-signal `2f+2`) **plus** the
> host-owned consume timeline (`rp_surface_set.consume_sync_handle`, wait `2*(f-slot_count)+3` before slot
> reuse, gated `slot_count>=2`). The rest of this task's import/QFOT/cmd-buffer mechanics still apply per slot.

**Files:** Modify `rp_rpcs3_present.cpp` (add the `Rpcs3XfbProducer` glue on the present seam).

**Interfaces:**
- Consumes: RPCS3's `VkDevice`/queue/queue-family (via `vk::get_current_renderer()` in `Emu/RSX/VK/`),
  `get_present_source()`'s `VkImage`+format+extent (in `VKPresent.cpp`), the imported shared image + timeline.
- Produces: per present, `rp_host.submit_frame(host, 0, generation, 2f+2)`.

- [ ] **Step 1: Device-UUID match.** Before boot, force RPCS3's Vulkan adapter to the one whose
  `VkPhysicalDeviceIDProperties.deviceUUID == set->device_uuid` (RPCS3 `render_creator`/`Emu.SetDefaultGraphicsAdapter`
  by name; map UUID→name). Fallback log if no match.
- [ ] **Step 2: Import (lazy, first present).** On RPCS3's device: re-create a byte-identical
  `VkImageCreateInfo` (R8G8B8A8_UNORM, `TRANSFER_DST|TRANSFER_SRC|COLOR_ATTACHMENT|SAMPLED`, EXCLUSIVE,
  `VkExternalMemoryImageCreateInfo` OPAQUE_WIN32), dedicated-import `shared_handle`
  (`VkImportMemoryWin32HandleInfoKHR`), bind; `vkImportSemaphoreWin32HandleKHR(sync_handle)`; own cmd
  pool/buffer/fence. Do NOT close the host's handles. (Mirror `XfbProducer::Init` in the Dolphin patch.)
- [ ] **Step 3: Per-frame producer** on our `rp_gs_frame::flip`/the VKPresent seam: wait timeline `2f+1`
  (frame>0); barrier present-source→`TRANSFER_SRC`, shared `UNDEFINED→GENERAL`; `vkCmdBlitImage`
  present-source→shared (BGRA→RGBA via the blit's format handling / component-swizzled copy); QFOT release
  shared `GENERAL→GENERAL` `qfam→VK_QUEUE_FAMILY_EXTERNAL_KHR`; submit with
  `VkTimelineSemaphoreSubmitInfo` signalling `2f+2`; wait own fence; `submit_frame(host,0,generation,2f+2)`;
  `++frame`.
- [ ] **Step 4: Handoff gate.** Run under the Runtime; host composites `surfaces[ready]`; read back the host
  target → matches RPCS3's frame (black loading frame is acceptable proof of plumbing). No Vulkan validation
  errors; lock-step holds; no deadlock. Expected: `submit_frame` fires with climbing even `sync_value`; host
  acquire (odd) proceeds.
- [ ] **Step 5: Commit** (`feat: rpcs3_present zero-copy frame handoff into the shared VkImage`).

### Task 4: Audio egress

> ⚠️ **Approach SUPERSEDED — see SUPERSEDING UPDATE §2.** Do NOT forward a fixed chunk on a free-running timer
> (that is the design that crackled in Dolphin). Pull by `rp_host_iface.audio_want(host)` and submit in
> ~512-frame chunks; across the process boundary, the DLL relays the want count to the child and the child
> relays the pulled s16 frames back over the pipe.

**Files:** Modify `rp_rpcs3_present.cpp`.

- [ ] **Step 1:** RPCS3 null audio backend; pull mixed interleaved-stereo s16; forward via
  `rp_host.audio_sample(host, frames, num_frames)`. Gate: nonsilent samples flow while the title runs.
- [ ] **Step 2: Commit** (`feat: rpcs3_present audio egress via rp_host.audio_sample`).

> **Input is OUT of this slice** (the RPCS3 pad web — its own slice). Savestate over the ABI: later.

## Execution status (2026-08-11)

- **Task 1 DONE** (gate pass): `rpcs3_present.dll` skeleton exports ABI v5; a load test mirroring `CoreLoader`
  passes (`abi_version=5`, `get_info={presenting,vulkan,rpcs3_present}`, create/get_av_info/load_content). Heavy
  DLL packages + links + loads.
- **Task 2 PARTIAL — gate proven, then a real BLOCKER.** `create`→`load_content(EBOOT)`→`start` boots RPCS3
  fully via the ABI on a `std::thread` (log: BootGame OK, cellAudio 48kHz open, RSX rendering, PRX modules
  loading — "booted, running the title"). BUT RPCS3 **destabilizes inside the DLL on a spawned thread**: silent
  process exit ~6s (QMetaObject `call_from_main_thread` + `app.exec()` on the boot thread) or ~1s (synchronous
  `call_from_main_thread`, no exec) — no crash dump, no fatal stderr, no "Stopping emulator". The standalone
  `rp_rpcs3_host` EXE (same boot logic, QCoreApplication on the process MAIN thread) runs stably 40s+.
  **ROOT CAUSE: RPCS3's QCoreApplication requires the process main thread**; the core can't own main (the
  Runtime does).
  **FIX OPTIONS (design decision for the next session):** (a) run RPCS3 on the process main thread (needs a
  Runtime "run-on-main" hook / different core-threading contract); **(b) spawn RPCS3 in a CHILD PROCESS** —
  clean for RPCS3 because it's desktop-only AND the OPAQUE_WIN32 shared image + external timeline are already
  **cross-process shareable**, so the zero-copy handoff works across the boundary; `rp_rpcs3_host` (which
  already runs RPCS3 stably on its own main thread) becomes the child, and `rpcs3_present.dll` launches + feeds
  it. **(b) is likely the cleanest for RPCS3** and should be the plan's revised Task-2 approach.
- **Task 2 RESOLVED via CHILD PROCESS (option b, user's choice) — DONE.** `rpcs3_present.dll` is now a thin
  launcher (34KB, no longer links rpcs3_lib): `start` spawns the sibling `rp_rpcs3_host.exe` (found via
  `GetModuleHandleEx`-from-address → same dir) with the EBOOT as argv[1]; the child runs RPCS3 on ITS own main
  thread. Verified STABLE: `running=1` for the full 28s window (vk_frames_presented=40), no ~6s crash, clean
  start/stop/DONE. `rp_rpcs3_host.exe` reads the boot path from argv[1] (LBP default otherwise). The in-process
  boot version of `rp_rpcs3_present.cpp` was replaced by the launcher.
- **Task 3 (revised for child process) — IN PROGRESS.**
  - **Sub-step A DONE (handle passing):** `rp_start` `DuplicateHandle`s the host image+timeline into inheritable
    copies and appends `--rp-image/--rp-timeline/--rp-uuid/--rp-gen/--rp-w/--rp-h` to the child cmdline
    (`bInheritHandles=TRUE`); `rp_rpcs3_host.exe` `rp_parse_handoff()` parses them into `g_handoff`. Verified:
    child logs `handoff: image=0x1234 timeline=0x5678 640x480 gen=7 uuid=..` then boots. Uses ring **slot 0**
    only (like dolphin_present), so only `surfaces[0]`'s handle is passed.
  - **Sub-step B/C TODO (the Vulkan producer, in the child):** force RPCS3's adapter to `g_handoff.uuid`;
    import the shared image + timeline into RPCS3's VkDevice; per-frame blit `get_present_source()` → shared
    image (BGRA→RGBA) + QFOT release `→VK_QUEUE_FAMILY_EXTERNAL_KHR` + timeline signal, then relay to the DLL.
    **MODEL: `cores/refcore_present_vk/RefCoreVk.cpp`** — `pick_device_by_uuid` (:216), `build_from_surfaces`
    import (:242,:281-340), `loop()` even/odd lock-step (:116-168, producer signals `2f`, waits `2*(f-count)+1`),
    `record_frame` QFOT (:175-211). Byte-identical `VkImageCreateInfo`: R8G8B8A8_UNORM, usage
    `TRANSFER_DST|SRC|COLOR_ATTACHMENT|SAMPLED`, TILING_OPTIMAL, EXCLUSIVE, UNDEFINED, OPAQUE_WIN32 dedicated;
    never close the host's NT handles. Also `tests/test_vulkan_handoff.cpp` = a no-core producer rig to validate
    import/blit/signal in isolation.
  - **Sub-step D DONE (submit_frame relay, compiles):** DLL `CreatePipe`s (write end inheritable → child via
    `--rp-pipe`, read end private), spawns `submit_reader` thread that `ReadFile`s `rp_submit_msg`
    {index,generation,sync_value} and calls `host.submit_frame`; `kill_child` closes our write end so the reader
    EOFs on child exit, then joins. Child has `rp_send_submit()` (WriteFile the msg) — call it after each frame.
  - **Sub-step B/C TODO — the producer (the intricate, deadlock-prone heart). COMPLETE SPEC:**
    - **Prereq patch 1 (device.cpp:508):** RPCS3's device enables `VK_EXT_external_memory_host` but NOT the
      win32 external ext's — ADD `VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME` +
      `VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME` to `requested_extensions` (guard on physical-device
      support); ensure `timelineSemaphore` feature enabled. Without these the OPAQUE_WIN32 import fails.
    - **Prereq patch 2 (adapter UUID):** before boot, enumerate VkPhysicalDevices, find the one whose
      `VkPhysicalDeviceIDProperties.deviceUUID == g_handoff.uuid`, read its name, set RPCS3's adapter to that
      name (`g_cfg.video.vk.adapter` / `Emu.SetDefaultGraphicsAdapter`). Single discrete GPU here so it likely
      already matches, but set explicitly.
    - **Handle extraction @ VKPresent.cpp:757** (from the VK cheat-sheet, all on `VKGSRender::flip`): VkDevice
      `*m_device`; VkPhysicalDevice `(VkPhysicalDevice)m_device->gpu()`; queue `m_device->get_graphics_queue()`;
      family `m_device->get_graphics_queue_family()`; src `image_to_flip->value` (**NULL-CHECK image_to_flip**);
      layout `image_to_flip->current_layout` (do NOT assume — read it); dims/format `->width()/height()/format()`.
    - **VKPresent hook (patch, inside `if (image_to_flip)` after the media-capture block ~:757):** call
      `rp_producer::on_present(device, phys, queue, qfam, srcImage, srcLayout, w, h, srcFormat)`.
    - **Producer (`rp_producer` in rp_rpcs3.cpp):** `active()`=`g_handoff.have`. Lazy `init`: import shared image
      — byte-identical `VkImageCreateInfo` (R8G8B8A8_UNORM, usage TRANSFER_DST|SRC|COLOR_ATTACHMENT|SAMPLED,
      TILING_OPTIMAL, EXCLUSIVE, UNDEFINED, `VkExternalMemoryImageCreateInfo` OPAQUE_WIN32) at
      `g_handoff.width×height`, `VkImportMemoryWin32HandleInfoKHR(g_handoff.image)`+dedicated, bind; import
      timeline `vkImportSemaphoreWin32HandleKHR(g_handoff.timeline)`; own VkCommandPool/Buffer/Fence on `qfam`.
      Per `on_present` (frame f, slot 0): raw barrier src `current_layout→TRANSFER_SRC`, shared `UNDEFINED→GENERAL`,
      `vkCmdBlitImage` src→shared LINEAR, restore src, **QFOT release shared GENERAL→GENERAL `qfam→
      VK_QUEUE_FAMILY_EXTERNAL_KHR`**; submit under **`vk::acquire_global_submit_lock()`** (MTRSX shares the
      queue!) with `VkTimelineSemaphoreSubmitInfo` **signal 2f, wait 2(f-1)+1 if f>1**; `vkWaitForFences`;
      `rp_send_submit(0, g_handoff.generation, 2f)`. MODEL: `RefCoreVk.cpp` loop/record_frame (swap its
      `vkCmdClearColorImage` for the blit; it lives entirely in GENERAL).
    - **BGRA→RGBA caveat:** `vkCmdBlitImage` does NOT swizzle channels — blitting RPCS3's B8G8R8A8 present-source
      into the R8G8B8A8 shared image swaps R/B (blue tint). Fix with a component-swizzled image view / small pass,
      or accept swapped colors for the first proof.
    - **GATE:** copy `tests/test_dolphin_core_e2e.cpp` → `test_rpcs3_core_e2e.cpp` (`RP_RPCS3_CORE_DIR` + a PS3
      EBOOT), null-HWND runtime, assert non-empty; OR `retropark_harness.exe --api vulkan --core <dir> --content
      <EBOOT>` (visual; window shows the frame once the blit lands).
  - **GATE:** copy `tests/test_dolphin_core_e2e.cpp` (null-HWND offscreen runtime → `load_core(rpcs3_present)` →
    `load_content(EBOOT.BIN file)` → loop `rp_runtime_present(buf)` → assert non-empty), swapping
    `RP_DOLPHIN_CORE_DIR`→`RP_RPCS3_CORE_DIR` + a PS3 EBOOT; child needs Qt-bin on PATH + `RPCS3_CONFIG_DIR`.
    OR visual: `retropark_harness.exe --api vulkan --core <dir>/cores/rpcs3_present --content <EBOOT.BIN>`.
    Timeline note: host `composite_and_present` waits `sync_value` + signals `sync_value+1` (VulkanBackend.cpp:517).
  - Audio (Task 4) relays over the same pipe.
  - **INTEGRATION VALIDATED under the real Runtime (2026-08-11):** core dir `build/cores/rpcs3_present/` (DLL +
    `rp_rpcs3_host.exe` sibling + `core.json`); `build/harness/windowed/Debug/retropark_harness.exe --api
    vulkan --core <coredir> --content <EBOOT.BIN>` (Qt bin on PATH + `RPCS3_CONFIG_DIR`). RetroPark's real
    Runtime loaded the core, created the shared image + timeline, `set_surfaces` (real handles), the DLL
    launched the child, and **RPCS3 booted + ran stably ~14.7s under the real Runtime**. Core-loads +
    child-gets-real-handles PROVEN; the window composites empty until the producer lands. So the ONLY remaining
    work is the Vulkan producer (sub-steps B/C) + the submit_frame pipe relay (D) + a headless doctest gate.

## Self-Review notes
- Spec §6 decision recorded: Path A (GPU blit). B (CPU staging) stays the documented portable fallback.
- LBP-content caveat (Slice I): the handoff gate shows black frames until LBP's boot-past-logos is solved
  separately — the plumbing is provable regardless.
- Heavy-DLL packaging (Task 1) is the first real risk — isolate it before the Vulkan interop (Task 3).
