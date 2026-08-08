# RetroPark — Slice K Design (dolphin_present reusable rp_core)

**Date:** 2026-08-08
**Status:** Approved (design), pending implementation plan
**Scope:** Package the working Slice J copy-from-XFB producer as a **reusable `dolphin_present`
presenting core** behind `rp_core_abi` — loadable by the RetroPark Runtime like `refcore_present_vk`,
fed **any** GameCube/Wii ISO via the content ABI, driven through the normal presenting path so overlays
compose in our window. The reusable-core payoff of the whole Dolphin arc.

---

## 0. Context and goal

Slice J proved the handoff: standalone Dolphin renders a real ROM into RetroPark's shared `VkImage`
(copy-from-XFB), composited + overlay-blended, validation-clean. But it's driven by a **test-only C API**
(`rp_dolphin_boot`/`last_signal`/`stop`) and a manual consume loop. Slice K turns it into a first-class
core: the RetroPark **Runtime** loads it, hands it surfaces, and drives `present()` — exactly as it does
`refcore_present_vk` — so Dolphin becomes a swappable core, any ISO plays, and the whole capability stack
(overlays now; audio/input/savestate later) layers on because it's just a core behind the ABI.

### Decisions (packaging slice — no design forks)

| Decision | Choice |
|---|---|
| Shape | `dolphin_present.dll` exports `rp_get_core_abi()` (presenting vtable) + a `core.json` manifest; the Slice J C API is replaced by the ABI. |
| Mechanism | Reuse the Slice J copy-from-XFB producer verbatim (hidden window, `after_present_event` hook, own cmd buffer, QFOT + timeline). |
| Frame delivery | The producer reports each frame via `host->submit_frame(host, index, generation, sync_value=2f+2)` (not the global `last_signal`), so the Runtime's `SurfaceRing` + `present()` consume it — identical to `refcore_present_vk`. |
| Content | The **content ABI** (`load_content`, Slice D): `load_content(iso)` stores the path; `start()` boots Dolphin with it. Any `.rvz`/`.iso`. |
| ABI | **No core-ABI change** (presenting + content hooks all exist; ABI stays v5). |
| Out | Dolphin **audio + input/controllers**, savestate/netplay-for-Dolphin, Wii specifics, multi-game validation, the render-into-image zero-copy optimization — all later. |

---

## 1. Components

### `cores/dolphin_present/` — the core
- **`rp_get_core_abi()`** returns a `rp_core_abi` with `type=RP_CORE_PRESENTING`, `graphics_api=RP_GFX_VULKAN`,
  `id="dolphin_present"` and the lifecycle fns below. This replaces the Slice J `extern "C"` boot API in
  `rp_dolphin.cpp` (the producer + boot logic move behind the vtable, unchanged).
- **`create(host)`** — store the `rp_host_iface` (for `submit_frame`); no Dolphin boot yet.
- **`load_content(path)`** — store the ISO path (content ABI hook).
- **`set_surfaces(rp_surface_set)`** — copy the shared `shared_handle` + `sync_handle` + `device_uuid` +
  w/h into `RetroParkExternalPresent` (what the producer imports). Presenting, Vulkan.
- **`start()`** — boot Dolphin (the Slice J `HostThread`: hidden window, no-op alert handler, Vulkan
  backend, UUID device, boot the stored ISO) with the producer hooked. The producer now, after each
  blit+signal, calls `host->submit_frame(host, /*index=*/0, generation, /*sync_value=*/2f+2)` so the
  Runtime consumes via `present()`.
- **`stop()` / `destroy()`** — stop the emu thread, destroy the window, tear down the producer's Vulkan
  resources.
