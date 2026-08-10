# RetroPark Slice P — Static-Core Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load a core that's statically compiled into the app — no `dlopen`/DLL/filesystem — through the same Runtime/ABI path as a dynamic core (libretro's `HAVE_DYNAMIC=0` mode; the mechanism that makes iOS possible).

**Architecture:** A `StaticCoreModule` implements the existing `ICoreModule` seam by wrapping a compiled-in `rp_get_core_abi` pointer; a `StaticCoreRegistry` maps `core_id → getter`. `rp_runtime_load_static_core` looks the id up, builds a `StaticCoreModule`, and drives the core through the same create/branch logic as `load_core` (shared `finish_load_core` helper), taking metadata from the core's own `get_info()` — no file. Proven device-independently with two statically-linked `refcore_driven` copies (no GPU) plus a Runtime-path test on the headless D3D11/WARP backend.

**Tech Stack:** C++17, the existing `ICoreModule`/`CoreLoader` seam, RetroPark Runtime, doctest, CMake (static-link the core into the test with a per-copy symbol rename).

## Global Constraints

- **No AI attribution** in any commit message. Conventional prefixes (`feat:`/`fix:`/`docs:`).
- **ABI stays v5** — no change to `include/retropark/retropark_abi.h`. This slice adds a load path + a registry, not an ABI change.
- **The dynamic DLL path (`rp_runtime_load_core`) must stay behavior-identical** — the shared `finish_load_core` refactor preserves it; the full A–O suite stays green.
- **Windows-only, but keep it portable-shaped:** `StaticCoreModule`/`StaticCoreRegistry` use no Win32 API (plain C++), so they compile on any platform. The proof runs on Windows; Mac/iOS validation is deferred (needs a Metal backend + Apple toolchain).
- **Build:** `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug`. Full suite: `C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe` → `105 passed | 0 failed` before this slice; each task adds cases.
- **Reference patterns:** `src/loader/Win32CoreModule.h/.cpp` (the `ICoreModule` twin to mirror), `tests/test_savestate.cpp` (uses `CoreLoader` + a module directly, no Runtime/GPU — mirror for the device-independent test), `tests/test_driven_e2e.cpp` (the Runtime present-path a driven core is driven through).

---

### Task 1: StaticCoreModule + StaticCoreRegistry + device-independent proof

Build the static module seam + registry and prove a statically-linked core loads and runs through `CoreLoader` with **no GPU and no DLL**, plus two static cores coexisting with no symbol collision.

**Files:**
- Create: `src/loader/StaticCoreModule.h`, `src/loader/StaticCoreModule.cpp`
- Create: `src/loader/StaticCoreRegistry.h`, `src/loader/StaticCoreRegistry.cpp`
- Modify: `CMakeLists.txt` (add the two .cpp to the `retropark` lib source list at ~line 30-31)
- Create: `tests/test_static_core.cpp`
- Modify: `tests/CMakeLists.txt` (register the test; static-link two renamed copies of `RefCoreDriven.cpp`)

**Interfaces:**
- Consumes: `ICoreModule` (`src/loader/ICoreModule.h`), `CoreLoader` (`load`/`create`/`run_frame`/`serialize_size`/`serialize`, `abi()`), `RP_CORE_ABI_EXPORT_NAME` + `rp_get_core_abi_fn` (`retropark_abi.h`).
- Produces: `class StaticCoreModule : public ICoreModule` (ctor `StaticCoreModule(rp_get_core_abi_fn getter)`; `resolve(symbol)` returns the getter for `RP_CORE_ABI_EXPORT_NAME`, else `nullptr`); `namespace rp { struct StaticCoreRegistry { static void register_core(const std::string& id, rp_get_core_abi_fn getter); static bool has(const std::string& id); static rp_get_core_abi_fn get(const std::string& id); }; }` (last-wins on double-register; `get` returns `nullptr` for unknown).

- [ ] **Step 1: Write the failing device-independent test.** Create `tests/test_static_core.cpp`:

