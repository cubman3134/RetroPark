# RetroPark — Slice P Design (static-core path, the iOS-shaped loading)

**Date:** 2026-08-10
**Status:** Approved (design)
**Scope:** Prove RetroPark can load a core that is **statically compiled into the app** — no
`dlopen`/`LoadLibrary`, no DLL, no filesystem, resolved at link time — through the same Runtime/ABI path
as a dynamic core. This is libretro's `HAVE_DYNAMIC=0` mode and the mechanism that makes iOS (and any
locked-down platform that forbids loading downloaded native code) possible. Proven on Windows with a real
core, device-independently. **No core-ABI change.**

---

## 0. Context and goal

RetroPark's thesis is "runs on any platform, including iOS." iOS (and tvOS, consoles, etc.) forbid loading
native code that wasn't shipped and code-signed in the app bundle, and forbid child processes that load
code — so cores there must be **statically linked into the app**, exactly as libretro does with its
`HAVE_DYNAMIC=0` build. RetroPark is already structured for this: the Runtime loads cores through the
`ICoreModule` seam (`resolve(symbol)` — one virtual), with `Win32CoreModule` (LoadLibrary + GetProcAddress)
as the only implementation today. A `StaticCoreModule` that returns a compiled-in `rp_get_core_abi` pointer
is the static twin; the ABI, Runtime, compositor, and two-execution-model design are unchanged and portable.

This slice builds and proves that static path on Windows. It is the **portability-mechanism proof** — the
load-bearing evidence that "any platform" is real — not a shipped iOS app (a real iOS build additionally
needs a Metal render backend + the Apple toolchain, deferred; the user will validate on a Mac later).

### Decisions

| Decision | Choice |
|---|---|
| Mechanism | `StaticCoreModule` (implements the existing `ICoreModule`) + a `StaticCoreRegistry` (id → compiled-in `rp_get_core_abi` fn). Mirrors libretro's static/dynamic split. |
| Load API | A dedicated **`rp_runtime_load_static_core(rt, core_id)`**: registry lookup → `StaticCoreModule` → same create/validate/branch as `load_core`, but metadata (id/type/graphics_api/abi_version) comes from the core's own `get_info()` — **no `core.json` file, no DLL** (the true iOS shape). Dynamic `load_core` is unchanged. |
| Symbol uniqueness | Statically-linked cores can't all export `rp_get_core_abi` (one binary). Each static core gets a per-core unique getter name (compile-time rename) and registers it by id. |
| Proof | Two statically-linked cores in the test binary, device-independent (`refcore_driven`, CPU-only). |
| ABI | **No change.** |

---

## 1. Components

### `StaticCoreModule` (`src/loader/StaticCoreModule.h/.cpp`)
- Implements `ICoreModule`. Constructed with a `rp_get_core_abi_fn` (and, optionally, a small name→pointer
  map for cores that also export extra C symbols — not needed for the driven proof). `resolve(symbol)`
  returns the getter when `symbol == RP_CORE_ABI_EXPORT_NAME`, else `nullptr`. ~15 lines; the static twin of
  `Win32CoreModule`. No handle, no `FreeLibrary` — nothing to unload (the code lives in the app).

### `StaticCoreRegistry` (`src/loader/StaticCoreRegistry.h/.cpp`)
- A process-wide map `core_id → rp_get_core_abi_fn`. API: `register_core(id, getter)`, `bool has(id)`,
  `rp_get_core_abi_fn get(id)`. Cores register at startup (an explicit registration call from the app/test's
  init, or a static initializer). This is how the Runtime finds a core with no DLL and no filesystem.

### Runtime static-load path (`src/runtime/Runtime.cpp` + C API)
- New C API `rp_result rp_runtime_load_static_core(rp_runtime* rt, const char* core_id)`:
  1. `StaticCoreRegistry::get(core_id)` → getter (else `RP_ERR_NOT_FOUND`).
  2. Build a `StaticCoreModule(getter)`; `loader_.load(module)` (resolves the getter, checks
     `abi_version == RETROPARK_ABI_VERSION`); `loader_.create(&host_iface_)`.
  3. Read metadata from the core's `get_info(&info)` (id/type/graphics_api). Validate the same way
     `load_core` does (presenting core's `graphics_api` must match the runtime's `api_`), then run the
     **same** driven/presenting branch (`requires_content_`, `rebuild_surfaces`, deferred start, av-info,
     `open_audio`, …).
