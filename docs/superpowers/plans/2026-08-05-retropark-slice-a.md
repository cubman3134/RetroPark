# RetroPark Slice A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone C++ runtime that dynamically loads a "presenting" core (one that owns its own D3D11 device and render thread), lets it render into a host-owned shared texture, and composites a blended overlay over it — on Windows, via D3D11.

**Architecture:** A standalone library (`retropark`) exposes a C API to frontends and a flat C ABI to cores. A reference core (`refcore_present.dll`) models a heavy app: its own D3D11 device, its own render thread, drawing into a ring of host-allocated shared textures (shared NT handle + `IDXGIKeyedMutex`). The host composites the latest core frame plus a blended overlay in one GPU pass and presents (or reads back, headless).

**Tech Stack:** C++17, CMake, Direct3D 11 + DXGI, doctest (tests), nlohmann/json (manifest), Win32 (windowed harness). WARP software driver for deterministic headless GPU tests.

## Global Constraints

- **Language/std:** C++17. The two public headers (`retropark.h`, `retropark_abi.h`) MUST compile as C (`extern "C"`, C89-compatible types, `<stdint.h>`). Verify with a C-compiled translation unit.
- **No Qt, no EverythingBox references anywhere** in `retropark`. It is a standalone library.
- **Core ABI is a flat C ABI.** A core exports exactly one symbol: `rp_get_core_abi` (name in `RP_CORE_ABI_EXPORT_NAME`), returning a static `const rp_core_abi*`.
- **ABI version:** `RETROPARK_ABI_VERSION == 1`. Loader MUST reject any core whose `abi_version` differs, with `RP_ERR_ABI_MISMATCH`.
- **Keyed-mutex convention (every shared surface):** producer (core) `AcquireSync(0)` → render → `ReleaseSync(1)`; consumer (host) `AcquireSync(1)` → composite → `ReleaseSync(0)`. All `AcquireSync` calls use a finite timeout (default 100 ms); a timeout returns `RP_ERR_TIMEOUT` and the frame is skipped — never an infinite wait.
- **Pixel format:** `RP_FMT_R8G8B8A8_UNORM` (`DXGI_FORMAT_R8G8B8A8_UNORM`) only.
- **Headless determinism:** all pixel-assertion tests create the host device with `D3D_DRIVER_TYPE_WARP`. Cross-device handoff integration tests must tolerate environments where WARP lacks shared-keyed-mutex support: gate such a test with a capability probe and `SKIP` (doctest `WARN` + early return) rather than failing.
- **Build system:** CMake ≥ 3.20, `FetchContent` for doctest (tag `v2.4.11`) and nlohmann/json (tag `v3.11.3`). If network is unavailable, vendor the single headers under `third_party/` and point `FetchContent` at them — do not change the include paths (`<doctest/doctest.h>`, `<nlohmann/json.hpp>`).
- **Commits:** conventional prefixes (`feat:`, `test:`, `docs:`, `chore:`). No AI attribution of any kind in commit messages, PR bodies, or comments.
- **Warnings:** compile the library and cores with `/W4`. Warnings are acceptable during a red step but the task's final commit should be warning-clean for `retropark` sources.

---

## File Structure

```
RetroPark/
  CMakeLists.txt                      # top-level: options, FetchContent, subdirs
  cmake/                              # helper .cmake if needed
  include/retropark/
    retropark_abi.h                   # core-facing flat C ABI (Task 2)
    retropark.h                       # frontend-facing C API (Task 11)
  src/
    loader/
      ICoreModule.h                   # module abstraction (Task 4)
      CoreLoader.h / .cpp             # lifecycle state machine (Task 4)
      Win32CoreModule.h / .cpp        # LoadLibrary impl (Task 5)
      Manifest.h / .cpp               # core.json parse (Task 3)
    render/
      IRenderBackend.h                # abstraction (Task 8)
      SurfaceRing.h / .cpp            # ring + generation bookkeeping (Task 6)
      d3d11/
        D3D11Backend.h / .cpp         # device, swapchain, shared-tex ring (Task 8)
        D3D11Compositor.h / .cpp      # composite pass + overlay (Task 9)
        Shaders.h                     # embedded HLSL (Task 9)
    runtime/
      Runtime.h / .cpp                # ties loader+backend+input; C API impl (Task 11)
  cores/
    refcore_present/
      core.json                       # manifest (Task 10)
      RefCore.cpp                      # reference presenting core (Task 10)
      CMakeLists.txt
  harness/
    windowed/main.cpp                 # Win32 smoke app (Task 13)
    headless/main.cpp                 # (optional CLI; e2e lives in tests)
  tests/
    CMakeLists.txt
    test_main.cpp                     # doctest entry
    test_manifest.cpp                 # Task 3
    test_loader.cpp                   # Task 4
    test_loader_ffi.cpp              # Task 5
    test_surface_ring.cpp            # Task 6
    test_d3d11_handoff.cpp           # Task 8
    test_compositor.cpp              # Task 9
    test_e2e.cpp                     # Task 12
    mock_core/                        # mock core dll (Task 5)
      MockCore.cpp
      CMakeLists.txt
  third_party/                        # optional vendored single-headers
  docs/superpowers/
    specs/2026-08-05-retropark-slice-a-design.md
    plans/2026-08-05-retropark-slice-a.md
```

---

## Task 1: Project scaffold, CMake, doctest wired to one green test

**Files:**
- Create: `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/test_main.cpp`, `tests/test_sanity.cpp`
- Create: `.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces: a buildable CMake project; a `retropark_tests` executable runnable via `ctest`; a `retropark` static library target (empty for now) that later tasks add sources to.

- [ ] **Step 1: Write `.gitignore`**

```gitignore
/build/
*.user
*.obj
*.exe
*.dll
*.pdb
*.ilk
```

- [ ] **Step 2: Write the failing test**

`tests/test_sanity.cpp`:
```cpp
#include <doctest/doctest.h>

TEST_CASE("sanity: build harness is wired") {
    CHECK(1 + 1 == 2);
}
```

`tests/test_main.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

- [ ] **Step 3: Write top-level `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(RetroPark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if (MSVC)
  add_compile_options(/W4 /permissive-)
endif()

include(FetchContent)
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)
FetchContent_MakeAvailable(doctest nlohmann_json)

# Library target — sources appended by later tasks.
add_library(retropark STATIC)
target_include_directories(retropark PUBLIC include)
target_compile_features(retropark PUBLIC cxx_std_17)

enable_testing()
add_subdirectory(tests)
```

- [ ] **Step 4: Write `tests/CMakeLists.txt`**

```cmake
add_executable(retropark_tests
  test_main.cpp
  test_sanity.cpp)
target_link_libraries(retropark_tests PRIVATE retropark doctest::doctest nlohmann_json::nlohmann_json)
add_test(NAME retropark_tests COMMAND retropark_tests)
```

- [ ] **Step 5: Configure, build, run — verify green**

Run:
```bash
cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure
```
Expected: builds; `retropark_tests` runs; 1 test case, 0 failures.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/ .gitignore
git commit -m "chore: scaffold CMake project with doctest and empty retropark lib"
```

---

## Task 2: Core ABI header (`retropark_abi.h`)

**Files:**
- Create: `include/retropark/retropark_abi.h`
- Create: `tests/test_abi_compiles.c` (C-compiled TU proving C compatibility)
- Modify: `tests/CMakeLists.txt` (add the C TU to the test target)

**Interfaces:**
- Consumes: nothing.
- Produces: the entire core ABI every later task depends on — `RETROPARK_ABI_VERSION`, `rp_result`, `rp_core_type`, `rp_graphics_api`, `rp_pixel_format`, `rp_surface_desc`, `rp_input_state`, `rp_host_iface`, `rp_core_info`, `rp_core_abi`, `rp_get_core_abi_fn`, `RP_CORE_ABI_EXPORT_NAME`.

- [ ] **Step 1: Write the header**

`include/retropark/retropark_abi.h`:
```c
#ifndef RETROPARK_ABI_H
#define RETROPARK_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETROPARK_ABI_VERSION 1u
#define RP_CORE_ABI_EXPORT_NAME "rp_get_core_abi"

typedef enum rp_result {
    RP_OK = 0,
    RP_ERR_ABI_MISMATCH = 1,
    RP_ERR_BAD_ARG = 2,
    RP_ERR_DEVICE = 3,
    RP_ERR_INTERNAL = 4,
    RP_ERR_TIMEOUT = 5,
    RP_ERR_NOT_FOUND = 6,
    RP_ERR_UNSUPPORTED = 7
} rp_result;

typedef enum rp_core_type {
    RP_CORE_PRESENTING = 0,
    RP_CORE_DRIVEN = 1          /* declared; not implemented in Slice A */
} rp_core_type;

typedef enum rp_graphics_api {
    RP_GFX_D3D11 = 0,
    RP_GFX_VULKAN = 1           /* later slice */
} rp_graphics_api;

typedef enum rp_pixel_format {
    RP_FMT_R8G8B8A8_UNORM = 0
} rp_pixel_format;

typedef struct rp_host rp_host;   /* opaque host handle */
typedef struct rp_core rp_core;   /* opaque core instance */

typedef struct rp_surface_desc {
    uint32_t index;             /* slot index in the ring */
    uint32_t width;
    uint32_t height;
    uint32_t format;            /* rp_pixel_format */
    void*    shared_handle;     /* NT HANDLE to a shared, keyed-mutex Texture2D (D3D11) */
    uint64_t generation;        /* ring generation; echoed back on submit_frame */
} rp_surface_desc;

typedef struct rp_input_state {
    uint8_t  keys[256];         /* virtual-key down flags */
    int16_t  pad_axes[8];
    uint16_t pad_buttons;
} rp_input_state;

typedef struct rp_host_iface {
    rp_host* host;
    void (*log)(rp_host* host, int level, const char* msg);
    void (*submit_frame)(rp_host* host, uint32_t index, uint64_t generation);
    void (*input_state)(rp_host* host, rp_input_state* out);
} rp_host_iface;

typedef struct rp_core_info {
    uint32_t        abi_version;
    rp_core_type    type;
    rp_graphics_api graphics_api;
    const char*     id;
} rp_core_info;

typedef struct rp_core_abi {
    uint32_t abi_version;                       /* must equal RETROPARK_ABI_VERSION */
    void      (*get_info)(rp_core_info* out);
    rp_core*  (*create)(const rp_host_iface* host);
    void      (*destroy)(rp_core* core);
    rp_result (*set_surfaces)(rp_core* core, const rp_surface_desc* descs, uint32_t count);
    rp_result (*start)(rp_core* core);
    rp_result (*stop)(rp_core* core);
} rp_core_abi;