```cpp
#include <doctest/doctest.h>
#include "loader/StaticCoreModule.h"
#include "loader/StaticCoreRegistry.h"
#include "loader/CoreLoader.h"
#include <retropark/retropark_abi.h>
#include <string>

using namespace rp;

// Two static cores compiled into the test binary (RefCoreDriven.cpp, twice, with the getter renamed per
// copy — see tests/CMakeLists.txt). Declared here; registered by register_static_test_cores().
extern "C" const rp_core_abi* refcore_driven_static_get_core_abi(void);
extern "C" const rp_core_abi* refcore_driven_b_static_get_core_abi(void);

static void register_static_test_cores() {
    StaticCoreRegistry::register_core("refcore_driven", &refcore_driven_static_get_core_abi);
    StaticCoreRegistry::register_core("refcore_driven_b", &refcore_driven_b_static_get_core_abi);
}

TEST_CASE("static core: registry resolves registered ids, rejects unknown") {
    register_static_test_cores();
    CHECK(StaticCoreRegistry::has("refcore_driven"));
    CHECK(StaticCoreRegistry::has("refcore_driven_b"));   // second static core links -> no symbol collision
    CHECK_FALSE(StaticCoreRegistry::has("nope"));
    CHECK(StaticCoreRegistry::get("refcore_driven") != nullptr);
    CHECK(StaticCoreRegistry::get("nope") == nullptr);
}

TEST_CASE("static core: loads + runs through CoreLoader with no DLL and no GPU") {
    register_static_test_cores();
    StaticCoreModule mod(StaticCoreRegistry::get("refcore_driven"));
    CoreLoader ld; std::string err;
    REQUIRE(ld.load(&mod, err) == RP_OK);                 // resolves the compiled-in getter, checks abi_version
    REQUIRE(ld.create(nullptr, err) == RP_OK);
    // Drive it: refcore_driven's serialized state is its frame counter (Slice F). Advance -> it changes.
    for (int i = 0; i < 5; ++i) REQUIRE(ld.run_frame(err) == RP_OK);
    std::vector<uint8_t> a(ld.serialize_size());
    REQUIRE(ld.serialize(a.data(), a.size(), err) == RP_OK);
    for (int i = 0; i < 7; ++i) REQUIRE(ld.run_frame(err) == RP_OK);
    std::vector<uint8_t> b(ld.serialize_size());
    REQUIRE(ld.serialize(b.data(), b.size(), err) == RP_OK);
    CHECK(a != b);                                        // the statically-linked core actually ran
    ld.destroy();
}
```

- [ ] **Step 2: Wire the static cores + test into CMake.** In `tests/CMakeLists.txt`, add `test_static_core.cpp` to the `add_executable(retropark_tests …)` list, and add two object libraries compiling `RefCoreDriven.cpp` with per-copy getter renames, linked into the test (its internals are in an anonymous namespace, so only the `rp_get_core_abi` getter needs renaming):

```cmake
# Slice P: statically link refcore_driven into the test binary TWICE with renamed getters, to prove a
# core loads with no DLL and that two static cores coexist without symbol collision.
add_library(static_core_a OBJECT ${CMAKE_SOURCE_DIR}/cores/refcore_driven/RefCoreDriven.cpp)
target_include_directories(static_core_a PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(static_core_a PRIVATE rp_get_core_abi=refcore_driven_static_get_core_abi)
add_library(static_core_b OBJECT ${CMAKE_SOURCE_DIR}/cores/refcore_driven/RefCoreDriven.cpp)
target_include_directories(static_core_b PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(static_core_b PRIVATE rp_get_core_abi=refcore_driven_b_static_get_core_abi)
target_link_libraries(retropark_tests PRIVATE $<TARGET_OBJECTS:static_core_a> $<TARGET_OBJECTS:static_core_b>)
```

(If `$<TARGET_OBJECTS:…>` in `target_link_libraries` is awkward on this CMake, add them to the `add_executable` sources instead: `add_executable(retropark_tests … $<TARGET_OBJECTS:static_core_a> $<TARGET_OBJECTS:static_core_b>)`.)

- [ ] **Step 3: Build; verify it FAILS to compile.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug --target retropark_tests`
Expected: FAIL — `StaticCoreModule.h` / `StaticCoreRegistry.h` don't exist.

- [ ] **Step 4: Implement `StaticCoreModule`.** Create `src/loader/StaticCoreModule.h`:

```cpp
#pragma once
#include <cstring>
#include "loader/ICoreModule.h"
#include <retropark/retropark_abi.h>

