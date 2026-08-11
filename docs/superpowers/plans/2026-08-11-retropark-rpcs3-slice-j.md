# RPCS3 Slice J — `rpcs3_present` loadable core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development or executing-plans
> to implement task-by-task. Steps use `- [ ]` checkboxes.

**Goal:** Package the Slice-I RPCS3 embed as a loadable `rpcs3_present` RetroPark core (ABI v5) that renders
into the Runtime's host-owned shared `VkImage` exactly like `dolphin_present`.

**Architecture:** A `rpcs3_present.dll` exports `rp_get_core_abi()`; the Runtime loads it, hands it a shared
`VkImage` + external timeline semaphore; the core imports them into RPCS3's own VkDevice (UUID-matched to the
host GPU) and per-frame blits RPCS3's present-source image into the shared image with QFOT + even/odd timeline
lock-step. **Frame-transfer path chosen: A — zero-copy GPU blit.**

**Tech Stack:** RPCS3-from-source (`rpcs3_lib` closure, Qt 6.10.3 at `D:\Qt\6.10.3\msvc2022_64`), RetroPark
`include/retropark/retropark_abi.h`, Vulkan external memory/semaphore (OPAQUE_WIN32), CMake/VS build.

## Global Constraints

- ABI **v5**, no bump. Export symbol `rp_get_core_abi`; `abi_version == RETROPARK_ABI_VERSION (5)`.
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
- **Task 3 (revised for child process):** on `start`, `DuplicateHandle` the host's shared-image + timeline NT
  handles into the child and append their child-side values + `device_uuid` + `generation` + `w/h` to the
  child's command line; the child (`rp_rpcs3_host.exe`) imports them into RPCS3's VkDevice, does the per-frame
  blit of `get_present_source()` into the shared image (QFOT + even/odd timeline), and relays `submit_frame`
  (index/generation/sync_value) back to the DLL over a pipe; the DLL calls `host.submit_frame`. Audio (Task 4)
  relays the same way.

## Self-Review notes
- Spec §6 decision recorded: Path A (GPU blit). B (CPU staging) stays the documented portable fallback.
- LBP-content caveat (Slice I): the handoff gate shows black frames until LBP's boot-past-logos is solved
  separately — the plumbing is provable regardless.
- Heavy-DLL packaging (Task 1) is the first real risk — isolate it before the Vulkan interop (Task 3).
