# RetroPark — Slice I Design (build + headless-embed Dolphin on Vulkan)

**Date:** 2026-08-08
**Status:** Approved (design), pending implementation plan
**Scope:** Build standalone Dolphin from source (no libretro) and drive it headless on its **Vulkan**
backend to boot a real GameCube ROM and render real frames — proven by a captured non-black frame that
changes over time. This is the first slice of a three-slice arc that ends in a reusable
`dolphin_present` RetroPark core. **No shared-surface handoff yet** (that's Slice J).

---

## 0. Context and goal

RetroPark's thesis is "better libretro": run heavy apps (Dolphin/RPCS3/VLC) that libretro handles
poorly, **natively**, rendering into the host's shared surface via the presenting model. Slice B
already proved the hard half — a separate `VkDevice` (`refcore_present_vk`) rendering into RetroPark's
**exported shared `VkImage`**, timeline-synchronized, composited by our Vulkan compositor. The Dolphin
integration is that exact handoff with **real Dolphin replacing the reference core**.

Routing Dolphin through its libretro core is explicitly rejected — that inherits libretro's
performance and feature limits, which is the whole thing RetroPark exists to escape. We integrate
**standalone Dolphin from source**, its own Vulkan backend rendering into our surface at full native
speed, cross-platform (every OS has the external-memory primitive Slice B uses).

The three-slice arc:
- **Slice I (this):** build Dolphin + drive it headless on Vulkan, boot the real ROM, capture a
  verifiable non-black frame. **De-risks the biggest unknown — can we build and drive standalone
  Dolphin at all — and documents the exact init + present seam Slice J retargets.**
- **Slice J:** retarget Dolphin's Vulkan present into our imported shared `VkImage` + timeline (the
  Slice B pattern) → composited by our compositor. Zero copy, native speed.
- **Slice K:** package as a reusable `dolphin_present` **presenting core** behind `rp_core_abi`
  (`RP_CORE_PRESENTING` / `RP_GFX_VULKAN`), loadable by the runtime like `refcore_present_vk`, fed any
  GC/Wii ISO through the existing content ABI, overlay composited on top, in our one window. The
  integration *pattern* becomes the template for the next heavy app.

### Decisions already made (this brainstorm)

| Decision | Choice |
|---|---|
| Emulator | **Dolphin, standalone, from source.** Not the libretro core (slow/limited — the thing we're escaping). |
| Render API | **Vulkan** (Dolphin's cross-platform native backend; matches our Slice B shared-`VkImage` + timeline handoff). |
| First proof | **Build + headless boot of a real ROM + a verifiable non-black frame.** No shared-surface handoff (Slice J). |
| Source | Cloned into **`external/dolphin`** (git-ignored), pinned to a recent **stable release tag**. |
| Test ROM | **`C:\RetroBat\roms\gamecube\Billy Hatcher and the Giant Egg (USA)\Billy Hatcher and the Giant Egg (USA).rvz`** (`.rvz` = Dolphin's native disc format, loads directly). Kept out of git. |
| Verification | **Gated** (build + real GPU + Vulkan + ROM present): captured frame non-black + changes across frames. Partly manual (eyeball the screenshot) — real-GPU/hardware-gated like the Vulkan + netplay slices. |

---

## 1. Components

### Dolphin source + build (`external/dolphin`, git-ignored)
- Clone the official Dolphin repo with submodules, pinned to a recent **stable release tag** (steadier
  than `master`). The pinned tag/commit is recorded in the build doc/script for reproducibility.
- Build via **Dolphin's own CMake** with the **Vulkan** backend enabled (our Vulkan SDK is at
  `C:\VulkanSDK\1.4.357.0`). We do **not** need the Qt GUI (`DolphinQt`) — only the core + video +
  common libraries and a headless driver. First establish that the full build succeeds (buildability
  is a real risk), then narrow to the headless targets.
- Dolphin ships `DolphinNoGUI` — a headless frontend with a `Platform` abstraction (headless / Win32 /
  X11 / …). That is the embedding seam.

### Stage A — de-risk via DolphinNoGUI + Dolphin's own capture
Prove *build + boot-real-ROM + Vulkan-render* with the least surgery:
- Run `DolphinNoGUI` on Billy Hatcher with `-v Vulkan` (or the config equivalent) and the **headless
  platform**, for a bounded number of frames.
- Capture a frame using **Dolphin's built-in screenshot / frame-dump** (config: `Movie` frame-dump or
  the screenshot hotkey/automation). Assert the resulting image is **non-black** (a real rendered
  frame), and that two captures at different frame counts **differ** (real emulation advancing).
- This confirms the toolchain end-to-end without touching Dolphin's internals.

### Stage B — in-process embedding seam
A minimal RetroPark-side host (`external`-linked, e.g. `tools/dolphin_embed` or a gated test harness)
that links Dolphin's libraries and drives its real init sequence in-process:
- `UICommon::Init()` → construct a `WindowSystemInfo` (headless/offscreen) → `BootManager::BootCore()`
  with the ROM path → run the emulation for N frames → obtain a frame.
- **Frame access:** read back Dolphin's rendered frame (via its Vulkan backend's final image, or its
  existing framebuffer-dump path) and assert non-black + changing — the in-process equivalent of Stage
  A's proof.