typedef const rp_core_abi* (*rp_get_core_abi_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* RETROPARK_ABI_H */
```

- [ ] **Step 2: Write the C-compatibility test (failing until wired)**

`tests/test_abi_compiles.c`:
```c
#include <retropark/retropark_abi.h>

/* Compile-time proof the ABI is C-clean and self-consistent. */
int retropark_abi_c_check(void) {
    rp_core_info info;
    info.abi_version = RETROPARK_ABI_VERSION;
    info.type = RP_CORE_PRESENTING;
    info.graphics_api = RP_GFX_D3D11;
    info.id = "x";
    return (int)info.abi_version;
}
```

- [ ] **Step 3: Add the C TU to the test target**

In `tests/CMakeLists.txt`, add `test_abi_compiles.c` to the `retropark_tests` sources and a reference so it links:

```cmake
add_executable(retropark_tests
  test_main.cpp
  test_sanity.cpp
  test_abi_compiles.c)
```

Add to `tests/test_sanity.cpp`:
```cpp
extern "C" int retropark_abi_c_check(void);

TEST_CASE("abi: header compiles as C and constants are sane") {
    CHECK(retropark_abi_c_check() == 1);
}
```

- [ ] **Step 4: Build and run — verify green**

Run:
```bash
cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure
```
Expected: builds (proving the header is valid C and C++); the abi test case passes.

- [ ] **Step 5: Commit**

```bash
git add include/retropark/retropark_abi.h tests/test_abi_compiles.c tests/test_sanity.cpp tests/CMakeLists.txt
git commit -m "feat: define RetroPark core ABI (retropark_abi.h)"
```

---

## Task 3: Manifest parsing (`core.json`)

**Files:**
- Create: `src/loader/Manifest.h`, `src/loader/Manifest.cpp`
- Create: `tests/test_manifest.cpp`
- Modify: top-level `CMakeLists.txt` (add source to `retropark`), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `rp_core_type`, `rp_graphics_api`, `rp_result` from `retropark_abi.h`.
- Produces:
  - `struct rp::CoreManifest { std::string id, name, entry; rp_core_type type; rp_graphics_api graphics_api; uint32_t abi_version; };`
  - `rp_result rp::parse_manifest(const std::string& json_text, CoreManifest& out, std::string& error);`

- [ ] **Step 1: Write the failing tests**

`tests/test_manifest.cpp`:
```cpp
#include <doctest/doctest.h>
#include "loader/Manifest.h"

using rp::parse_manifest;
using rp::CoreManifest;

static const char* kValid = R"({
  "id":"refcore_present","name":"Reference","type":"presenting",
  "abi_version":1,"graphics_api":"d3d11","entry":"refcore_present.dll"})";

TEST_CASE("manifest: valid parses") {
    CoreManifest m; std::string err;
    CHECK(parse_manifest(kValid, m, err) == RP_OK);
    CHECK(m.id == "refcore_present");
    CHECK(m.type == RP_CORE_PRESENTING);
    CHECK(m.graphics_api == RP_GFX_D3D11);
    CHECK(m.abi_version == 1u);
    CHECK(m.entry == "refcore_present.dll");
}

TEST_CASE("manifest: missing field rejected") {
    CoreManifest m; std::string err;
    CHECK(parse_manifest(R"({"id":"x"})", m, err) == RP_ERR_BAD_ARG);
    CHECK(!err.empty());
}

TEST_CASE("manifest: unknown type rejected") {
    CoreManifest m; std::string err;
    const char* j = R"({"id":"x","name":"n","type":"driven-plus","abi_version":1,
                        "graphics_api":"d3d11","entry":"x.dll"})";
    CHECK(parse_manifest(j, m, err) == RP_ERR_BAD_ARG);
}

TEST_CASE("manifest: driven type is accepted (declared)") {
    CoreManifest m; std::string err;
    const char* j = R"({"id":"x","name":"n","type":"driven","abi_version":1,
                        "graphics_api":"d3d11","entry":"x.dll"})";
    CHECK(parse_manifest(j, m, err) == RP_OK);
    CHECK(m.type == RP_CORE_DRIVEN);
}

TEST_CASE("manifest: malformed json rejected") {
    CoreManifest m; std::string err;
    CHECK(parse_manifest("{not json", m, err) == RP_ERR_BAD_ARG);
}
```

- [ ] **Step 2: Write the header**

`src/loader/Manifest.h`:
```cpp
#pragma once
#include <string>
#include <cstdint>
#include <retropark/retropark_abi.h>

namespace rp {
struct CoreManifest {
    std::string id;
    std::string name;
    std::string entry;
    rp_core_type type = RP_CORE_PRESENTING;
    rp_graphics_api graphics_api = RP_GFX_D3D11;
    uint32_t abi_version = 0;
};
rp_result parse_manifest(const std::string& json_text, CoreManifest& out, std::string& error);
}
```

- [ ] **Step 3: Run the tests — verify they fail to link**

Run (after wiring CMake per Step 5, or expect a link error now):
```bash
cmake --build build --config Debug
```
Expected: FAIL — `parse_manifest` unresolved.

- [ ] **Step 4: Write the implementation**

`src/loader/Manifest.cpp`:
```cpp
#include "loader/Manifest.h"
#include <nlohmann/json.hpp>

namespace rp {

static bool as_string(const nlohmann::json& j, const char* key, std::string& out) {
    if (!j.contains(key) || !j[key].is_string()) return false;
    out = j[key].get<std::string>();
    return true;
}

rp_result parse_manifest(const std::string& text, CoreManifest& out, std::string& error) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) { error = "malformed json"; return RP_ERR_BAD_ARG; }

    std::string type_s, gfx_s;
    if (!as_string(j, "id", out.id))    { error = "missing id"; return RP_ERR_BAD_ARG; }
    if (!as_string(j, "name", out.name)){ error = "missing name"; return RP_ERR_BAD_ARG; }
    if (!as_string(j, "entry", out.entry)){ error = "missing entry"; return RP_ERR_BAD_ARG; }
    if (!as_string(j, "type", type_s)) { error = "missing type"; return RP_ERR_BAD_ARG; }
    if (!as_string(j, "graphics_api", gfx_s)) { error = "missing graphics_api"; return RP_ERR_BAD_ARG; }
    if (!j.contains("abi_version") || !j["abi_version"].is_number_unsigned()) {
        error = "missing abi_version"; return RP_ERR_BAD_ARG;
    }
    out.abi_version = j["abi_version"].get<uint32_t>();

    if (type_s == "presenting") out.type = RP_CORE_PRESENTING;
    else if (type_s == "driven") out.type = RP_CORE_DRIVEN;
    else { error = "unknown type: " + type_s; return RP_ERR_BAD_ARG; }

    if (gfx_s == "d3d11") out.graphics_api = RP_GFX_D3D11;
    else if (gfx_s == "vulkan") out.graphics_api = RP_GFX_VULKAN;
    else { error = "unknown graphics_api: " + gfx_s; return RP_ERR_BAD_ARG; }

    return RP_OK;
}
}
```

- [ ] **Step 5: Wire CMake**

Top-level `CMakeLists.txt`, after the `add_library(retropark STATIC)` line:
```cmake
target_sources(retropark PRIVATE
  src/loader/Manifest.cpp)
