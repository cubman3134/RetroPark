<p align="center">
  <a href="https://discord.gg/bW7KMVhgwH"><img src="https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-blue" alt="License: GPLv3"></a>
  <img src="https://img.shields.io/badge/Core%20ABI-v5-informational" alt="Core ABI v5">
</p>

# RetroPark

A runtime that loads runnable modules — **cores** — at runtime and drives them
through a stable flat-C contract. RetroPark is to [EverythingBox][eb] what
libretro is to RetroArch: the layer that turns "an emulator" into "a core the
host can load, composite, record, rewind and net-sync without knowing what's
inside it."

The reason it isn't just libretro: libretro has **one** model — the host calls
`retro_run()`, the core produces exactly one frame and returns. That is a good
fit for a 6502 and a hopeless one for Dolphin or RPCS3, which own their own
threads, their own GPU device, and their own frame clock and cannot be pumped
one frame at a time by someone else's loop. RetroPark's contract has **two**
core models so a Game Boy core and a full PS3 emulator can both be "a RetroPark
core" without either one pretending to be the other.

## The two-core model

Every core declares itself **driven** or **presenting** in its `get_info`, and
the runtime treats it accordingly:

- **Driven** — the *host* owns the frame clock. The core runs exactly one frame
  per `run_frame()` and hands back a framebuffer + audio. Because the host holds
  the clock, it gets host-owned **rewind**, savestate **scrubbing**, and
  deterministic **lockstep / rollback netplay** for free. This is the libretro
  shape, and it's ideal for retro cores.
- **Presenting** — the *core* keeps its own loop, threads and GPU device, and
  renders into a shared surface the host provides. The host still owns the final
  composite (overlays, filters) and input routing — but **not** the clock, so a
  heavy emulator runs at its own full speed with no forced lockstep.

Pixels cross between the core's GPU device and the host's through a **ring of
shared textures** — a D3D11 shared handle guarded by an `IDXGIKeyedMutex`, or a
Vulkan image with a shared timeline semaphore — with a generation counter so a
resize can't hand the host a stale surface. The host composites its overlay on
top and presents. Two devices, zero forced copies on the hot path.

## Cores in the tree

| Core | Type | What it is |
|------|------|------------|
| `refcore_driven` | driven | The reference driven core; doubles as the static-core example (compiled in, no `dlopen`, for locked-down platforms). |
| `refcore_present` | presenting | Reference presenting core on **D3D11**. |
| `refcore_present_vk` | presenting | Reference presenting core on **Vulkan**. |
| `refcore_rollback` | driven | Reference core exercising rollback / netplay resimulation. |
| `libretro_shim` | driven | Wraps a real **libretro** core (e.g. `fceumm`) behind the RetroPark ABI — runs real NES ROMs with audio, savestate and rewind. |
| `dolphin_present` | presenting | **Dolphin** built from source, rendering GameCube/Wii titles into RetroPark's shared Vulkan image (audio, input, savestate, and Dolphin's own netplay driven headless). Built from a working checkout under `external/` (not vendored). |
| `rpcs3_present` | presenting | **RPCS3** as a cross-process Vulkan frame producer handing PS3 frames into the shared image. Built from a working checkout under `external/` (not vendored). |

`refcore_*`, `libretro_shim` and the reference cores build from this repo alone.
`dolphin_present` and `rpcs3_present` are **manifest stubs** here — their DLLs are
produced from the git-ignored emulator checkouts under `external/`; see
[`docs/dolphin-build.md`](docs/dolphin-build.md) and
[`docs/rpcs3-embed-findings.md`](docs/rpcs3-embed-findings.md).

## The API

Two headers under [`include/retropark/`](include/retropark/):

- **[`retropark.h`](include/retropark/retropark.h)** — the flat-C **frontend**
  API. A host creates a runtime for a graphics API, loads a core (by directory,
  or statically by id on platforms without `dlopen`), loads content, then drives
  it: `advance` / `present` / `render`, `set_input`, `save_state` / `load_state`,
  `set_rewind` / `rewind`, and the runtime-control hooks `pause` / `resume` /
  `reset` / `get_status`. Those last four are what let a frontend build a
  RetroArch-style hotkey + in-game menu layer on top of RetroPark without the
  runtime owning any UI.