namespace rp {
// ICoreModule backed by a compiled-in core getter (no dlopen). The static twin of Win32CoreModule: the
// core's code lives in the app binary, so there is nothing to load or free.
class StaticCoreModule : public ICoreModule {
public:
    explicit StaticCoreModule(rp_get_core_abi_fn getter) : getter_(getter) {}
    void* resolve(const char* symbol) override {
        if (symbol && std::strcmp(symbol, RP_CORE_ABI_EXPORT_NAME) == 0)
            return reinterpret_cast<void*>(getter_);
        return nullptr;
    }
private:
    rp_get_core_abi_fn getter_ = nullptr;
};
}
```

Create `src/loader/StaticCoreModule.cpp` (empty TU so CMake has a source to compile, or header-only — if header-only, skip the .cpp and don't add it to CMake):

```cpp
#include "loader/StaticCoreModule.h"
// Header-only; this TU exists so the class has a compilation unit in the retropark lib.
```

- [ ] **Step 5: Implement `StaticCoreRegistry`.** Create `src/loader/StaticCoreRegistry.h`:

```cpp
#pragma once
#include <string>
#include <retropark/retropark_abi.h>

namespace rp {
// Process-wide map core_id -> compiled-in rp_get_core_abi getter. Statically-linked cores register here
// at startup so the Runtime can load them with no DLL and no filesystem (the iOS shape).
struct StaticCoreRegistry {
    static void register_core(const std::string& id, rp_get_core_abi_fn getter);  // last-wins
    static bool has(const std::string& id);
    static rp_get_core_abi_fn get(const std::string& id);  // nullptr if unknown
};
}
```

Create `src/loader/StaticCoreRegistry.cpp`:

```cpp
#include "loader/StaticCoreRegistry.h"
#include <map>

namespace rp {
namespace {
std::map<std::string, rp_get_core_abi_fn>& registry() {
    static std::map<std::string, rp_get_core_abi_fn> r;   // function-local static: safe init order
    return r;
}
}
void StaticCoreRegistry::register_core(const std::string& id, rp_get_core_abi_fn getter) {
    registry()[id] = getter;   // last-wins
}
bool StaticCoreRegistry::has(const std::string& id) { return registry().count(id) != 0; }
rp_get_core_abi_fn StaticCoreRegistry::get(const std::string& id) {
    auto it = registry().find(id);
    return it == registry().end() ? nullptr : it->second;
}
}
```

- [ ] **Step 6: Add the sources to the retropark lib.** In `CMakeLists.txt`, after `src/loader/Win32CoreModule.cpp` (line ~31), add `src/loader/StaticCoreRegistry.cpp` (and `src/loader/StaticCoreModule.cpp` if you made it a .cpp).

- [ ] **Step 7: Build + run the static-core tests.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug --target retropark_tests && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe --test-case="static core*"`
Expected: 2 passed. The binary linking at all proves the two static cores coexist (no duplicate `rp_get_core_abi`).

- [ ] **Step 8: Full suite + commit.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: `107 passed | 0 failed` (105 + 2 new cases).

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add src/loader/StaticCoreModule.h src/loader/StaticCoreModule.cpp src/loader/StaticCoreRegistry.h src/loader/StaticCoreRegistry.cpp CMakeLists.txt tests/test_static_core.cpp tests/CMakeLists.txt && git commit -m "feat(loader): StaticCoreModule + StaticCoreRegistry — statically-linked cores load via the ICoreModule seam (no dlopen, device-independent proof)"
```

---

### Task 2: Runtime `load_static_core` + shared `finish_load_core` refactor

Add the Runtime static-load path and prove it end-to-end through the present pipeline, with the dynamic path refactored to share the same post-create logic (behavior unchanged).

**Files:**
- Modify: `src/runtime/Runtime.h` (generalize `module_` to `ICoreModule`; declare `load_static_core` + `finish_load_core`)
- Modify: `src/runtime/Runtime.cpp` (`load_static_core`, extract `finish_load_core`, C API)
- Modify: `include/retropark/retropark.h` (declare `rp_runtime_load_static_core`)
- Modify: `tests/test_static_core.cpp` (Runtime-path case)

**Interfaces:**
- Consumes (from Task 1): `StaticCoreModule`, `StaticCoreRegistry::get`. From the Runtime: `loader_.load`/`create`, `loader_.abi()->get_info(rp_core_info*)`, the existing driven/presenting branch logic in `load_core`.
- Produces: `rp_result rp_runtime_load_static_core(rp_runtime* rt, const char* core_id)`; `Runtime::load_static_core(const std::string& id)`; `Runtime::finish_load_core(rp_core_type type, std::string& err)` (private, shared by `load_core` and `load_static_core`).

- [ ] **Step 1: Write the failing Runtime-path test.** In `tests/test_static_core.cpp`, add (mirrors `test_driven_e2e.cpp`, but loads statically; D3D11/WARP is headless in this suite):

```cpp
#include <retropark/retropark.h>