- **Refactor:** extract `load_core`'s post-`create` logic (the metadata-validate + driven/presenting branch)
  into a private `finish_load_core(rp_core_type type, rp_graphics_api gfx, std::string& err)` helper so the
  dynamic (`load_core`, metadata from `core.json`) and static (`load_static_core`, metadata from `get_info`)
  paths share one implementation. Behavior of the existing dynamic path is unchanged.

### Symbol-uniqueness for static cores (build)
- A core built as a **static library** for linking into the app exports its getter under a **per-core unique
  name** (a compile-time rename of `rp_get_core_abi`), and a tiny registration TU declares that symbol and
  calls `StaticCoreRegistry::register_core(id, &<unique_getter>)`. The dynamic DLL build is unchanged (still
  exports `rp_get_core_abi`). The proof uses a CMake `target_compile_definitions` rename (e.g.
  `-Drp_get_core_abi=refcore_driven_static_get_core_abi`) when compiling the core source into the test.

## 2. Data flow

```
startup: static cores self-register  (id -> &<core>_static_get_core_abi)   [no dlopen, no files]
rp_runtime_load_static_core(rt, "refcore_driven")
   -> StaticCoreRegistry::get(id) -> StaticCoreModule(getter)
   -> loader_.load (abi_version check) -> loader_.create(host)
   -> get_info(&info) -> finish_load_core(info.type, info.graphics_api)   [shared with load_core]
   -> run frames  (identical to the dynamic DLL path)
```

## 3. Error handling

- Unknown id → `RP_ERR_NOT_FOUND`. ABI-version mismatch → `RP_ERR_ABI_MISMATCH`. `create` null →
  `RP_ERR_INTERNAL`. Presenting core whose `graphics_api` != runtime api → `RP_ERR_UNSUPPORTED`. Same codes
  and semantics as `load_core`; never a crash.
- Double-register of the same id → **last-wins** (overwrites the entry). Registering after cores are loaded
  is a no-op for already-loaded cores.

## 4. Testing

- **Static-load proof (device-independent, always-on — no gate):** in the test, register `refcore_driven`
  (statically compiled into the test binary via the rename), `rp_runtime_create(RP_GFX_NONE)` +
  `rp_runtime_load_static_core(rt, "refcore_driven")` → drive frames (`rp_runtime_present`/advance) → assert
  the same non-black/advancing behavior the dynamic `refcore_driven` e2e asserts. **Zero DLL, zero
  filesystem** — proving the iOS-shaped load. Runs on every build (CPU-only, no GPU).
- **Multi-static-core no-collision proof:** compile `refcore_driven.cpp` a **second** time with a different
  rename + id (e.g. `refcore_driven_b` → `refcore_driven_b_static_get_core_abi`), register both, load each by
  id — asserts the registry + per-core symbol renaming supports many cores in one binary (the real iOS
  scenario). The collision proof is structural: linking two getters both named `rp_get_core_abi` would be a
  duplicate-symbol link error, so a test binary that **links and runs** with both cores present proves the
  renaming works.
- **Regression:** full A–O suite green; the dynamic DLL path (`rp_runtime_load_core`) is byte-for-byte
  unchanged (the shared `finish_load_core` helper preserves its behavior).

## 5. Scope

**In Slice P:** `StaticCoreModule`, `StaticCoreRegistry`, `rp_runtime_load_static_core` + the shared
`finish_load_core` refactor, per-core symbol-uniqueness for static linking, and the two static-core tests
(load + no-collision), device-independent. **No ABI change.**

**Out (later):** an actual iOS/tvOS build (Metal render backend + Apple toolchain + App Store packaging —
the user validates on a Mac later); statically linking the **presenting/GPU** cores (needs the render-backend
port); auto-registration macros / link-time core-list ergonomics; bundling manifests as app resources;
Android's static/bundled specifics.

**The single provable claim:** *A core statically compiled into the app — no `dlopen`, no DLL, no
filesystem — loads and runs through RetroPark's Runtime identically to a dynamic core, and two such cores
coexist in one binary without symbol collision. This is the mechanism that makes iOS and any locked-down
platform possible, proven on Windows with a real core; Mac/iOS validation is a later step.*

## 6. Repo additions

```
src/loader/StaticCoreModule.h, .cpp        # ICoreModule impl wrapping a compiled-in rp_get_core_abi
src/loader/StaticCoreRegistry.h, .cpp      # id -> getter registry
include/retropark/retropark.h              # + rp_runtime_load_static_core
src/runtime/Runtime.h, .cpp                # load_static_core + finish_load_core (shared with load_core)
tests/test_static_core.cpp                 # static-load + multi-core-no-collision (device-independent)
tests/CMakeLists.txt                       # static-link refcore_driven (twice, renamed) into the test
```