target_include_directories(retropark PRIVATE src)
target_link_libraries(retropark PUBLIC nlohmann_json::nlohmann_json)
```

`tests/CMakeLists.txt`: add `test_manifest.cpp` to `retropark_tests` sources, and give the test target the `src` include dir:
```cmake
target_include_directories(retropark_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
```

- [ ] **Step 6: Build and run — verify green**

Run:
```bash
cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure
```
Expected: all manifest test cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/loader/Manifest.* tests/test_manifest.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: parse and validate core.json manifest"
```

---

## Task 4: Loader lifecycle state machine (with `ICoreModule` seam)

**Files:**
- Create: `src/loader/ICoreModule.h`, `src/loader/CoreLoader.h`, `src/loader/CoreLoader.cpp`
- Create: `tests/test_loader.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `rp_core_abi`, `rp_get_core_abi_fn`, `RP_CORE_ABI_EXPORT_NAME`, `RETROPARK_ABI_VERSION`, `rp_host_iface`, `rp_surface_desc`, `rp_result`.
- Produces:
  - `struct rp::ICoreModule { virtual ~ICoreModule(); virtual void* resolve(const char* symbol) = 0; };`
  - `enum class rp::LoaderState { Unloaded, Loaded, Created, Started };`
  - `class rp::CoreLoader` with:
    - `rp_result load(ICoreModule* module, std::string& error);`
    - `rp_result create(const rp_host_iface* host, std::string& error);`
    - `rp_result set_surfaces(const rp_surface_desc* descs, uint32_t count, std::string& error);`
    - `rp_result start(std::string& error);` / `rp_result stop(std::string& error);`
    - `void destroy();`
    - `LoaderState state() const;`
    - `const rp_core_abi* abi() const;`

`CoreLoader` does NOT own the module lifetime or call `LoadLibrary` — Task 5 supplies a real `ICoreModule`. Tests supply a fake.

- [ ] **Step 1: Write the failing tests**

`tests/test_loader.cpp`:
```cpp
#include <doctest/doctest.h>
#include "loader/CoreLoader.h"
#include <retropark/retropark_abi.h>
#include <cstring>

using namespace rp;

// ---- A fake core implemented inline, exposed through a fake module ----
namespace {
struct FakeCoreState { bool created=false, started=false; uint32_t surfaces=0; };
FakeCoreState g_fake;

void fake_get_info(rp_core_info* out){
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_PRESENTING;
    out->graphics_api = RP_GFX_D3D11;
    out->id = "fake";
}
rp_core* fake_create(const rp_host_iface*){ g_fake.created=true; return reinterpret_cast<rp_core*>(&g_fake); }
void      fake_destroy(rp_core*){ g_fake.created=false; }
rp_result fake_set_surfaces(rp_core*, const rp_surface_desc*, uint32_t n){ g_fake.surfaces=n; return RP_OK; }
rp_result fake_start(rp_core*){ g_fake.started=true; return RP_OK; }
rp_result fake_stop(rp_core*){ g_fake.started=false; return RP_OK; }

const rp_core_abi kGoodAbi = {
    RETROPARK_ABI_VERSION, fake_get_info, fake_create, fake_destroy,
    fake_set_surfaces, fake_start, fake_stop
};
const rp_core_abi kBadVersionAbi = {
    999u, fake_get_info, fake_create, fake_destroy, fake_set_surfaces, fake_start, fake_stop
};
const rp_core_abi* good_entry(){ return &kGoodAbi; }
const rp_core_abi* bad_entry(){ return &kBadVersionAbi; }

struct FakeModule : ICoreModule {
    rp_get_core_abi_fn fn;
    bool return_null_symbol = false;
    explicit FakeModule(rp_get_core_abi_fn f): fn(f) {}
    void* resolve(const char* symbol) override {
        if (return_null_symbol) return nullptr;
        if (std::strcmp(symbol, RP_CORE_ABI_EXPORT_NAME) == 0) return reinterpret_cast<void*>(fn);
        return nullptr;
    }
};
}

TEST_CASE("loader: happy path advances states") {
    g_fake = {};
    CoreLoader ld; std::string err;
    FakeModule mod(good_entry);
    CHECK(ld.load(&mod, err) == RP_OK);
    CHECK(ld.state() == LoaderState::Loaded);

    rp_host_iface host{}; host.host = nullptr;
    CHECK(ld.create(&host, err) == RP_OK);
    CHECK(ld.state() == LoaderState::Created);
    CHECK(g_fake.created);

    rp_surface_desc descs[2] = {};
    CHECK(ld.set_surfaces(descs, 2, err) == RP_OK);
    CHECK(g_fake.surfaces == 2u);

    CHECK(ld.start(err) == RP_OK);
    CHECK(ld.state() == LoaderState::Started);
    CHECK(g_fake.started);

    CHECK(ld.stop(err) == RP_OK);
    CHECK(ld.state() == LoaderState::Created);
    ld.destroy();
    CHECK(ld.state() == LoaderState::Unloaded);
    CHECK(!g_fake.created);
}

TEST_CASE("loader: abi version mismatch rejected at load") {
    CoreLoader ld; std::string err;
    FakeModule mod(bad_entry);
    CHECK(ld.load(&mod, err) == RP_ERR_ABI_MISMATCH);
    CHECK(ld.state() == LoaderState::Unloaded);
}

TEST_CASE("loader: missing export rejected") {
    CoreLoader ld; std::string err;
    FakeModule mod(good_entry);
    mod.return_null_symbol = true;
    CHECK(ld.load(&mod, err) == RP_ERR_NOT_FOUND);
    CHECK(ld.state() == LoaderState::Unloaded);
}

TEST_CASE("loader: create before load is rejected") {
    CoreLoader ld; std::string err; rp_host_iface host{};
    CHECK(ld.create(&host, err) == RP_ERR_INTERNAL);
}

TEST_CASE("loader: start before create is rejected") {
    CoreLoader ld; std::string err;
    FakeModule mod(good_entry);
    CHECK(ld.load(&mod, err) == RP_OK);
    CHECK(ld.start(err) == RP_ERR_INTERNAL);
}
```

- [ ] **Step 2: Write the headers**

`src/loader/ICoreModule.h`:
```cpp
#pragma once
namespace rp {
struct ICoreModule {
    virtual ~ICoreModule() = default;
    virtual void* resolve(const char* symbol) = 0;
};
}
```

`src/loader/CoreLoader.h`:
```cpp
#pragma once
#include <string>
#include <cstdint>
#include <retropark/retropark_abi.h>
#include "loader/ICoreModule.h"

namespace rp {
enum class LoaderState { Unloaded, Loaded, Created, Started };

class CoreLoader {
public:
    rp_result load(ICoreModule* module, std::string& error);
    rp_result create(const rp_host_iface* host, std::string& error);
    rp_result set_surfaces(const rp_surface_desc* descs, uint32_t count, std::string& error);
    rp_result start(std::string& error);
    rp_result stop(std::string& error);
    void destroy();

    LoaderState state() const { return state_; }
    const rp_core_abi* abi() const { return abi_; }

private:
    LoaderState state_ = LoaderState::Unloaded;
    ICoreModule* module_ = nullptr;
    const rp_core_abi* abi_ = nullptr;
    rp_core* core_ = nullptr;
};
}
```

- [ ] **Step 3: Run tests — verify they fail**

Run:
```bash
cmake --build build --config Debug
```
Expected: FAIL — `CoreLoader` methods unresolved.

- [ ] **Step 4: Write the implementation**

`src/loader/CoreLoader.cpp`:
```cpp
#include "loader/CoreLoader.h"

namespace rp {

rp_result CoreLoader::load(ICoreModule* module, std::string& error) {
    if (state_ != LoaderState::Unloaded) { error = "already loaded"; return RP_ERR_INTERNAL; }
    if (!module) { error = "null module"; return RP_ERR_BAD_ARG; }
    auto fn = reinterpret_cast<rp_get_core_abi_fn>(module->resolve(RP_CORE_ABI_EXPORT_NAME));
    if (!fn) { error = "core does not export " RP_CORE_ABI_EXPORT_NAME; return RP_ERR_NOT_FOUND; }
    const rp_core_abi* abi = fn();
    if (!abi) { error = "core returned null abi"; return RP_ERR_INTERNAL; }
    if (abi->abi_version != RETROPARK_ABI_VERSION) {
        error = "abi version mismatch"; return RP_ERR_ABI_MISMATCH;
    }
    module_ = module; abi_ = abi; state_ = LoaderState::Loaded;
    return RP_OK;
}

rp_result CoreLoader::create(const rp_host_iface* host, std::string& error) {
    if (state_ != LoaderState::Loaded) { error = "create requires Loaded"; return RP_ERR_INTERNAL; }
    if (!abi_->create) { error = "core missing create"; return RP_ERR_INTERNAL; }
    core_ = abi_->create(host);
    if (!core_) { error = "core create returned null"; return RP_ERR_INTERNAL; }
    state_ = LoaderState::Created;
    return RP_OK;
}

rp_result CoreLoader::set_surfaces(const rp_surface_desc* descs, uint32_t count, std::string& error) {
    if (state_ != LoaderState::Created) { error = "set_surfaces requires Created"; return RP_ERR_INTERNAL; }
    if (!abi_->set_surfaces) { error = "core missing set_surfaces"; return RP_ERR_UNSUPPORTED; }
    return abi_->set_surfaces(core_, descs, count);
}

rp_result CoreLoader::start(std::string& error) {
    if (state_ != LoaderState::Created) { error = "start requires Created"; return RP_ERR_INTERNAL; }
    rp_result r = abi_->start ? abi_->start(core_) : RP_ERR_UNSUPPORTED;
    if (r == RP_OK) state_ = LoaderState::Started;
    return r;
}

rp_result CoreLoader::stop(std::string& error) {
    if (state_ != LoaderState::Started) { error = "stop requires Started"; return RP_ERR_INTERNAL; }
    rp_result r = abi_->stop ? abi_->stop(core_) : RP_OK;
    state_ = LoaderState::Created;
    return r;
}

void CoreLoader::destroy() {
    if (state_ == LoaderState::Started) { std::string e; stop(e); }
    if (core_ && abi_ && abi_->destroy) abi_->destroy(core_);
    core_ = nullptr; abi_ = nullptr; module_ = nullptr;
    state_ = LoaderState::Unloaded;
}
}
```

- [ ] **Step 5: Wire CMake**

Add to `retropark` `target_sources`: `src/loader/CoreLoader.cpp`. Add `test_loader.cpp` to `retropark_tests`.

- [ ] **Step 6: Build and run — verify green**

Run:
```bash
cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure
```
Expected: all loader test cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/loader/ICoreModule.h src/loader/CoreLoader.* tests/test_loader.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: core loader lifecycle state machine with module seam"
```

---

## Task 5: Real dynamic loading (`Win32CoreModule`) + mock core DLL FFI test

**Files:**
- Create: `src/loader/Win32CoreModule.h`, `src/loader/Win32CoreModule.cpp`
- Create: `tests/mock_core/MockCore.cpp`, `tests/mock_core/CMakeLists.txt`
- Create: `tests/test_loader_ffi.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ICoreModule`, `CoreLoader`, the ABI.
- Produces:
  - `class rp::Win32CoreModule : public ICoreModule` with static `static rp_result open(const std::string& dll_path, std::unique_ptr<Win32CoreModule>& out, std::string& error);` and `void* resolve(const char*) override;` (frees the library in its destructor).
  - A `mock_core` shared library (built to the tests' output dir) exporting `rp_get_core_abi`.

- [ ] **Step 1: Write the mock core DLL**

`tests/mock_core/MockCore.cpp`:
```cpp
#include <retropark/retropark_abi.h>

#if defined(_WIN32)
  #define MOCK_EXPORT extern "C" __declspec(dllexport)
#else
  #define MOCK_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
struct MockCore { int started = 0; uint32_t surfaces = 0; };

void mock_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_PRESENTING;
    out->graphics_api = RP_GFX_D3D11;
    out->id = "mock_core";
}
rp_core* mock_create(const rp_host_iface*) { return reinterpret_cast<rp_core*>(new MockCore()); }
void mock_destroy(rp_core* c) { delete reinterpret_cast<MockCore*>(c); }
rp_result mock_set_surfaces(rp_core* c, const rp_surface_desc*, uint32_t n) {
    reinterpret_cast<MockCore*>(c)->surfaces = n; return RP_OK;
}
rp_result mock_start(rp_core* c) { reinterpret_cast<MockCore*>(c)->started = 1; return RP_OK; }
rp_result mock_stop(rp_core* c) { reinterpret_cast<MockCore*>(c)->started = 0; return RP_OK; }

const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, mock_get_info, mock_create, mock_destroy,
    mock_set_surfaces, mock_start, mock_stop
};
}

MOCK_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
```

`tests/mock_core/CMakeLists.txt`:
```cmake
add_library(mock_core SHARED MockCore.cpp)
target_link_libraries(mock_core PRIVATE retropark)
set_target_properties(mock_core PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:retropark_tests>
  LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:retropark_tests>)
add_dependencies(retropark_tests mock_core)
```

- [ ] **Step 2: Write the failing FFI test**

`tests/test_loader_ffi.cpp`:
```cpp
#include <doctest/doctest.h>
#include "loader/Win32CoreModule.h"
#include "loader/CoreLoader.h"
#include <memory>

using namespace rp;

// Directory of the test exe; the mock_core dll is emitted alongside it.
static std::string mock_dll_path() {
#if defined(_WIN32)
    return "mock_core.dll";     // same dir as the test exe; CWD is set by ctest WORKING_DIRECTORY
#else
    return "./libmock_core.so";
#endif
}

TEST_CASE("ffi: load real mock core dll through the full lifecycle") {
    std::unique_ptr<Win32CoreModule> mod;
    std::string err;
    REQUIRE(Win32CoreModule::open(mock_dll_path(), mod, err) == RP_OK);

    CoreLoader ld;
    CHECK(ld.load(mod.get(), err) == RP_OK);
    rp_host_iface host{};
    CHECK(ld.create(&host, err) == RP_OK);
    rp_surface_desc d[3] = {};
    CHECK(ld.set_surfaces(d, 3, err) == RP_OK);
    CHECK(ld.start(err) == RP_OK);
    CHECK(ld.stop(err) == RP_OK);
    ld.destroy();
    CHECK(ld.state() == LoaderState::Unloaded);
}

TEST_CASE("ffi: opening a missing dll fails cleanly") {
    std::unique_ptr<Win32CoreModule> mod;
    std::string err;
    CHECK(Win32CoreModule::open("does_not_exist.dll", mod, err) == RP_ERR_NOT_FOUND);
    CHECK(mod == nullptr);
}
```

- [ ] **Step 3: Write the header**

`src/loader/Win32CoreModule.h`:
```cpp
#pragma once
#include <memory>
#include <string>
#include "loader/ICoreModule.h"
#include <retropark/retropark_abi.h>

namespace rp {
class Win32CoreModule : public ICoreModule {
public:
    static rp_result open(const std::string& dll_path,
                          std::unique_ptr<Win32CoreModule>& out, std::string& error);
    ~Win32CoreModule() override;
    void* resolve(const char* symbol) override;
private:
    explicit Win32CoreModule(void* handle) : handle_(handle) {}
    void* handle_ = nullptr;
};
}
```

- [ ] **Step 4: Run — verify fail**

Run:
```bash
cmake --build build --config Debug
```
Expected: FAIL — `Win32CoreModule::open`/`resolve` unresolved.

- [ ] **Step 5: Write the implementation**

`src/loader/Win32CoreModule.cpp`:
```cpp
#include "loader/Win32CoreModule.h"
#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace rp {

rp_result Win32CoreModule::open(const std::string& path,
                                std::unique_ptr<Win32CoreModule>& out, std::string& error) {
#if defined(_WIN32)
    HMODULE h = ::LoadLibraryA(path.c_str());
    if (!h) { error = "LoadLibrary failed for " + path; return RP_ERR_NOT_FOUND; }
    out.reset(new Win32CoreModule(reinterpret_cast<void*>(h)));
    return RP_OK;
#else
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) { error = dlerror() ? dlerror() : "dlopen failed"; return RP_ERR_NOT_FOUND; }
    out.reset(new Win32CoreModule(h));
    return RP_OK;
#endif
}

Win32CoreModule::~Win32CoreModule() {
    if (!handle_) return;
#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
    ::dlclose(handle_);
#endif
}

void* Win32CoreModule::resolve(const char* symbol) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle_), symbol));
#else
    return ::dlsym(handle_, symbol);
#endif
}
}
```

- [ ] **Step 6: Wire CMake**

Add `src/loader/Win32CoreModule.cpp` to `retropark` sources. In `tests/CMakeLists.txt`: add `test_loader_ffi.cpp` to sources, `add_subdirectory(mock_core)` AFTER the `retropark_tests` target is defined, and set the test's working directory so the dll resolves:
```cmake
set_tests_properties(retropark_tests PROPERTIES WORKING_DIRECTORY $<TARGET_FILE_DIR:retropark_tests>)
```

- [ ] **Step 7: Build and run — verify green**

Run:
```bash
cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure
```
Expected: FFI test loads the real dll and completes the lifecycle; missing-dll test returns `RP_ERR_NOT_FOUND`.

- [ ] **Step 8: Commit**

```bash
git add src/loader/Win32CoreModule.* tests/mock_core/ tests/test_loader_ffi.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: dynamic core loading via LoadLibrary with mock-core FFI test"
```

---

## Task 6: Surface ring + generation bookkeeping

**Files:**
- Create: `src/render/SurfaceRing.h`, `src/render/SurfaceRing.cpp`
- Create: `tests/test_surface_ring.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing beyond `<cstdint>`.
- Produces:
  - `class rp::SurfaceRing` (pure bookkeeping — no GPU):
    - `SurfaceRing(uint32_t slot_count);`
    - `uint64_t reallocate(uint32_t width, uint32_t height);` — bumps generation, returns new generation.
    - `uint32_t next_producer_slot();` — round-robin index for the core to fill.
    - `bool accept_submit(uint32_t index, uint64_t generation);` — false if stale generation or bad index; on success records it as the latest ready slot.
    - `bool latest_ready(uint32_t& index_out) const;`
    - `uint64_t generation() const; uint32_t slot_count() const; uint32_t width() const; uint32_t height() const;`

- [ ] **Step 1: Write the failing tests**

`tests/test_surface_ring.cpp`:
```cpp
#include <doctest/doctest.h>
#include "render/SurfaceRing.h"

using rp::SurfaceRing;

TEST_CASE("ring: reallocate bumps generation and size") {
    SurfaceRing r(3);
    uint64_t g0 = r.generation();
    uint64_t g1 = r.reallocate(640, 480);
    CHECK(g1 > g0);
    CHECK(r.width() == 640);
    CHECK(r.height() == 480);
    CHECK(r.slot_count() == 3u);
}

TEST_CASE("ring: producer slots are round robin") {
    SurfaceRing r(3);
    r.reallocate(16,16);
    CHECK(r.next_producer_slot() == 0u);
    CHECK(r.next_producer_slot() == 1u);
    CHECK(r.next_producer_slot() == 2u);
    CHECK(r.next_producer_slot() == 0u);
}

TEST_CASE("ring: valid submit becomes latest_ready") {
    SurfaceRing r(3);
    uint64_t g = r.reallocate(16,16);
    uint32_t idx = r.next_producer_slot();
    CHECK(r.accept_submit(idx, g));
    uint32_t out=99;
    CHECK(r.latest_ready(out));
    CHECK(out == idx);
}

TEST_CASE("ring: stale generation submit is dropped") {
    SurfaceRing r(3);
    uint64_t g_old = r.reallocate(16,16);
    r.reallocate(32,32);              // new generation
    CHECK_FALSE(r.accept_submit(0, g_old));
    uint32_t out=99;
    CHECK_FALSE(r.latest_ready(out)); // nothing ready at new generation yet
}

TEST_CASE("ring: out-of-range index is rejected") {
    SurfaceRing r(2);
    uint64_t g = r.reallocate(16,16);
    CHECK_FALSE(r.accept_submit(5, g));
}
```

- [ ] **Step 2: Write the header**

`src/render/SurfaceRing.h`:
```cpp
#pragma once
#include <cstdint>

namespace rp {
class SurfaceRing {
public:
    explicit SurfaceRing(uint32_t slot_count);
    uint64_t reallocate(uint32_t width, uint32_t height);
    uint32_t next_producer_slot();
    bool     accept_submit(uint32_t index, uint64_t generation);
    bool     latest_ready(uint32_t& index_out) const;

    uint64_t generation() const { return generation_; }
    uint32_t slot_count() const { return slot_count_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    uint32_t slot_count_;
    uint32_t width_ = 0, height_ = 0;
    uint64_t generation_ = 0;
    uint32_t producer_cursor_ = 0;
    bool     has_ready_ = false;
    uint32_t ready_index_ = 0;
    uint64_t ready_generation_ = 0;
};
}
```

- [ ] **Step 3: Run — verify fail**

Run: `cmake --build build --config Debug`
Expected: FAIL — `SurfaceRing` methods unresolved.

- [ ] **Step 4: Write the implementation**

`src/render/SurfaceRing.cpp`:
```cpp
#include "render/SurfaceRing.h"

namespace rp {

SurfaceRing::SurfaceRing(uint32_t slot_count)
    : slot_count_(slot_count == 0 ? 1 : slot_count) {}

uint64_t SurfaceRing::reallocate(uint32_t width, uint32_t height) {
    width_ = width; height_ = height;
    ++generation_;
    producer_cursor_ = 0;
    has_ready_ = false;
    return generation_;
}

uint32_t SurfaceRing::next_producer_slot() {
    uint32_t s = producer_cursor_;
    producer_cursor_ = (producer_cursor_ + 1) % slot_count_;
    return s;
}

bool SurfaceRing::accept_submit(uint32_t index, uint64_t generation) {
    if (generation != generation_) return false;
    if (index >= slot_count_) return false;
    has_ready_ = true;
    ready_index_ = index;
    ready_generation_ = generation;
    return true;
}

bool SurfaceRing::latest_ready(uint32_t& index_out) const {
    if (!has_ready_ || ready_generation_ != generation_) return false;
    index_out = ready_index_;
    return true;
}
}
```

- [ ] **Step 5: Wire CMake + build + run**

Add `src/render/SurfaceRing.cpp` to `retropark`; add `test_surface_ring.cpp` to tests.
Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: all ring tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/render/SurfaceRing.* tests/test_surface_ring.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: surface ring with generation-based stale-frame rejection"
```

---

## Task 7: `IRenderBackend` interface + backend-shared types

**Files:**
- Create: `src/render/IRenderBackend.h`
- Create: `tests/test_backend_iface.cpp` (compile/const-correctness only)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `rp_surface_desc`, `rp_result`.
- Produces:
  - `struct rp::SharedSurface { void* shared_handle; uint32_t index; uint32_t width, height; };`
  - `struct rp::IRenderBackend` abstract:
    - `virtual rp_result initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) = 0;`
    - `virtual rp_result allocate_surfaces(uint32_t count, uint32_t w, uint32_t h, std::vector<rp_surface_desc>& out, std::string& err) = 0;`
    - `virtual rp_result composite_and_present(uint32_t ready_index, bool has_frame, uint8_t* out_rgba, std::string& err) = 0;`
    - `virtual ~IRenderBackend();`

This task only defines the interface; `D3D11Backend` implements it in Tasks 8–9.

- [ ] **Step 1: Write the header**

`src/render/IRenderBackend.h`:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <retropark/retropark_abi.h>

namespace rp {
struct IRenderBackend {
    virtual ~IRenderBackend() = default;
    virtual rp_result initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) = 0;
    virtual rp_result allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                        std::vector<rp_surface_desc>& out, std::string& err) = 0;
    // If out_rgba != null, the composited image (w*h*4, RGBA8) is copied there (headless).
    virtual rp_result composite_and_present(uint32_t ready_index, bool has_frame,
                                            uint8_t* out_rgba, std::string& err) = 0;
};
}
```

- [ ] **Step 2: Write a compile-only test**

`tests/test_backend_iface.cpp`:
```cpp
#include <doctest/doctest.h>
#include "render/IRenderBackend.h"

TEST_CASE("backend: interface is abstract and includes cleanly") {
    // A no-op subclass proves the vtable shape compiles.
    struct Stub : rp::IRenderBackend {
        rp_result initialize(void*, uint32_t, uint32_t, std::string&) override { return RP_OK; }
        rp_result allocate_surfaces(uint32_t, uint32_t, uint32_t,
                                    std::vector<rp_surface_desc>&, std::string&) override { return RP_OK; }
        rp_result composite_and_present(uint32_t, bool, uint8_t*, std::string&) override { return RP_OK; }
    } s;
    std::string e;
    CHECK(s.initialize(nullptr, 1, 1, e) == RP_OK);
}
```

- [ ] **Step 3: Wire, build, run**

Add `test_backend_iface.cpp` to tests.
Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: passes.

- [ ] **Step 4: Commit**

```bash
git add src/render/IRenderBackend.h tests/test_backend_iface.cpp tests/CMakeLists.txt
git commit -m "feat: IRenderBackend abstraction (surface + composite boundary)"
```

---

## Task 8: D3D11 backend — device, swapchain, shared-texture ring + cross-device handoff test

**Files:**
- Create: `src/render/d3d11/D3D11Backend.h`, `src/render/d3d11/D3D11Backend.cpp`
- Create: `tests/test_d3d11_handoff.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `IRenderBackend`, `rp_surface_desc`.
- Produces:
  - `class rp::D3D11Backend : public IRenderBackend` implementing `initialize` (WARP if `native_window==nullptr`, else hardware+swapchain) and `allocate_surfaces` (creates N shared, keyed-mutex Texture2Ds; fills `rp_surface_desc.shared_handle` with an NT handle via `IDXGIResource1::CreateSharedHandle`). `composite_and_present` is a stub returning `RP_ERR_UNSUPPORTED` until Task 9.
  - Accessor for tests: `ID3D11Device* device();` and a helper `static bool probe_shared_keyed_mutex();`

Reference behavior for shared textures: create with `D3D11_TEXTURE2D_DESC.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYED_MUTEX`, `BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE`. Obtain the NT handle via `QueryInterface(IDXGIResource1)` → `CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ|WRITE, nullptr, &handle)`. A second device opens it via `ID3D11Device1::OpenSharedResource1(handle, IID_PPV_ARGS(&tex))`.

- [ ] **Step 1: Write the failing cross-device handoff test**

`tests/test_d3d11_handoff.cpp`:
```cpp
#include <doctest/doctest.h>
#include "render/d3d11/D3D11Backend.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace rp;

// Producer writes a solid color into a shared texture; consumer reads it back.
TEST_CASE("d3d11: cross-device shared keyed-mutex handoff") {
    if (!D3D11Backend::probe_shared_keyed_mutex()) {
        WARN("environment lacks shared keyed-mutex support; skipping");
        return;
    }
    D3D11Backend host;               // consumer (WARP, headless)
    std::string err;
    REQUIRE(host.initialize(nullptr, 8, 8, err) == RP_OK);

    std::vector<rp_surface_desc> descs;
    REQUIRE(host.allocate_surfaces(1, 8, 8, descs, err) == RP_OK);
    REQUIRE(descs.size() == 1);
    REQUIRE(descs[0].shared_handle != nullptr);

    // Producer: a second WARP device opens the shared handle and clears it red.
    ComPtr<ID3D11Device> pdev; ComPtr<ID3D11DeviceContext> pctx;
    D3D_FEATURE_LEVEL fl;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &pdev, &fl, &pctx)));
    ComPtr<ID3D11Device1> pdev1; pdev.As(&pdev1);
    ComPtr<ID3D11Texture2D> ptex;
    REQUIRE(SUCCEEDED(pdev1->OpenSharedResource1(descs[0].shared_handle, IID_PPV_ARGS(&ptex))));
    ComPtr<IDXGIKeyedMutex> pkm; ptex.As(&pkm);
    REQUIRE(SUCCEEDED(pkm->AcquireSync(0, 100)));   // producer takes key 0
    ComPtr<ID3D11RenderTargetView> prtv;
    REQUIRE(SUCCEEDED(pdev->CreateRenderTargetView(ptex.Get(), nullptr, &prtv)));
    const float red[4] = {1,0,0,1};
    pctx->ClearRenderTargetView(prtv.Get(), red);
    pctx->Flush();
    REQUIRE(SUCCEEDED(pkm->ReleaseSync(1)));         // hand to consumer with key 1

    // Consumer reads back pixel (0,0) and expects red.
    uint8_t rgba[4] = {0,0,0,0};
    REQUIRE(host.readback_surface_pixel(descs[0].index, 0, 0, rgba, err) == RP_OK);
    CHECK(rgba[0] == 255);
    CHECK(rgba[1] == 0);
    CHECK(rgba[2] == 0);
}
```

Note: this test uses a test-only helper `readback_surface_pixel` on the backend (declared in Step 2) that acquires key 1, copies the texel to a staging texture, maps it, and releases key 0.

- [ ] **Step 2: Write the header**

`src/render/d3d11/D3D11Backend.h`:
```cpp
#pragma once
#include "render/IRenderBackend.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>

namespace rp {
class D3D11Backend : public IRenderBackend {
public:
    rp_result initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) override;
    rp_result allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                std::vector<rp_surface_desc>& out, std::string& err) override;
    rp_result composite_and_present(uint32_t ready_index, bool has_frame,
                                    uint8_t* out_rgba, std::string& err) override;

    // Test helpers.
    static bool probe_shared_keyed_mutex();
    rp_result readback_surface_pixel(uint32_t index, uint32_t x, uint32_t y,
                                     uint8_t rgba_out[4], std::string& err);
    ID3D11Device* device() const { return device_.Get(); }

protected:
    struct Surface {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        void* handle = nullptr;
    };
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Device1> device1_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;   // null when headless
    std::vector<Surface> surfaces_;
    uint32_t width_ = 0, height_ = 0;
};
}
```

- [ ] **Step 3: Run — verify fail**

Run: `cmake --build build --config Debug`
Expected: FAIL — `D3D11Backend` methods unresolved.

- [ ] **Step 4: Write the implementation (device + surfaces + readback; composite stubbed)**

`src/render/d3d11/D3D11Backend.cpp`:
```cpp
#include "render/d3d11/D3D11Backend.h"
using Microsoft::WRL::ComPtr;

namespace rp {

static rp_result make_device(bool warp, ComPtr<ID3D11Device>& dev,
                             ComPtr<ID3D11DeviceContext>& ctx, std::string& err) {
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr,
        warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { err = "D3D11CreateDevice failed"; return RP_ERR_DEVICE; }
    return RP_OK;
}

bool D3D11Backend::probe_shared_keyed_mutex() {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; std::string e;
    if (make_device(true, dev, ctx, e) != RP_OK) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 4; d.Height = 4; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYED_MUTEX;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) return false;
    ComPtr<IDXGIResource1> res; if (FAILED(tex.As(&res))) return false;
    HANDLE h = nullptr;
    if (FAILED(res->CreateSharedHandle(nullptr,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h))) return false;
    if (h) ::CloseHandle(h);
    return true;
}

rp_result D3D11Backend::initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) {
    width_ = w; height_ = h;
    const bool headless = (native_window == nullptr);
    rp_result r = make_device(headless, device_, ctx_, err);
    if (r != RP_OK) return r;
    if (FAILED(device_.As(&device1_))) { err = "no ID3D11Device1"; return RP_ERR_DEVICE; }
    // Swapchain creation for the windowed path is added in the harness task; headless needs none.
    return RP_OK;
}

rp_result D3D11Backend::allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                          std::vector<rp_surface_desc>& out, std::string& err) {
    surfaces_.clear(); out.clear();
    width_ = w; height_ = h;
    for (uint32_t i = 0; i < count; ++i) {
        Surface s;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYED_MUTEX;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &s.tex))) {
            err = "CreateTexture2D(shared) failed"; return RP_ERR_DEVICE;
        }
        if (FAILED(s.tex.As(&s.keyed))) { err = "no keyed mutex"; return RP_ERR_DEVICE; }
        if (FAILED(device_->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv))) {
            err = "CreateSRV failed"; return RP_ERR_DEVICE;
        }
        ComPtr<IDXGIResource1> res;
        if (FAILED(s.tex.As(&res))) { err = "no IDXGIResource1"; return RP_ERR_DEVICE; }
        HANDLE handle = nullptr;
        if (FAILED(res->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
            err = "CreateSharedHandle failed"; return RP_ERR_DEVICE;
        }
        s.handle = handle;

        rp_surface_desc desc{};
        desc.index = i; desc.width = w; desc.height = h;
        desc.format = RP_FMT_R8G8B8A8_UNORM;
        desc.shared_handle = handle;
        desc.generation = 0;   // Runtime overwrites with the ring generation.
        surfaces_.push_back(std::move(s));
        out.push_back(desc);
    }
    return RP_OK;
}

rp_result D3D11Backend::readback_surface_pixel(uint32_t index, uint32_t x, uint32_t y,
                                               uint8_t rgba_out[4], std::string& err) {
    if (index >= surfaces_.size()) { err = "bad index"; return RP_ERR_BAD_ARG; }
    Surface& s = surfaces_[index];
    if (FAILED(s.keyed->AcquireSync(1, 100))) { err = "acquire timeout"; return RP_ERR_TIMEOUT; }

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = width_; sd.Height = height_; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging))) {
        s.keyed->ReleaseSync(0); err = "staging create failed"; return RP_ERR_DEVICE;
    }
    ctx_->CopyResource(staging.Get(), s.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        s.keyed->ReleaseSync(0); err = "map failed"; return RP_ERR_DEVICE;
    }
    const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
    const uint8_t* px = row + x * 4;
    rgba_out[0]=px[0]; rgba_out[1]=px[1]; rgba_out[2]=px[2]; rgba_out[3]=px[3];
    ctx_->Unmap(staging.Get(), 0);
    s.keyed->ReleaseSync(0);
    return RP_OK;
}

rp_result D3D11Backend::composite_and_present(uint32_t, bool, uint8_t*, std::string& err) {
    err = "composite not implemented until Task 9";
    return RP_ERR_UNSUPPORTED;   // replaced in Task 9
}
}
```

- [ ] **Step 5: Wire CMake**

Top-level `CMakeLists.txt`: add the source and link D3D libs on Windows:
```cmake
target_sources(retropark PRIVATE src/render/d3d11/D3D11Backend.cpp)
if (WIN32)
  target_link_libraries(retropark PUBLIC d3d11 dxgi dxguid)
endif()
```
`tests/CMakeLists.txt`: add `test_d3d11_handoff.cpp`.

- [ ] **Step 6: Build and run — verify green (or SKIP)**

Run:
```bash
cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure
```
Expected: handoff test passes (red pixel read back), or emits the SKIP `WARN` if the environment lacks shared keyed-mutex support.

- [ ] **Step 7: Commit**

```bash
git add src/render/d3d11/D3D11Backend.* tests/test_d3d11_handoff.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: D3D11 backend device + shared keyed-mutex surface ring"
```

---

## Task 9: D3D11 compositor — core frame + blended overlay, headless pixel test

**Files:**
- Create: `src/render/d3d11/Shaders.h`, `src/render/d3d11/D3D11Compositor.h`, `src/render/d3d11/D3D11Compositor.cpp`
- Modify: `src/render/d3d11/D3D11Backend.h/.cpp` (own a compositor; implement `composite_and_present` for the headless readback path)
- Create: `tests/test_compositor.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `D3D11Backend` internals (device, context, `Surface` SRVs).
- Produces:
  - `class rp::D3D11Compositor` with `rp_result initialize(ID3D11Device*, std::string&)` and `rp_result render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* core_srv_or_null, uint32_t w, uint32_t h, std::string& err)`.
  - `composite_and_present` in `D3D11Backend`: for headless (`out_rgba != null`) it renders into an internal offscreen RTV, then copies to a staging texture and fills `out_rgba`. It acquires key 1 on the ready surface (if `has_frame`), composites, releases key 0.

The compositor draws: (1) a fullscreen triangle sampling `core_srv` (or a clear color if null), then (2) a blended overlay quad in the top-left quadrant with alpha 0.5 tinting toward blue — so a pixel under the overlay is provably a *blend* of the core color and the overlay color, not either alone.

- [ ] **Step 1: Write the failing headless composite test**

`tests/test_compositor.cpp`:
```cpp
#include <doctest/doctest.h>
#include "render/d3d11/D3D11Backend.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace rp;