TEST_CASE("static core: Runtime loads it by id and drives frames through present (no DLL)") {
    register_static_test_cores();
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);   // headless WARP, like the driven e2e
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    CHECK(rp_runtime_load_static_core(rt, "no_such_id") == RP_ERR_NOT_FOUND);
    REQUIRE(rp_runtime_load_static_core(rt, "refcore_driven") == RP_OK);   // loaded from the registry, no DLL
    std::vector<uint8_t> img(64 * 64 * 4, 0);
    bool sawGreen = false;
    for (int i = 0; i < 60 && !sawGreen; ++i) {
        if (rp_runtime_present(rt, img.data()) != RP_OK) continue;
        if (img[((64 - 4) * 64 + (64 - 4)) * 4 + 1] > 150) sawGreen = true;  // core's green (driven e2e check)
    }
    CHECK(sawGreen);
    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}
```

- [ ] **Step 2: Build; verify it FAILS.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug --target retropark_tests`
Expected: FAIL — `rp_runtime_load_static_core` undeclared.

- [ ] **Step 3: Declare the C API.** In `include/retropark/retropark.h`, near `rp_runtime_load_core`, add:

```c
/* Load a core that was statically compiled into the app + registered in the StaticCoreRegistry, by its id.
   No DLL, no filesystem — the static/dynamic split that makes iOS (and any locked-down platform) possible.
   Metadata (type/graphics_api/abi_version) comes from the core's get_info(). */
rp_result   rp_runtime_load_static_core(rp_runtime* rt, const char* core_id);
```

- [ ] **Step 4: Generalize `module_` + declare the new methods.** In `src/runtime/Runtime.h`: change `#include "loader/Win32CoreModule.h"` to also pull `#include "loader/ICoreModule.h"` (and `StaticCoreModule.h`/`StaticCoreRegistry.h`), change the member to `std::unique_ptr<ICoreModule> module_;`, and declare in the private/public section:

```cpp
    rp_result load_static_core(const std::string& core_id);
```
and (private):
```cpp
    rp_result finish_load_core(rp_core_type type, std::string& err);  // shared post-create logic
```

- [ ] **Step 5: Extract `finish_load_core` and use it in `load_core`.** In `src/runtime/Runtime.cpp`, refactor `load_core` so that everything from `core_loaded_ = true;` through the end of the driven/presenting branches becomes `finish_load_core`. `load_core` keeps its manifest read, the early `graphics_api`-match check, module open, `loader_.load`, `loader_.create`, then `return finish_load_core(m.type, err);`. `finish_load_core` contains exactly the current post-create body:

```cpp
rp_result Runtime::finish_load_core(rp_core_type type, std::string& err) {
    core_loaded_ = true;
    core_type_ = type;
    if (type == RP_CORE_DRIVEN) {
        requires_content_ = loader_.has_load_content();
        content_loaded_ = false;
        if (!requires_content_) {
            rp_av_info av{};
            rp_result r = loader_.get_av_info(&av, err);
            if (r != RP_OK) { unload_core(); return r; }
            if (!(av.base_width > 0 && av.base_height > 0)) { unload_core(); return RP_ERR_UNSUPPORTED; }
            if (av.pixel_format != RP_FMT_R8G8B8A8_UNORM) { unload_core(); return RP_ERR_UNSUPPORTED; }
            dr_max_w_ = std::max(av.max_width, av.base_width);
            dr_max_h_ = std::max(av.max_height, av.base_height);
            open_audio(av);
        }
        return RP_OK;
    }
    // Presenting core (unchanged from load_core's presenting branch).
    requires_content_ = loader_.has_load_content();
    content_loaded_ = false;
    rp_result r = rebuild_surfaces(err);
    if (r != RP_OK) { unload_core(); return r; }
    if (!requires_content_) {
        r = loader_.start(err);
        if (r != RP_OK) { unload_core(); return r; }
    }
    return RP_OK;
}
```