- **The deliverable that matters for Slice J:** a written **seam report** documenting exactly (a) the
  init/boot call sequence, (b) where Dolphin's Vulkan backend produces its **final presentable image**
  (the class/function in `VideoBackends/Vulkan` that owns the swapchain present), and (c) the cleanest
  point to later substitute our external `VkImage` + timeline signal. Slice J hooks this.

If Stage B proves too deep for one slice, **Stage A alone is a complete Slice I** (build + headless
boot + non-black frame), with Stage B's in-process embed rolling into Slice J. The plan sequences A
before B so the slice always lands something provable.

## 2. Data flow (Slice I)

```
external/dolphin (built, Vulkan) → boot Billy Hatcher .rvz headless
   → Dolphin emulates + renders on its own VkDevice
   → frame captured (Dolphin screenshot/dump [Stage A] or in-process readback [Stage B])
   → assert non-black + changes across frames
```
No RetroPark surface, no compositor, no overlay yet — this slice ends at "Dolphin renders real frames,
captured and verified." The shared-`VkImage` handoff into our compositor is Slice J.

## 3. Error handling / honesty

- **Build failure** (missing dep, toolchain mismatch) → surfaced explicitly; the build doc records the
  exact CMake flags + toolchain that worked. Not hidden behind a green result.
- **Boot failure** (bad ROM path, missing GC IPL/BIOS — GC generally boots games without IPL) →
  reported; the test WARN-skips if the ROM is absent (like the FCEUmm gated tests).
- **No GPU / no Vulkan** → the render proof is real-GPU-gated; WARN-skip on machines without a capable
  Vulkan device (as the Slice B Vulkan tests do).
- **Frame capture nondeterminism** → assert *non-black* + *frames differ*, not pixel-exact (Dolphin is
  not deterministic frame-to-frame like the reference core); the bar is "real rendering happened."
- **This slice is heavily gated + partly manual** — full GPU-output verification isn't possible from
  the headless session; the automated gate proves non-black+changing, the screenshot is the human
  confirmation. Stated plainly, not overclaimed.

## 4. Scope

**In Slice I:** clone + pin + **build Dolphin** (Vulkan enabled, headless targets); **Stage A** —
DolphinNoGUI boots Billy Hatcher on Vulkan, capture a non-black, changing frame; **Stage B** — a
minimal in-process embedding that boots the ROM and reads back a frame, plus the **seam report** for
Slice J. The build recipe (exact flags/toolchain/pinned commit) is documented.

**Explicitly out (later slices):**
- **The shared-`VkImage` + timeline handoff into our compositor (Slice J).**
- **`dolphin_present` presenting-core packaging behind `rp_core_abi`, the overlay, and the harness
  (Slice K).**
- Dolphin audio, input/controllers, savestate/rewind/netplay, the Qt GUI, Wii support specifics,
  multi-game validation. GC/Wii breadth and capabilities layer on *after* the core exists (Slice K+).

**The single provable claim of Slice I:** *Standalone Dolphin, built from source with its Vulkan
backend (no libretro), boots the real Billy Hatcher GameCube ROM headless and renders real frames —
proven by a captured non-black frame that changes across frames — and the exact init + Vulkan-present
seam is documented for Slice J to retarget into RetroPark's shared surface.*

## 5. Repo additions

```
external/dolphin/                     # cloned Dolphin source (git-ignored, pinned stable tag)
docs/dolphin-build.md                 # exact clone/pin + CMake flags + toolchain that built it (reproducible)
docs/dolphin-seam.md                  # Stage B seam report: init sequence + Vulkan final-image present point (feeds Slice J)
tools/dolphin_embed/ (or a gated test) # minimal in-process host: boot ROM headless on Vulkan, read back a frame
tests/                                # gated: assert captured frame non-black + changes; WARN-skip w/o build/GPU/ROM
```