// Fill the single core surface with solid green from a producer device, then
// composite; assert green shows outside the overlay and a blue-ward blend inside it.
TEST_CASE("compositor: core frame shows and overlay blends over it") {
    if (!D3D11Backend::probe_shared_keyed_mutex()) { WARN("no shared keyed mutex; skip"); return; }
    const uint32_t W=64, H=64;
    D3D11Backend host; std::string err;
    REQUIRE(host.initialize(nullptr, W, H, err) == RP_OK);
    std::vector<rp_surface_desc> descs;
    REQUIRE(host.allocate_surfaces(1, W, H, descs, err) == RP_OK);

    // Producer clears the shared surface green (key 0 -> release key 1).
    ComPtr<ID3D11Device> pdev; ComPtr<ID3D11DeviceContext> pctx; D3D_FEATURE_LEVEL fl;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &pdev, &fl, &pctx)));
    ComPtr<ID3D11Device1> pdev1; pdev.As(&pdev1);
    ComPtr<ID3D11Texture2D> ptex; REQUIRE(SUCCEEDED(pdev1->OpenSharedResource1(descs[0].shared_handle, IID_PPV_ARGS(&ptex))));
    ComPtr<IDXGIKeyedMutex> pkm; ptex.As(&pkm);
    REQUIRE(SUCCEEDED(pkm->AcquireSync(0, 100)));
    ComPtr<ID3D11RenderTargetView> prtv; REQUIRE(SUCCEEDED(pdev->CreateRenderTargetView(ptex.Get(), nullptr, &prtv)));
    const float green[4]={0,1,0,1}; pctx->ClearRenderTargetView(prtv.Get(), green); pctx->Flush();
    REQUIRE(SUCCEEDED(pkm->ReleaseSync(1)));

    std::vector<uint8_t> img(W*H*4, 0);
    REQUIRE(host.composite_and_present(/*ready_index=*/0, /*has_frame=*/true, img.data(), err) == RP_OK);

    auto at = [&](uint32_t x, uint32_t y, int c){ return img[(y*W + x)*4 + c]; };
    // Bottom-right quadrant: no overlay -> pure green.
    CHECK(at(60, 60, 1) > 200);            // G high
    CHECK(at(60, 60, 2) < 60);             // B low
    // Top-left quadrant: overlay blended over green -> blue raised, green reduced.
    CHECK(at(4, 4, 2) > 80);               // B raised by overlay
    CHECK(at(4, 4, 1) < at(60, 60, 1));    // G reduced vs the non-overlay region
}
```

- [ ] **Step 2: Write the shaders header**

`src/render/d3d11/Shaders.h`:
```cpp
#pragma once
// Minimal HLSL compiled at runtime with D3DCompile.
namespace rp {

// Fullscreen triangle; samples the core texture. If no core texture is bound the
// pipeline uses the clear color instead (handled on the C++ side).
static const char* kFullscreenVS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.uv = p;
    o.pos = float4(p * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
)";

static const char* kSamplePS = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(smp, uv);
}
)";