- **`core.json`** — manifest (`id`, `type=presenting`, `graphics_api=vulkan`, `requires_content=true`),
  mirroring `cores/refcore_present_vk/core.json`. Built + copied to `cores/dolphin_present/` beside the
  DLL (the loader's convention).

### RetroPark side (reuse — no host changes expected)
- `Runtime::rebuild_surfaces()` already exports the shared image + timeline and calls `set_surfaces`.
- `Runtime::present()` pulls `SurfaceRing::latest_ready(idx, sv)` and calls
  `composite_and_present(idx, sv, …)`. The producer's `submit_frame(sync_value=2f+2)` feeds the ring; the
  Runtime consumes and signals `2f+3` — the same lock-step, now Runtime-driven (the producer's
  `wait timeline≥2f+1` before frame f throttles Dolphin to the Runtime's present rate).

### Harness
- `--content <iso>` with the dolphin core dir: `rp_runtime_load_core(dolphin_present dir)` +
  `rp_runtime_load_content(iso)` → the Runtime drives `present()` → Dolphin in our window with the
  blended overlay. Reuses the Slice A/B windowed presenting harness.

## 2. Data flow (per frame)

```
Runtime: allocate_surfaces -> set_surfaces(shared_handle, sync_handle, uuid) -> start()
   Dolphin boots the ISO (hidden window), renders; after_present_event ->
     producer blits XFB -> shared image (QFOT release) -> signal timeline 2f+2 ->
       host->submit_frame(0, gen, 2f+2)   [into SurfaceRing]
   Runtime::present(): latest_ready -> composite_and_present(0, 2f+2) -> overlay blend ->
     present/readback -> signal timeline 2f+3
   producer frame f+1 waits timeline >= 2f+3 (host consumed) -> next blit
```
Identical to the `refcore_present_vk` presenting flow, Dolphin as the producer.

## 3. Error handling

- No content on `start()` → error (Dolphin needs an ISO). Missing ISO → surfaced, not a crash.
- No matching Vulkan device / no external-memory support → `set_surfaces`/`start` fails cleanly.
- The Slice J robustness carries: no-op alert handler (no modal blocks), single-threaded submission,
  1s fence/timeline timeouts, hidden window. Crash honesty unchanged (in-process; a Dolphin fault still
  faults the host — out-of-process isolation is a separate later slice).

## 4. Testing

- **Gated e2e through the Runtime/core path (real-GPU):** `rp_runtime_create(RP_GFX_VULKAN)` →
  `load_core(cores/dolphin_present)` → `load_content(Billy Hatcher)` → drive `rp_runtime_present` for a
  few hundred frames → assert the readback is non-black + changing + overlay-blended (the Slice J
  assertions, now via the public Runtime API, mirroring `test_vulkan_e2e.cpp`'s core-DLL path). WARN-skips
  without GPU / the built DLL / the ROM. Opt-in (heavy).
- **Harness (human proof):** `--content <iso>` shows Dolphin in our window with the overlay; manual.
- **Regression:** A–J suite green; Slice J's direct-handoff test still passes; the render-into-image dead
  code stays out of the live path.

## 5. Scope

**In Slice K:** `dolphin_present` `rp_core_abi` (create/load_content/set_surfaces/start/stop/destroy) +
`core.json` + build/copy; the producer reports via `submit_frame`; harness `--content` with the core;
the gated Runtime-path e2e. **No core-ABI change.**

**Out (later):** Dolphin audio (the Slice E `audio_sample` path for a presenting core), input/controllers,
savestate/rewind/netplay for Dolphin, out-of-process crash isolation, the pure render-into-image
zero-copy optimization, Wii specifics, multi-game validation, non-Vulkan Dolphin backends.

**The single provable claim of Slice K:** *`dolphin_present` is a reusable RetroPark presenting core —
the Runtime loads it, `load_content` accepts any GameCube ISO, and Dolphin renders it into RetroPark's
surface with a composed overlay, driven entirely through `rp_core_abi` like any other core, no
libretro. Proven by a gated Runtime-path e2e (non-black + changing + overlay-blend) and the game in the
harness window.*

## 6. Repo additions

```
cores/dolphin_present/
  core.json                              # manifest (presenting, vulkan, requires_content)
  (dolphin_present.dll built from external/dolphin/.../RetroParkDolphin.vcxproj, copied here — git-ignored)
external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp  # C API -> rp_core_abi vtable; submit_frame (patch)
harness/windowed/main.cpp                # --content with the dolphin core (if not already generic)
tests/test_dolphin_core_e2e.cpp          # gated Runtime-path e2e (load_core + load_content + present)
```
