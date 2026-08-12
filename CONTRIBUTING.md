# Contributing to RetroPark

Thanks for wanting to help. RetroPark is a small C/C++17 runtime with a **stable
flat-C ABI** at its centre and two deliberately different core models bolted to
it. Most of what will get a pull request rejected is a violation of one of those
two contracts rather than a style nit, so the sections below are worth the five
minutes before you write code.

## Building

You need CMake ≥ 3.20, a C++17 compiler, and the **Vulkan SDK** — the runtime
links `Vulkan::Vulkan` and the build compiles the reference shaders with `glslc`
and embeds the SPIR-V as headers, so a missing SDK fails at configure time, not
at runtime. On Windows the runtime also links the D3D11 stack
(`d3d11 dxgi dxguid d3dcompiler`) and `xaudio2`. [doctest][doctest] and
[nlohmann_json][json] are fetched automatically by `FetchContent`; nothing else
is vendored that you have to install.

```bash
cmake -S . -B build
cmake --build build --config Debug
```

The Vulkan SDK this tree has been built against is `1.4.357.0`; any recent SDK
with `glslc` on it works.

### Use the right `cmake.exe` on Windows

This is the one build gotcha that will waste an afternoon. The checked-out
`build/` cache was generated with a **newer Visual Studio generator than the
cmake bundled inside VS2022**. If you drive the build with VS2022's cmake against
that cache you get a generator-mismatch error. Use a standalone CMake —
`C:\Program Files\CMake\bin\cmake.exe` — or delete `build/` and reconfigure with
whichever cmake you intend to keep using. Pick one and stay on it; the mismatch
is silent until it isn't.

## The gate

There is no probe runner and no CI — the gate is the test suite, run locally,
before every pull request:

```bash
ctest --test-dir build -C Debug --output-on-failure
```

It must report every test passing (currently **114 test cases**). The suite is a
single [doctest][doctest] executable, `retropark_tests`; you can run the binary
directly for a faster inner loop and use doctest's `--test-case=` /
`--test-suite=` filters while iterating, but a pull request is judged on the full
`ctest` run.

Two things about the suite are worth knowing before you trust a green result:

- **GPU tests skip themselves when the machine can't run them.** The
  shared-texture path needs a GPU that supports the cross-device sharing the
  presenting model relies on; `VulkanBackend`'s capability probe gates those
  tests so they **SKIP** rather than fail on a headless box or an unsupported
  adapter. A "pass" on a machine where they all skipped has proven nothing about
  the presenting path — run them somewhere they actually execute before claiming
  a rendering change works.
- **The end-to-end core tests load the real built cores**, wired in through
  CMake `RP_*_CORE_DIR` compile definitions, and some need real content on disk
  (the libretro/NES e2e expects a ROM directory). Those tests skip cleanly when
  the content isn't present; don't mistake a skip for a pass there either.

## The contracts a review will hold you to

### A core is driven **or** presenting — never both, never neither