// Overlay quad: a constant-color, alpha-blended draw. Position/color come from a CB.
static const char* kOverlayVS = R"(
cbuffer Ov : register(b0) { float4 rect; float4 color; };  // rect = (x0,y0,x1,y1) in NDC
struct VSOut { float4 pos : SV_Position; };
VSOut main(uint vid : SV_VertexID) {
    float2 corners[4] = { float2(rect.x,rect.y), float2(rect.z,rect.y),
                          float2(rect.x,rect.w), float2(rect.z,rect.w) };
    VSOut o; o.pos = float4(corners[vid], 0, 1); return o;
}
)";

static const char* kOverlayPS = R"(
cbuffer Ov : register(b0) { float4 rect; float4 color; };
float4 main(float4 pos : SV_Position) : SV_Target { return color; }
)";
}
```

- [ ] **Step 3: Write the compositor header**

`src/render/d3d11/D3D11Compositor.h`:
```cpp
#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <string>
#include <retropark/retropark_abi.h>

namespace rp {
class D3D11Compositor {
public:
    rp_result initialize(ID3D11Device* dev, std::string& err);
    rp_result render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target,
                     ID3D11ShaderResourceView* core_srv, uint32_t w, uint32_t h, std::string& err);
private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> fs_vs_, ov_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  sample_ps_, ov_ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState>   blend_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       ov_cb_;
};
}
```

- [ ] **Step 4: Run — verify fail**

Run: `cmake --build build --config Debug`
Expected: FAIL — compositor + `composite_and_present` readback path unresolved/stubbed.

- [ ] **Step 5: Write the compositor implementation**

`src/render/d3d11/D3D11Compositor.cpp`:
```cpp
#include "render/d3d11/D3D11Compositor.h"
#include "render/d3d11/Shaders.h"
#include <d3dcompiler.h>
using Microsoft::WRL::ComPtr;