- **[`retropark_abi.h`](include/retropark/retropark_abi.h)** — the **core** ABI a
  core implements: `get_info`, `create` / `destroy`, `set_surfaces`, `start` /
  `stop`, `run_frame`, `serialize` / `unserialize`, `load_content`, plus the host
  callback table (`submit_frame`, `input_state`, `video_refresh`,
  `audio_sample`). The ABI is versioned — currently **`RETROPARK_ABI_VERSION 5`**
  — and a core that declares the wrong version is refused at load.

## Where things are

- [`include/retropark/`](include/retropark/) — the two public headers above.
- [`src/`](src/) — the runtime library: `loader/` (module loading), `runtime/`
  (the `Runtime` that drives a core + `BackendFactory`), `render/d3d11/` and
  `render/vulkan/` (the two `IRenderBackend`s), `audio/`, `net/`.
- [`cores/`](cores/) — the cores above, each a package with a `core.json`.
- [`harness/windowed/`](harness/windowed/) — a Win32 smoke app
  (`retropark_harness`) that loads a core into a real window for hand-testing
  (F5/F7 savestate, P pause, F8 reset, keyboard + XInput pad).
- [`tests/`](tests/) — the [doctest](https://github.com/doctest/doctest) suite
  (`retropark_tests`), including a C-clean ABI-compile check and end-to-end tests
  that load the real built cores.
- [`cmake/`](cmake/) — GLSL→SPIR-V shader compile + embed.
- [`docs/`](docs/) — design docs and the per-slice specs/plans under
  `docs/superpowers/`.

## Build

RetroPark builds with CMake ≥ 3.20, a C++17 compiler, and the **Vulkan SDK**
(the runtime links `Vulkan::Vulkan` and compiles shaders with `glslc`; on Windows
it also links the D3D11 stack and XAudio2). doctest and nlohmann_json are fetched
automatically by `FetchContent`.

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

That configure/build/test line is the whole gate — there is no separate probe
runner. See **[CONTRIBUTING.md](CONTRIBUTING.md)** for the Visual Studio
generator caveat (this tree's cache is a newer generator than VS2022's bundled
cmake), how the GPU tests skip on machines without shared-texture support, and
the rules a review will hold you to.

## Status

Windows, both backends. The reference cores render on D3D11 and Vulkan; the
`libretro_shim` runs real NES ROMs with audio, savestate and rewind, and both
lockstep and rollback netplay against a reference core; **Dolphin** renders real
GameCube titles into the shared Vulkan image with audio, input, savestate and
headless netplay; **RPCS3** hands PS3 frames across a process boundary into the
same shared image. The core ABI is stable at **v5**. macOS / iOS (Metal, and the
static-core path that makes a no-`dlopen` platform possible) are designed but not
yet built.

RetroPark is a runtime and a set of cores, not an end-user application — there are
no prebuilt downloads. It is developed as the next-generation emulation runtime
for **[EverythingBox][eb]**, which is where it gets embedded behind a UI.

## Community

RetroPark is developed as part of EverythingBox, and discussion happens on the
EverythingBox **[Discord][discord]** — `#dev-general` is the right room for
runtime, ABI and core-integration questions. It's much cheaper to learn there
that an approach is wrong than to learn it in review.

Reproducible bugs and design proposals belong in the
[issue tracker](https://github.com/cubman3134/RetroPark/issues) — those need to
stay searchable and stay open until they're fixed, which chat is bad at.

## Licence

RetroPark is free software under the **[GNU General Public License v3.0](LICENSE)** —
use it, study it, change it, and share it, including commercially. Derivative
works must stay under the same licence and ship their source.

Building the heavy cores pulls in emulators with their **own** terms: **Dolphin**
is GPLv2-or-later and **RPCS3** is GPLv2. Those trees live under `external/` as
working checkouts and are **not** distributed as part of this repository; a binary
that combines RetroPark with one of them is a distribution governed by *those*
licences too. Read them before you redistribute a combined build.

[eb]: https://github.com/cubman3134/EverythingBox
[discord]: https://discord.gg/bW7KMVhgwH