The whole point of RetroPark is the two-core split (see the
[README](README.md#the-two-core-model)). A core declares its `rp_core_type` in
`get_info`, and the runtime branches on it everywhere: a **driven** core is
pumped one `run_frame()` at a time and the host owns the clock; a **presenting**
core owns its own loop and renders into the shared surface and the host owns only
the composite. These are not two styles of the same thing — they have different
lifecycles, different threading, and different guarantees.

Do not add a code path that blurs them: a driven core that spawns its own render
thread, or a presenting core that expects the host to call it once per frame, is
a core that works in the harness and breaks the first time the host does rewind
(driven-only) or runs the compositor freeze (presenting-only). If a change needs
the runtime to treat a core "sometimes driven, sometimes presenting," that is an
ABI design conversation on the [Discord][discord] first, not a patch.

### The ABI is a version number, and bumping it breaks every core

`RETROPARK_ABI_VERSION` in
[`retropark_abi.h`](include/retropark/retropark_abi.h) is the single compatibility
gate: a core exports its ABI version and the loader **refuses** a core that
declares a different one. That is a feature — it turns a silent struct-layout
mismatch into a clean load failure — but it means the number carries real weight.

- **Changing the `rp_core_abi` vtable or any struct the core and host pass across
  the boundary is an ABI break.** Adding a function pointer, reordering fields,
  changing a struct's size — all of it. When you do it, bump
  `RETROPARK_ABI_VERSION`, and understand that every existing core (including the
  ones built from the `external/` emulator trees) must be rebuilt against the new
  header. The last bump — v4 → v5 — added a port to `input_state` for netplay;
  that is the bar for what a bump is *for*.
- **A runtime-only feature does not touch the ABI.** The `pause` / `resume` /
  `reset` / `get_status` control hooks were added to the *frontend* API
  ([`retropark.h`](include/retropark/retropark.h)) and the `Runtime` without
  touching `retropark_abi.h` at all, because the cores didn't need to know. If
  you can implement your feature host-side by reusing what cores already expose,
  do — an ABI bump you didn't need is a rebuild you inflicted on every core for
  nothing. `tests/test_abi_compiles.c` includes the ABI header from a **C**
  translation unit and instantiates its structs, so an accidentally C++-only
  change to the header fails the build; keep the header C-clean.

### The shared surface is a ring with a generation counter — respect it

Pixels cross devices through a ring of shared textures, and a resize bumps a
**generation counter** so the host can tell a surface from the current
generation apart from one the core acquired before the resize. Code on either
side of that boundary — acquiring a slot, signalling the mutex/semaphore,
handling a resize — is the part most likely to look correct and deadlock or tear
under load. Changes there get read carefully, need to run on hardware where the
GPU tests actually execute (see the gate), and should say in the PR what you ran
them against.

### A new core or component is registered in CMake, and tested without a window

Extract logic you can test without a live GPU into something a doctest
`TEST_CASE` can exercise headlessly, and add the test to `tests/`. A new core is
a package under `cores/` with its `core.json` and its CMake target wired into the
build like its siblings — an unbuilt core is one no test can load. Follow the
existing cores as the pattern; `refcore_driven` is the smallest complete driven
example and `refcore_present_vk` the smallest presenting one.

Write the failing test first, watch it fail, then make it pass — the suite is
built that way and it's the fastest way to be sure an assertion actually
discriminates. An assertion that passes against broken code is worse than no
assertion, because it reads as protection.

## Commits

Conventional prefixes:

- `feat:` — a new capability (a core, an API function, a backend feature)
- `fix:` — a bug fix
- `docs:` — documentation only
- `refactor:` — behaviour-preserving restructuring

Write the body for whoever reads it in a year. If you worked around something
surprising — a driver quirk, an ordering constraint in the shared-surface
handshake, a reason the obvious approach doesn't work — say what surprised you.
That context is the part a future reader cannot reconstruct from the diff.

**Do not add any AI/tool attribution to commits or pull requests.** No
`Co-Authored-By` trailer, no "generated with" line, no tool name in the body.
Commits are authored by the repository owner; the tooling used to produce them is
not part of the record. This applies to PR descriptions and issue comments too.

## Reporting bugs and proposing features

Use the issue templates in [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/).
For a crash or a rendering fault, say which **backend** (D3D11 or Vulkan), which
**core**, and what your **GPU/driver** is — the presenting path is GPU-sensitive
and a repro that names the adapter is worth ten that don't.

For design discussion *before* you write code — anything touching the ABI, the
two-core contract, or the shared-surface handshake — `#dev-general` on the
[Discord][discord] is far lower latency than issue comments, and it is much
cheaper to learn there that an approach is wrong than to learn it in review.
Whatever is decided in chat still gets written down in the issue or the pull
request: chat is where a decision is reached, not where it is recorded.

By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).
Contributions are accepted under the [GNU General Public License v3.0](LICENSE).

[doctest]: https://github.com/doctest/doctest
[json]: https://github.com/nlohmann/json
[discord]: https://discord.gg/bW7KMVhgwH