namespace rp {

struct OverlayCB { float rect[4]; float color[4]; };

static rp_result compile(const char* src, const char* target, ComPtr<ID3DBlob>& out, std::string& err) {
    ComPtr<ID3DBlob> errblob;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target,
                            0, 0, &out, &errblob);
    if (FAILED(hr)) { err = errblob ? (const char*)errblob->GetBufferPointer() : "compile failed"; return RP_ERR_DEVICE; }
    return RP_OK;
}

rp_result D3D11Compositor::initialize(ID3D11Device* dev, std::string& err) {
    ComPtr<ID3DBlob> b;
    rp_result r;
    if ((r = compile(kFullscreenVS, "vs_5_0", b, err)) != RP_OK) return r;
    if (FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &fs_vs_))) { err="fsVS"; return RP_ERR_DEVICE; }
    if ((r = compile(kSamplePS, "ps_5_0", b, err)) != RP_OK) return r;
    if (FAILED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &sample_ps_))) { err="samplePS"; return RP_ERR_DEVICE; }
    if ((r = compile(kOverlayVS, "vs_5_0", b, err)) != RP_OK) return r;
    if (FAILED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ov_vs_))) { err="ovVS"; return RP_ERR_DEVICE; }
    if ((r = compile(kOverlayPS, "ps_5_0", b, err)) != RP_OK) return r;
    if (FAILED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ov_ps_))) { err="ovPS"; return RP_ERR_DEVICE; }

    D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(dev->CreateSamplerState(&sd, &sampler_))) { err="sampler"; return RP_ERR_DEVICE; }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &blend_))) { err="blend"; return RP_ERR_DEVICE; }

    D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof(OverlayCB); cb.Usage=D3D11_USAGE_DEFAULT;
    cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&cb, nullptr, &ov_cb_))) { err="cb"; return RP_ERR_DEVICE; }
    return RP_OK;
}

rp_result D3D11Compositor::render(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target,
                                  ID3D11ShaderResourceView* core_srv, uint32_t w, uint32_t h, std::string& err) {
    (void)err;
    const float clear[4] = {0,0,0,1};
    ctx->ClearRenderTargetView(target, clear);
    ctx->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT vp{0,0,(float)w,(float)h,0,1}; ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);

    // Pass 1: core fullscreen (opaque) if we have a frame.
    if (core_srv) {
        float noBlend[4]={0,0,0,0};
        ctx->OMSetBlendState(nullptr, noBlend, 0xffffffff);
        ctx->VSSetShader(fs_vs_.Get(), nullptr, 0);
        ctx->PSSetShader(sample_ps_.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &core_srv);
        ID3D11SamplerState* s = sampler_.Get(); ctx->PSSetSamplers(0, 1, &s);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullsrv=nullptr; ctx->PSSetShaderResources(0,1,&nullsrv);
    }

    // Pass 2: blended overlay quad in the top-left quadrant, alpha 0.5 toward blue.
    OverlayCB data{};
    data.rect[0]=-1.0f; data.rect[1]=1.0f; data.rect[2]=0.0f; data.rect[3]=0.0f; // NDC top-left quadrant
    data.color[0]=0.0f; data.color[1]=0.0f; data.color[2]=1.0f; data.color[3]=0.5f;
    ctx->UpdateSubresource(ov_cb_.Get(), 0, nullptr, &data, 0, 0);
    float bf[4]={0,0,0,0};
    ctx->OMSetBlendState(blend_.Get(), bf, 0xffffffff);
    ctx->VSSetShader(ov_vs_.Get(), nullptr, 0);
    ID3D11Buffer* cb = ov_cb_.Get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShader(ov_ps_.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->Draw(4, 0);
    return RP_OK;
}
}
```

- [ ] **Step 6: Extend `D3D11Backend` to own the compositor and implement the headless composite path**

In `D3D11Backend.h`, add includes and members:
```cpp
#include "render/d3d11/D3D11Compositor.h"
// ... inside class, protected:
    D3D11Compositor compositor_;
    bool compositor_ready_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> offscreen_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> offscreen_rtv_;
```

In `D3D11Backend.cpp`, replace `composite_and_present`:
```cpp
rp_result D3D11Backend::composite_and_present(uint32_t ready_index, bool has_frame,
                                              uint8_t* out_rgba, std::string& err) {
    if (!compositor_ready_) {
        rp_result r = compositor_.initialize(device_.Get(), err);
        if (r != RP_OK) return r;
        compositor_ready_ = true;
    }
    // Ensure an offscreen RTV of the current size (headless target).
    if (!offscreen_) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width=width_; d.Height=height_; d.MipLevels=1; d.ArraySize=1;
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1;
        d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &offscreen_))) { err="offscreen"; return RP_ERR_DEVICE; }
        if (FAILED(device_->CreateRenderTargetView(offscreen_.Get(), nullptr, &offscreen_rtv_))) { err="offRTV"; return RP_ERR_DEVICE; }
    }

    ID3D11ShaderResourceView* core_srv = nullptr;
    bool acquired = false;
    if (has_frame && ready_index < surfaces_.size()) {
        if (FAILED(surfaces_[ready_index].keyed->AcquireSync(1, 100))) { err="acquire timeout"; return RP_ERR_TIMEOUT; }
        acquired = true;
        core_srv = surfaces_[ready_index].srv.Get();
    }

    rp_result r = compositor_.render(ctx_.Get(), offscreen_rtv_.Get(), core_srv, width_, height_, err);

    if (acquired) surfaces_[ready_index].keyed->ReleaseSync(0);
    if (r != RP_OK) return r;

    if (out_rgba) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width=width_; sd.Height=height_; sd.MipLevels=1; sd.ArraySize=1;
        sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count=1;
        sd.Usage=D3D11_USAGE_STAGING; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging))) { err="composite staging"; return RP_ERR_DEVICE; }
        ctx_->CopyResource(staging.Get(), offscreen_.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { err="composite map"; return RP_ERR_DEVICE; }
        for (uint32_t y=0; y<height_; ++y)
            memcpy(out_rgba + y*width_*4, (const uint8_t*)m.pData + y*m.RowPitch, width_*4);
        ctx_->Unmap(staging.Get(), 0);
    }
    return RP_OK;
}
```

- [ ] **Step 7: Wire CMake**

Add `src/render/d3d11/D3D11Compositor.cpp` to `retropark`; link `d3dcompiler`:
```cmake
if (WIN32)
  target_link_libraries(retropark PUBLIC d3d11 dxgi dxguid d3dcompiler)
endif()
```
Add `test_compositor.cpp` to tests.

- [ ] **Step 8: Build and run — verify green (or SKIP)**

Run:
```bash
cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure
```
Expected: composite test passes — green outside the overlay, blue-ward blend inside it (proving compositing, not layering).

- [ ] **Step 9: Commit**

```bash
git add src/render/d3d11/ tests/test_compositor.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: D3D11 compositor with blended overlay over core frame"
```

---

## Task 10: Reference presenting core (`refcore_present.dll`)

**Files:**
- Create: `cores/refcore_present/RefCore.cpp`, `cores/refcore_present/core.json`, `cores/refcore_present/CMakeLists.txt`
- Modify: top-level `CMakeLists.txt` (`add_subdirectory(cores/refcore_present)`)

**Interfaces:**
- Consumes: `retropark_abi.h`, `rp_host_iface`, `rp_surface_desc`.
- Produces: a DLL exporting `rp_get_core_abi`. On `start`, it launches its **own thread** that owns **its own D3D11 device**, opens each shared surface handle, and every ~16 ms renders an animated color (a time-varying solid fill is sufficient to prove the path) into the next ring slot, then calls `host->submit_frame(index, generation)`.

- [ ] **Step 1: Write the manifest**

`cores/refcore_present/core.json`:
```json
{
  "id": "refcore_present",
  "name": "Reference Presenting Core",
  "type": "presenting",
  "abi_version": 1,
  "graphics_api": "d3d11",
  "entry": "refcore_present.dll"
}
```

- [ ] **Step 2: Write the core**

`cores/refcore_present/RefCore.cpp`:
```cpp
#include <retropark/retropark_abi.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <vector>
using Microsoft::WRL::ComPtr;

#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {
struct Slot {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<IDXGIKeyedMutex> keyed;
    ComPtr<ID3D11RenderTargetView> rtv;
    uint32_t index = 0;
    uint64_t generation = 0;
};

struct RefCore {
    rp_host_iface host{};
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11DeviceContext> ctx;
    std::vector<Slot> slots;
    std::thread th;
    std::atomic<bool> running{false};
    uint32_t cursor = 0;
    uint32_t frame = 0;

    void loop() {
        while (running.load()) {
            if (!slots.empty()) {
                Slot& s = slots[cursor];
                cursor = (cursor + 1) % (uint32_t)slots.size();
                if (SUCCEEDED(s.keyed->AcquireSync(0, 100))) {
                    float t = (float)((frame++ % 120)) / 120.0f;   // animate
                    const float col[4] = { 0.0f, 1.0f, t, 1.0f };  // green with rising blue
                    ctx->ClearRenderTargetView(s.rtv.Get(), col);
                    ctx->Flush();
                    s.keyed->ReleaseSync(1);
                    host.submit_frame(host.host, s.index, s.generation);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
};

void ref_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_PRESENTING;
    out->graphics_api = RP_GFX_D3D11;
    out->id = "refcore_present";
}

rp_core* ref_create(const rp_host_iface* host) {
    auto* c = new RefCore();
    c->host = *host;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &c->dev, &fl, &c->ctx))) { delete c; return nullptr; }
    if (FAILED(c->dev.As(&c->dev1))) { delete c; return nullptr; }
    return reinterpret_cast<rp_core*>(c);
}

void ref_destroy(rp_core* core) {
    auto* c = reinterpret_cast<RefCore*>(core);
    if (c->running.load()) { c->running = false; if (c->th.joinable()) c->th.join(); }
    delete c;
}