Update `load_core` to open into a `Win32CoreModule` local named **`mod`** (the existing manifest local is named `m` — do NOT shadow it), then move into the generalized `module_`:
```cpp
    // ... (existing: parse core.json into `CoreManifest m`, the early graphics_api-match check, build `dll`) ...
    std::unique_ptr<Win32CoreModule> mod;
    if (Win32CoreModule::open(dll, mod, err) != RP_OK) return RP_ERR_NOT_FOUND;
    module_ = std::move(mod);
    if (loader_.load(module_.get(), err) != RP_OK) { module_.reset(); return RP_ERR_ABI_MISMATCH; }
    if (loader_.create(&host_iface_, err) != RP_OK) { loader_.destroy(); module_.reset(); return RP_ERR_INTERNAL; }
    return finish_load_core(m.type, err);   // `m` = the CoreManifest; its .type feeds the shared branch logic
```
This preserves the dynamic path exactly — same manifest read, same early `graphics_api` check, same errors — only the post-create branch body now lives in `finish_load_core`.

- [ ] **Step 6: Implement `load_static_core` + the C API.** In `src/runtime/Runtime.cpp`:

```cpp
rp_result Runtime::load_static_core(const std::string& core_id) {
    if (!init_ok_) return RP_ERR_DEVICE;
    if (core_loaded_ || loader_.state() != LoaderState::Unloaded) unload_core();
    rp_get_core_abi_fn getter = StaticCoreRegistry::get(core_id);
    if (!getter) return RP_ERR_NOT_FOUND;
    std::string err;
    module_ = std::make_unique<StaticCoreModule>(getter);
    if (loader_.load(module_.get(), err) != RP_OK) { module_.reset(); return RP_ERR_ABI_MISMATCH; }
    rp_core_info info{};
    if (loader_.abi() && loader_.abi()->get_info) loader_.abi()->get_info(&info);
    else { loader_.destroy(); module_.reset(); return RP_ERR_INTERNAL; }
    if (info.type == RP_CORE_PRESENTING && (rp_graphics_api)info.graphics_api != api_) {
        loader_.destroy(); module_.reset(); return RP_ERR_UNSUPPORTED;   // presenting core must match runtime api
    }
    if (loader_.create(&host_iface_, err) != RP_OK) { loader_.destroy(); module_.reset(); return RP_ERR_INTERNAL; }
    return finish_load_core((rp_core_type)info.type, err);
}
```

And the C API next to `rp_runtime_load_core`:
```cpp
rp_result rp_runtime_load_static_core(rp_runtime* rt, const char* core_id) {
    return reinterpret_cast<Runtime*>(rt)->load_static_core(core_id ? core_id : "");
}
```

- [ ] **Step 7: Build + run the Runtime-path test.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug --target retropark_tests && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe --test-case="static core: Runtime*"`
Expected: PASS — the statically-linked `refcore_driven` loads by id (no DLL) and its green reaches the composited readback.

- [ ] **Step 8: Full suite (dynamic path unchanged) + commit.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: `108 passed | 0 failed` (107 + 1). The driven/vulkan/dolphin dynamic-load tests still pass, proving `finish_load_core` preserved the dynamic path.

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add src/runtime/Runtime.h src/runtime/Runtime.cpp include/retropark/retropark.h tests/test_static_core.cpp && git commit -m "feat(runtime): rp_runtime_load_static_core — load a statically-linked core by id (metadata from get_info, no DLL/file); shared finish_load_core"
```

---

## Post-plan: verify + merge + memory

After Task 2 is green and reviewed: full suite green (dynamic path unchanged + the new static cases), then merge to `main` + push `origin main` (no finish-branch menu, no AI attribution), then update memory (`retropark-project.md` + `MEMORY.md`) marking Slice P done — noting it proves the static-core loading mechanism (the iOS/locked-down-platform path) on Windows device-independently, that `StaticCoreModule`/`StaticCoreRegistry` are Win32-free (portable), and that a real iOS/Mac build (Metal backend + Apple toolchain) is the deferred next step the user validates later.