rp_result ref_set_surfaces(rp_core* core, const rp_surface_desc* descs, uint32_t count) {
    auto* c = reinterpret_cast<RefCore*>(core);
    c->slots.clear();
    for (uint32_t i = 0; i < count; ++i) {
        Slot s; s.index = descs[i].index; s.generation = descs[i].generation;
        if (FAILED(c->dev1->OpenSharedResource1((HANDLE)descs[i].shared_handle, IID_PPV_ARGS(&s.tex))))
            return RP_ERR_DEVICE;
        if (FAILED(s.tex.As(&s.keyed))) return RP_ERR_DEVICE;
        if (FAILED(c->dev->CreateRenderTargetView(s.tex.Get(), nullptr, &s.rtv))) return RP_ERR_DEVICE;
        c->slots.push_back(std::move(s));
    }
    c->cursor = 0;
    return RP_OK;
}

rp_result ref_start(rp_core* core) {
    auto* c = reinterpret_cast<RefCore*>(core);
    if (c->running.load()) return RP_OK;
    c->running = true;
    c->th = std::thread([c]{ c->loop(); });
    return RP_OK;
}

rp_result ref_stop(rp_core* core) {
    auto* c = reinterpret_cast<RefCore*>(core);
    c->running = false;
    if (c->th.joinable()) c->th.join();
    return RP_OK;
}

const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, ref_get_info, ref_create, ref_destroy,
    ref_set_surfaces, ref_start, ref_stop
};
}

RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
```

- [ ] **Step 3: Write the core's CMake**

`cores/refcore_present/CMakeLists.txt`:
```cmake
add_library(refcore_present SHARED RefCore.cpp)
target_include_directories(refcore_present PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(refcore_present PRIVATE d3d11 dxgi dxguid)
# Emit dll + manifest next to a cores/ dir under the build tree for the harness/e2e.
set(RP_CORE_OUT $<TARGET_FILE_DIR:refcore_present>/cores/refcore_present)
add_custom_command(TARGET refcore_present POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${RP_CORE_OUT}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:refcore_present> ${RP_CORE_OUT}/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/core.json ${RP_CORE_OUT}/)
```

Top-level `CMakeLists.txt`: add `add_subdirectory(cores/refcore_present)`.

- [ ] **Step 4: Build — verify the dll and manifest are emitted**

Run:
```bash
cmake -S . -B build && cmake --build build --config Debug
```
Expected: `refcore_present.dll` and `core.json` exist under `build/.../cores/refcore_present/`. (Behavior is asserted end-to-end in Task 12.)

- [ ] **Step 5: Commit**

```bash
git add cores/refcore_present/ CMakeLists.txt
git commit -m "feat: reference presenting core with own device and render thread"
```

---

## Task 11: Runtime — public C API tying loader + backend + ring + input

**Files:**
- Create: `include/retropark/retropark.h`, `src/runtime/Runtime.h`, `src/runtime/Runtime.cpp`
- Create: `tests/test_runtime_api.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `CoreLoader`, `Win32CoreModule`, `Manifest`, `SurfaceRing`, `D3D11Backend`.
- Produces the public C API (`retropark.h`):
  - `rp_runtime* rp_runtime_create(rp_graphics_api api, void* native_window);`
  - `void rp_runtime_destroy(rp_runtime*);`
  - `rp_result rp_runtime_load_core(rp_runtime*, const char* core_dir);`
  - `rp_result rp_runtime_unload_core(rp_runtime*);`
  - `rp_result rp_runtime_resize(rp_runtime*, uint32_t w, uint32_t h);`
  - `void rp_runtime_set_input(rp_runtime*, const rp_input_state*);`
  - `rp_result rp_runtime_present(rp_runtime*, uint8_t* out_rgba);`

The runtime installs the `rp_host_iface` whose `submit_frame` forwards to `SurfaceRing::accept_submit`, and whose `input_state` copies the last `rp_runtime_set_input` snapshot. On `load_core`: read `<core_dir>/core.json`, reject non-`presenting` types with `RP_ERR_UNSUPPORTED` (driven not implemented), `LoadLibrary` `<core_dir>/<entry>`, run loader `load`+`create`, allocate the ring surfaces from the backend, stamp each `rp_surface_desc.generation` with the ring generation, hand them to the core via `set_surfaces`, then `start`.

- [ ] **Step 1: Write the public header**

`include/retropark/retropark.h`:
```c
#ifndef RETROPARK_H
#define RETROPARK_H

#include <stdint.h>
#include "retropark_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rp_runtime rp_runtime;

rp_runtime* rp_runtime_create(rp_graphics_api api, void* native_window);
void        rp_runtime_destroy(rp_runtime* rt);

rp_result   rp_runtime_load_core(rp_runtime* rt, const char* core_dir);
rp_result   rp_runtime_unload_core(rp_runtime* rt);

rp_result   rp_runtime_resize(rp_runtime* rt, uint32_t width, uint32_t height);
void        rp_runtime_set_input(rp_runtime* rt, const rp_input_state* in);

/* Composite latest core frame + overlay; if out_rgba != NULL copies the RGBA8 image. */
rp_result   rp_runtime_present(rp_runtime* rt, uint8_t* out_rgba);

#ifdef __cplusplus
}
#endif
#endif /* RETROPARK_H */
```

- [ ] **Step 2: Write the failing test (API surface + input echo, no core)**

`tests/test_runtime_api.cpp`:
```cpp
#include <doctest/doctest.h>
#include <retropark/retropark.h>

TEST_CASE("runtime: create/resize/destroy headless") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt != nullptr);
    CHECK(rp_runtime_resize(rt, 64, 64) == RP_OK);
    rp_runtime_destroy(rt);
}

TEST_CASE("runtime: loading a non-existent core dir fails cleanly") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    CHECK(rp_runtime_load_core(rt, "no_such_dir") == RP_ERR_NOT_FOUND);
    rp_runtime_destroy(rt);
}
```

- [ ] **Step 3: Write `Runtime.h`**

`src/runtime/Runtime.h`:
```cpp
#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <retropark/retropark.h>
#include "loader/CoreLoader.h"
#include "loader/Win32CoreModule.h"
#include "render/SurfaceRing.h"
#include "render/d3d11/D3D11Backend.h"

namespace rp {
class Runtime {
public:
    Runtime(rp_graphics_api api, void* native_window);
    ~Runtime();
    rp_result load_core(const std::string& core_dir);
    rp_result unload_core();
    rp_result resize(uint32_t w, uint32_t h);
    void set_input(const rp_input_state& in);
    rp_result present(uint8_t* out_rgba);

    // Host-iface trampolines.
    void on_submit(uint32_t index, uint64_t generation);
    void on_input(rp_input_state* out);

private:
    rp_result rebuild_surfaces(std::string& err);

    void* native_window_ = nullptr;
    std::unique_ptr<D3D11Backend> backend_;
    std::unique_ptr<Win32CoreModule> module_;
    CoreLoader loader_;
    SurfaceRing ring_{3};
    rp_host_iface host_iface_{};
    rp_input_state input_{};
    std::mutex input_mtx_;
    uint32_t width_ = 64, height_ = 64;
    bool core_loaded_ = false;
};
}
```

- [ ] **Step 4: Write `Runtime.cpp` + the C API shims**

`src/runtime/Runtime.cpp`:
```cpp
#include "runtime/Runtime.h"
#include "loader/Manifest.h"
#include <fstream>
#include <sstream>
#include <cstring>

namespace rp {

static void host_log(rp_host*, int, const char*) {}
static void host_submit(rp_host* h, uint32_t i, uint64_t g) {
    reinterpret_cast<Runtime*>(h)->on_submit(i, g);
}
static void host_input(rp_host* h, rp_input_state* out) {
    reinterpret_cast<Runtime*>(h)->on_input(out);
}

Runtime::Runtime(rp_graphics_api, void* native_window) : native_window_(native_window) {
    backend_ = std::make_unique<D3D11Backend>();
    std::string err;
    backend_->initialize(native_window_, width_, height_, err);
    host_iface_.host = reinterpret_cast<rp_host*>(this);
    host_iface_.log = host_log;
    host_iface_.submit_frame = host_submit;
    host_iface_.input_state = host_input;
}

Runtime::~Runtime() { unload_core(); }

void Runtime::on_submit(uint32_t index, uint64_t generation) {
    ring_.accept_submit(index, generation);
}
void Runtime::on_input(rp_input_state* out) {
    std::lock_guard<std::mutex> lk(input_mtx_);
    *out = input_;
}
void Runtime::set_input(const rp_input_state& in) {
    std::lock_guard<std::mutex> lk(input_mtx_);
    input_ = in;
}

rp_result Runtime::rebuild_surfaces(std::string& err) {
    std::vector<rp_surface_desc> descs;
    rp_result r = backend_->allocate_surfaces(ring_.slot_count(), width_, height_, descs, err);
    if (r != RP_OK) return r;
    uint64_t gen = ring_.reallocate(width_, height_);
    for (auto& d : descs) d.generation = gen;
    if (loader_.state() == LoaderState::Started) { std::string e; loader_.stop(e); }
    if (loader_.state() == LoaderState::Created)
        return loader_.set_surfaces(descs.data(), (uint32_t)descs.size(), err);
    return RP_OK;
}

rp_result Runtime::resize(uint32_t w, uint32_t h) {
    width_ = w; height_ = h;
    std::string err;
    if (!core_loaded_) return backend_->initialize(native_window_, w, h, err);
    rp_result r = rebuild_surfaces(err);
    if (r != RP_OK) return r;
    if (loader_.state() == LoaderState::Created) return loader_.start(err);
    return RP_OK;
}

rp_result Runtime::load_core(const std::string& core_dir) {
    std::string manifest_path = core_dir + "/core.json";
    std::ifstream f(manifest_path, std::ios::binary);
    if (!f) return RP_ERR_NOT_FOUND;
    std::stringstream ss; ss << f.rdbuf();
    CoreManifest m; std::string err;
    if (parse_manifest(ss.str(), m, err) != RP_OK) return RP_ERR_BAD_ARG;
    if (m.type != RP_CORE_PRESENTING) return RP_ERR_UNSUPPORTED; // driven not in Slice A

    std::string dll = core_dir + "/" + m.entry;
    if (Win32CoreModule::open(dll, module_, err) != RP_OK) return RP_ERR_NOT_FOUND;
    if (loader_.load(module_.get(), err) != RP_OK) return RP_ERR_ABI_MISMATCH;
    if (loader_.create(&host_iface_, err) != RP_OK) return RP_ERR_INTERNAL;

    core_loaded_ = true;
    rp_result r = rebuild_surfaces(err);
    if (r != RP_OK) return r;
    return loader_.start(err);
}

rp_result Runtime::unload_core() {
    if (!core_loaded_) return RP_OK;
    loader_.destroy();
    module_.reset();
    core_loaded_ = false;
    return RP_OK;
}

rp_result Runtime::present(uint8_t* out_rgba) {
    uint32_t idx = 0;
    bool has = ring_.latest_ready(idx);
    std::string err;
    return backend_->composite_and_present(idx, has, out_rgba, err);
}

} // namespace rp

// ---- C API ----
using rp::Runtime;
extern "C" {

rp_runtime* rp_runtime_create(rp_graphics_api api, void* native_window) {
    return reinterpret_cast<rp_runtime*>(new Runtime(api, native_window));
}
void rp_runtime_destroy(rp_runtime* rt) { delete reinterpret_cast<Runtime*>(rt); }
rp_result rp_runtime_load_core(rp_runtime* rt, const char* dir) {
    return reinterpret_cast<Runtime*>(rt)->load_core(dir ? dir : "");
}
rp_result rp_runtime_unload_core(rp_runtime* rt) {
    return reinterpret_cast<Runtime*>(rt)->unload_core();
}
rp_result rp_runtime_resize(rp_runtime* rt, uint32_t w, uint32_t h) {
    return reinterpret_cast<Runtime*>(rt)->resize(w, h);
}
void rp_runtime_set_input(rp_runtime* rt, const rp_input_state* in) {
    if (in) reinterpret_cast<Runtime*>(rt)->set_input(*in);
}
rp_result rp_runtime_present(rp_runtime* rt, uint8_t* out_rgba) {
    return reinterpret_cast<Runtime*>(rt)->present(out_rgba);
}
}
```

- [ ] **Step 5: Wire CMake, build, run**

Add `src/runtime/Runtime.cpp` to `retropark`; add `test_runtime_api.cpp` to tests.
Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: runtime API tests pass (create/resize/destroy; missing-dir returns `RP_ERR_NOT_FOUND`).

- [ ] **Step 6: Commit**

```bash
git add include/retropark/retropark.h src/runtime/ tests/test_runtime_api.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: runtime C API wiring loader, backend, ring, and input"
```

---

## Task 12: End-to-end — load the real reference core and assert a composited frame

**Files:**
- Create: `tests/test_e2e.cpp`
- Modify: `tests/CMakeLists.txt` (depend on `refcore_present`, know its output dir)

**Interfaces:**
- Consumes: the public C API + the built `refcore_present` core package.
- Produces: the single provable claim of Slice A, asserted automatically.

- [ ] **Step 1: Write the failing e2e test**

`tests/test_e2e.cpp`:
```cpp
#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/d3d11/D3D11Backend.h"
#include <vector>
#include <thread>
#include <chrono>
#include <string>

#ifndef RP_CORE_DIR
#define RP_CORE_DIR "cores/refcore_present"   // relative to test CWD; overridden by CMake
#endif

TEST_CASE("e2e: reference core renders into our surface and we composite an overlay") {
    if (!rp::D3D11Backend::probe_shared_keyed_mutex()) { WARN("no shared keyed mutex; skip"); return; }
    const uint32_t W=64, H=64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_CORE_DIR) == RP_OK);

    // Give the core thread a few frames to submit.
    std::vector<uint8_t> img(W*H*4, 0);
    rp_result pr = RP_ERR_INTERNAL;
    bool sawCore = false;
    for (int i = 0; i < 60 && !sawCore; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        pr = rp_runtime_present(rt, img.data());
        if (pr != RP_OK) continue;
        // Bottom-right quadrant should carry the core's green once a frame lands.
        uint8_t g = img[((H-4)*W + (W-4))*4 + 1];
        if (g > 150) sawCore = true;
    }
    CHECK(pr == RP_OK);
    CHECK(sawCore);

    auto at=[&](uint32_t x,uint32_t y,int c){ return img[(y*W+x)*4+c]; };
    // Overlay quadrant (top-left) shows a blue-ward blend over the core.
    CHECK(at(4,4,2) > 80);
    CHECK(at(4,4,1) < at(W-4,H-4,1));

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}
```

- [ ] **Step 2: Wire CMake so the test finds the core package**

`tests/CMakeLists.txt`:
```cmake
add_dependencies(retropark_tests refcore_present)
target_compile_definitions(retropark_tests PRIVATE
  RP_CORE_DIR="$<TARGET_FILE_DIR:refcore_present>/cores/refcore_present")
```
Add `test_e2e.cpp` to the test sources.

- [ ] **Step 3: Build and run — verify green (or SKIP)**

Run:
```bash
cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure
```
Expected: e2e test passes — the reference core (own device + own thread) rendered green into a host surface, and the host composited a blue-ward overlay blend over it.

- [ ] **Step 4: Commit**

```bash
git add tests/test_e2e.cpp tests/CMakeLists.txt
git commit -m "test: end-to-end presenting core + overlay composite"
```

---

## Task 13: Windowed Win32 smoke harness

**Files:**
- Create: `harness/windowed/main.cpp`, `harness/windowed/CMakeLists.txt`
- Modify: `src/render/d3d11/D3D11Backend.h/.cpp` (windowed swapchain + present path)
- Modify: top-level `CMakeLists.txt` (`add_subdirectory(harness/windowed)`)

**Interfaces:**
- Consumes: the public C API; `D3D11Backend` gains a swapchain when `initialize` receives a real `HWND`.
- Produces: `retropark_harness.exe` — creates a window, loads `refcore_present`, and runs a present loop so a human sees the animated core with the overlay.

- [ ] **Step 1: Extend the backend with a windowed present path**

In `D3D11Backend::initialize`, when `native_window != nullptr`, create a swapchain via `IDXGIFactory2::CreateSwapChainForHwnd` and keep a back-buffer RTV. Add a member:
```cpp
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backbuffer_rtv_;
```
In `composite_and_present`, when `swapchain_` is non-null, render into `backbuffer_rtv_` (instead of the offscreen RTV) and call `swapchain_->Present(1, 0)` after compositing. Reuse the same acquire/compose/release sequence. Implementation:
```cpp
// inside initialize(), headless == false branch:
ComPtr<IDXGIDevice> dxgiDev; device_.As(&dxgiDev);
ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(&adapter);
ComPtr<IDXGIFactory2> factory; adapter->GetParent(IID_PPV_ARGS(&factory));
DXGI_SWAP_CHAIN_DESC1 sc{}; sc.Width=w; sc.Height=h; sc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
sc.SampleDesc.Count=1; sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sc.BufferCount=2;
sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), (HWND)native_window, &sc, nullptr, nullptr, &swapchain_))) {
    err="swapchain"; return RP_ERR_DEVICE;
}
ComPtr<ID3D11Texture2D> bb; swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
device_->CreateRenderTargetView(bb.Get(), nullptr, &backbuffer_rtv_);
```
And in `composite_and_present`, choose the target:
```cpp
ID3D11RenderTargetView* target = swapchain_ ? backbuffer_rtv_.Get() : offscreen_rtv_.Get();
// ... compositor_.render(ctx_.Get(), target, core_srv, width_, height_, err);
// after render + ReleaseSync:
if (swapchain_) swapchain_->Present(1, 0);
```
(Keep the offscreen/readback branch for `out_rgba != null`.)

- [ ] **Step 2: Write the harness**

`harness/windowed/main.cpp`:
```cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <retropark/retropark.h>
#include <string>

static rp_runtime* g_rt = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"RetroParkHarness";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"RetroPark Slice A",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, hInst, nullptr);

    g_rt = rp_runtime_create(RP_GFX_D3D11, hwnd);
    rp_runtime_resize(g_rt, 640, 480);
    // Core dir is passed at build time; fall back to a relative path.
#ifndef RP_HARNESS_CORE_DIR
#define RP_HARNESS_CORE_DIR "cores/refcore_present"
#endif
    rp_runtime_load_core(g_rt, RP_HARNESS_CORE_DIR);

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        rp_runtime_present(g_rt, nullptr);   // present to the window
    }
    rp_runtime_unload_core(g_rt);
    rp_runtime_destroy(g_rt);
    return 0;
}
```

- [ ] **Step 3: Write the harness CMake**

`harness/windowed/CMakeLists.txt`:
```cmake
add_executable(retropark_harness WIN32 main.cpp)
target_link_libraries(retropark_harness PRIVATE retropark)
add_dependencies(retropark_harness refcore_present)
target_compile_definitions(retropark_harness PRIVATE
  RP_HARNESS_CORE_DIR="$<TARGET_FILE_DIR:refcore_present>/cores/refcore_present")
```
Top-level `CMakeLists.txt`: add `add_subdirectory(harness/windowed)`.

- [ ] **Step 4: Build and smoke — verify a window shows the animated core + overlay**

Run:
```bash
cmake -S . -B build && cmake --build build --config Debug
```
Then launch `build/harness/windowed/Debug/retropark_harness.exe` (or wherever CMake emits it).
Expected: a window shows an animated green→blue fill (the presenting core, via its own device/thread) with a blue-tinted overlay quad in the top-left. Close the window to exit cleanly.

- [ ] **Step 5: Commit**

```bash
git add harness/windowed/ src/render/d3d11/D3D11Backend.* CMakeLists.txt
git commit -m "feat: windowed Win32 harness presenting the reference core with overlay"
```

---

## Self-Review

**Spec coverage:**
- Standalone C++ lib + own C API → Tasks 1, 11 (`retropark.h`, `Runtime`). ✓
- Flat C core ABI → Task 2 (`retropark_abi.h`), C-compile proof. ✓
- Two execution models, presenting proven / driven declared → ABI enums (Task 2), manifest accepts driven (Task 3), runtime rejects driven with `RP_ERR_UNSUPPORTED` (Task 11), presenting proven e2e (Task 12). ✓
- Render abstraction, D3D11 first → `IRenderBackend` (Task 7), `D3D11Backend` (Tasks 8–9). ✓
- Surface + compositing boundary → shared-texture ring + keyed mutex (Task 8), compositor blends overlay (Task 9). ✓
- Ring of 2–3 shared textures + generation + stale-frame drop → `SurfaceRing` (Task 6), used in `Runtime` (Task 11). ✓
- Keyed-mutex producer/consumer convention → Global Constraints; core uses Acquire(0)/Release(1) (Task 10), host uses Acquire(1)/Release(0) (Tasks 8–9). ✓
- Loader + manifest + lifecycle → Tasks 3, 4, 5. ✓
- Error handling: ABI mismatch (Task 4), missing exports (Task 4/5), acquire timeouts (Tasks 8–9), stale generation drop (Task 6), missing dll/dir (Tasks 5, 11). ✓ Crash-honesty SEH note: documented as out-of-scope for behavior but see below.
- Reference presenting core with own device + own thread → Task 10. ✓
- Windowed + headless harness + pixel tests → headless pixel tests (Tasks 8, 9, 12), windowed harness (Task 13). ✓
- CMake build → Task 1. ✓
- Audio declared, not implemented → the ABI has no audio hooks in Slice A and the reference core is silent; audio is explicitly deferred in the spec. Consistent (nothing to build). ✓

**Gap found and addressed:** the spec's error-handling section calls for **SEH guards** (`__try/__except`) around calls into the core. No task added them, because `__try/__except` cannot wrap C++ objects with destructors (the two are incompatible in the same function), which would force an awkward refactor of `CoreLoader`. **Resolution:** SEH crash-guarding is deferred to the same later slice as full out-of-process isolation (the spec already frames real isolation as later work, and SEH is only a partial mitigation). This plan implements the *structured-error* half of the spec's error handling (specific `rp_result` codes at every boundary, timeouts, stale-frame drops) and does **not** claim crash isolation — matching the spec's "this is not sandboxing" honesty. This deferral is noted so it isn't mistaken for a miss.

**Placeholder scan:** no TBD/TODO/"handle edge cases" — every code step carries complete code. ✓

**Type consistency:** `rp_core_abi` field order and signatures are identical across Task 2 (definition), the fake (Task 4), the mock (Task 5), and the reference core (Task 10). `composite_and_present(uint32_t, bool, uint8_t*, std::string&)`, `allocate_surfaces(...)`, and `SurfaceRing` method names match between definition and all call sites. `rp_result` codes referenced in tests (`RP_ERR_NOT_FOUND`, `RP_ERR_UNSUPPORTED`, `RP_ERR_ABI_MISMATCH`, `RP_ERR_TIMEOUT`) all exist in the Task 2 enum. ✓
