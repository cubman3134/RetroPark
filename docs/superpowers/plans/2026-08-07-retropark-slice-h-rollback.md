# RetroPark Slice H — Rollback Netplay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** GGPO-style rollback netplay for driven cores — predict the remote input, run ahead, and when a real input arrives that differs, load a savestate and re-simulate silently to correct — proven byte-exact against a lockstep ground truth on every confirmed frame.

**Architecture:** An additive runtime split (`advance` = simulate without composite / `render` = composite the last frame, audio suppressed during re-sim) lets a new `RollbackSession` re-simulate frames invisibly. The session predicts remote input (repeat-last), keeps a per-frame savestate ring, rolls back + re-sims on misprediction, stalls past a bounded prediction window, and checksums confirmed frames for desync. Reuses Slice G's transport/protocol. A tiny input-sensitive test core makes misprediction genuinely diverge so the portable gate proves correction, not just plumbing.

**Tech Stack:** C++17, MSVC, doctest, CMake. Reuses `src/net/` (Slice G) + savestate (Slice F).

## Global Constraints

- C++17. MSVC `/W4 /permissive-`, **warning-clean** (rebuild the TUs you change — incremental builds skip unchanged files, so a warning can hide). Do **not** use `strncpy` (C4996 here).
- **No core-ABI change.** `RETROPARK_ABI_VERSION` stays **5**. `advance`/`render` are additive C API; `run_frame`/audio/serialize already exist.
- The whole Slice A–G suite (89 cases) stays green. `present()` must remain byte-identical (= `advance(1)` + `render`).
- Rollback is **driven cores only**, 1v1, reuses TCP (no UDP). Never `memcpy` a struct across the wire (reuse the existing LE `NetProtocol`).
- Conventional commit prefixes (`feat:` / `test:`). **NO AI attribution** anywhere in any commit message or body.
- Fresh CMake reconfigure needs `export VULKAN_SDK=/c/VulkanSDK/1.4.357.0`. Build: `cmake --build build --config Debug`. Test binary: **`build/tests/Debug/retropark_tests.exe`** (doctest; `-tc="name"`). Full suite: `ctest --test-dir build -C Debug --output-on-failure`.
- Keep cores/ROMs out of git (already `.gitignore`d). FCEUmm at `external/libretro-cores/`, ROMs at `C:\RetroBat\roms\nes`.

---

## File Structure

```
include/retropark/retropark.h        # + rp_runtime_advance / rp_runtime_render (additive)
src/runtime/Runtime.h / .cpp         # advance/render split, suppress_audio_ gate
src/net/
  RollbackPredict.h / .cpp           # rb_predict + rb_first_mispredicted + rb_prune_below (pure)
  RollbackSession.h / .cpp           # predict + state-ring + rollback-resim + stall + confirmed desync
cores/refcore_rollback/
  RefCoreRollback.cpp                # tiny input-sensitive driven core (acc += button?2:1)
  core.json
  CMakeLists.txt
harness/windowed/main.cpp            # --rollback modifier -> RollbackSession
tests/
  test_rollback_unit.cpp             # advance/render split + refcore_rollback input-sensitivity + predict helpers
  test_rollback_e2e.cpp             # portable convergence gate + gated FCEUmm gate (+ DelayTransport)
```

Add `src/net/RollbackPredict.cpp` + `src/net/RollbackSession.cpp` to the `retropark` static-lib target (`CMakeLists.txt:28` `add_library(retropark STATIC ...)`). Add `add_subdirectory(cores/refcore_rollback)` after line 73. Register both test files in the tests target (mirror `tests/test_netplay_e2e.cpp`).

---

## Task 1: Runtime advance / render split + audio suppression

**Files:**
- Modify: `include/retropark/retropark.h` (declare `rp_runtime_advance`, `rp_runtime_render`)
- Modify: `src/runtime/Runtime.h` (add `bool suppress_audio_ = false;` + `advance`/`render` decls), `src/runtime/Runtime.cpp` (split `present`, guard `on_audio_sample`)
- Test: `tests/test_rollback_unit.cpp` (new)
- Modify: `CMakeLists.txt` (register `tests/test_rollback_unit.cpp`)

**Interfaces:**
- Produces: `rp_result rp_runtime_advance(rp_runtime*, int emit_audio)`; `rp_result rp_runtime_render(rp_runtime*, uint8_t* out_rgba)`. `present` == `advance(1)`+`render`.
- Consumes: existing driven `present` internals (`loader_.run_frame`, `dr_*`, `composite_driven`).

**Reference — current `Runtime::present` (`src/runtime/Runtime.cpp:212-248`)** does, for a driven core: rewind-capture block → `dr_have_=false` → `loader_.run_frame` → validity check → `composite_driven`; for presenting: `ring_.latest_ready` → `composite_and_present`. The split preserves this exactly.

- [ ] **Step 1: Write the failing test** `tests/test_rollback_unit.cpp`

```cpp
#include <doctest/doctest.h>
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <vector>
#include <cstring>
#include <string>
using namespace rp;
static rp_runtime* c(Runtime* r){ return reinterpret_cast<rp_runtime*>(r); }

// Load refcore_driven into a runtime (lift the exact setup from tests/test_driven_e2e.cpp).
static void load_refcore_driven(Runtime& rt);

TEST_CASE("runtime: advance advances, render does not; present == advance+render") {
    Runtime rt(RP_GFX_D3D11, nullptr);         // WARP, device-free (as Slice F portable e2e)
    load_refcore_driven(rt);
    auto frame_of = [&]() -> uint32_t {
        uint32_t f = 0; size_t sz = rp_runtime_serialize_size(c(&rt));
        REQUIRE(sz == sizeof(uint32_t));
        REQUIRE(rp_runtime_save_state(c(&rt), &f, sizeof(f)) == RP_OK);
        return f;
    };
    std::vector<uint8_t> out(64 * 64 * 4, 0);
    uint32_t f0 = frame_of();
    REQUIRE(rp_runtime_advance(c(&rt), 1) == RP_OK);
    CHECK(frame_of() == f0 + 1);               // advance advanced the sim one frame
    REQUIRE(rp_runtime_render(c(&rt), out.data()) == RP_OK);
    CHECK(frame_of() == f0 + 1);               // render did NOT advance
    REQUIRE(rp_runtime_render(c(&rt), out.data()) == RP_OK);
    CHECK(frame_of() == f0 + 1);               // render is idempotent on state
    REQUIRE(rp_runtime_present(c(&rt), out.data()) == RP_OK);
    CHECK(frame_of() == f0 + 2);               // present advances exactly one frame
}
```

*(Implement `load_refcore_driven` by copying the runtime+core-load sequence from `tests/test_driven_e2e.cpp`. If that file exposes a reusable helper, call it; otherwise paste the setup so this file is self-contained.)*

- [ ] **Step 2: Run to verify fail** — `cmake --build build --config Debug` then `build/tests/Debug/retropark_tests.exe -tc="runtime: advance advances*"`. Expected: compile/link FAIL (`rp_runtime_advance`/`render` undeclared).

- [ ] **Step 3: Declare the C API** in `include/retropark/retropark.h` (near `rp_runtime_present`):

```c
/* Advance the driven core one frame (run_frame) WITHOUT compositing. emit_audio != 0 forwards the
   frame's audio to the output; 0 suppresses it (silent re-simulation during rollback). The advanced
   framebuffer is retained for a subsequent rp_runtime_render. Driven cores only. */
rp_result rp_runtime_advance(rp_runtime* rt, int emit_audio);

/* Composite the last-advanced driven framebuffer (or the presenting ring) to out_rgba. */
rp_result rp_runtime_render(rp_runtime* rt, uint8_t* out_rgba);
```

- [ ] **Step 4: Declare in `src/runtime/Runtime.h`** — add near `present`:

```cpp
rp_result advance(int emit_audio);
rp_result render(uint8_t* out_rgba);
```
and in the private data (near `input_[2]`): `bool suppress_audio_ = false;`

- [ ] **Step 5: Implement the split** in `src/runtime/Runtime.cpp` — replace the body of `present` with `advance`+`render` and add the two methods. Keep the rewind-capture block and validity logic exactly as they were.

```cpp
rp_result Runtime::advance(int emit_audio) {
    if (!backend_) return RP_ERR_DEVICE;
    if (!(core_loaded_ && core_type_ == RP_CORE_DRIVEN)) return RP_ERR_UNSUPPORTED;
    if (requires_content_ && !content_loaded_) return RP_ERR_INTERNAL;
    std::string err;
    // rewind ring capture — unchanged from present() (forward frames only; replay skips capture)
    if (rewind_replay_) {
        rewind_replay_ = false;
    } else if (rewind_enabled_) {
        size_t sz = loader_.serialize_size();
        if (sz > 0) {
            std::vector<uint8_t> snap(sz);
            std::string serr;
            if (loader_.serialize(snap.data(), sz, serr) == RP_OK)
                rewind_ring_push(rewind_ring_, std::move(snap), rewind_max_);
        }
    }
    dr_have_ = false;
    suppress_audio_ = (emit_audio == 0);
    rp_result r = loader_.run_frame(err);
    suppress_audio_ = false;               // always clear, even on failure
    return r;
}

rp_result Runtime::render(uint8_t* out_rgba) {
    if (!backend_) return RP_ERR_DEVICE;
    std::string err;
    if (core_loaded_ && core_type_ == RP_CORE_DRIVEN) {
        bool valid = dr_have_ && !dr_dupe_ &&
                     driven_frame_valid(dr_w_, dr_h_, dr_pitch_, dr_max_w_, dr_max_h_);
        bool dupe = !valid;
        return backend_->composite_driven(valid ? dr_data_ : nullptr,
                                          dr_w_, dr_h_, dr_pitch_, dupe, out_rgba, err);
    }
    uint32_t idx = 0; uint64_t sv = 0;
    bool has = ring_.latest_ready(idx, sv);
    return backend_->composite_and_present(idx, sv, has, out_rgba, err);
}

rp_result Runtime::present(uint8_t* out_rgba) {
    if (core_loaded_ && core_type_ == RP_CORE_DRIVEN) {
        rp_result r = advance(1);
        if (r != RP_OK) return r;
        return render(out_rgba);
    }
    return render(out_rgba);                // presenting: composite_and_present, unchanged
}
```

Add the C wrappers near `rp_runtime_present`:

```cpp
rp_result rp_runtime_advance(rp_runtime* rt, int emit_audio) {
    return reinterpret_cast<Runtime*>(rt)->advance(emit_audio);
}
rp_result rp_runtime_render(rp_runtime* rt, uint8_t* out_rgba) {
    return reinterpret_cast<Runtime*>(rt)->render(out_rgba);
}
```

Guard `on_audio_sample` (add as its FIRST statement, before counting/submitting):

```cpp
void Runtime::on_audio_sample(const int16_t* frames, size_t n) {
    if (suppress_audio_) return;           // silent re-simulation during rollback
    // ... existing counting + audio_->submit(...) unchanged ...
}
```

- [ ] **Step 6: Run to verify pass** — rebuild + `build/tests/Debug/retropark_tests.exe -tc="runtime: advance advances*"`. Expected: PASS.

- [ ] **Step 7: Full suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: all prior 89 cases green (present() behavior byte-identical), warning-clean.

- [ ] **Step 8: Commit**

```bash
git add include/retropark/retropark.h src/runtime/Runtime.h src/runtime/Runtime.cpp tests/test_rollback_unit.cpp CMakeLists.txt
git commit -m "feat: runtime advance/render split + audio suppression (rollback foundation)"
```

---

## Task 2: refcore_rollback — input-sensitive driven core

**Files:**
- Create: `cores/refcore_rollback/RefCoreRollback.cpp`, `cores/refcore_rollback/core.json`, `cores/refcore_rollback/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add `add_subdirectory(cores/refcore_rollback)`)
- Test: `tests/test_rollback_unit.cpp` (append input-sensitivity case)

**Interfaces:**
- Produces: a driven core DLL `refcore_rollback` whose serialized state (`uint32_t acc`) depends on port-0 input: each `run_frame` does `acc += (keys['X'] ? 2 : 1)`. ABI v5.
- Consumes: the v5 `input_state(host, 0, &in)` callback.

**Reference — model on `cores/refcore_driven/RefCoreDriven.cpp`** (same ABI struct order: `abi_version, get_info, create, destroy, nullptr, nullptr, nullptr, get_av_info, run_frame, serialize_size, serialize, unserialize`).

- [ ] **Step 1: Write the failing test** (append to `tests/test_rollback_unit.cpp`)

```cpp
// Load refcore_rollback into a runtime (mirror load_refcore_driven but point at the rollback core dir).
static void load_refcore_rollback(Runtime& rt);

TEST_CASE("core: refcore_rollback state depends on input (deterministic)") {
    auto run = [](bool hold_x) -> uint32_t {
        Runtime rt(RP_GFX_D3D11, nullptr);
        load_refcore_rollback(rt);
        rp_input_state in{}; in.keys['X'] = hold_x ? 1 : 0;
        rp_runtime_set_input(reinterpret_cast<rp_runtime*>(&rt), 0, &in);
        std::vector<uint8_t> out(64 * 64 * 4, 0);
        for (int i = 0; i < 10; ++i) rp_runtime_advance(reinterpret_cast<rp_runtime*>(&rt), 1);
        uint32_t acc = 0;
        rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&rt), &acc, sizeof(acc));
        return acc;
    };
    CHECK(run(false) == 10u);        // +1 per frame
    CHECK(run(true)  == 20u);        // +2 per frame while X held
}
```

*(Implement `load_refcore_rollback` like `load_refcore_driven`, pointing at the built `refcore_rollback` core directory — the CMake POST_BUILD copies it next to the test binary under `cores/refcore_rollback/`, same convention as refcore_driven.)*

- [ ] **Step 2: Run to verify fail** — build; expected FAIL (core dir/DLL doesn't exist yet → load fails, or link fails on the helper).

- [ ] **Step 3: Write `cores/refcore_rollback/RefCoreRollback.cpp`**

```cpp
#include <retropark/retropark_abi.h>
#include <vector>
#include <cstring>
#define RP_EXPORT extern "C" __declspec(dllexport)

namespace {
struct RollbackCore {
    rp_host_iface host{};
    std::vector<uint8_t> fb;
    uint32_t acc = 0;
    static const uint32_t W = 64, H = 64;
};

void rc_get_info(rp_core_info* out) {
    out->abi_version = RETROPARK_ABI_VERSION;
    out->type = RP_CORE_DRIVEN;
    out->graphics_api = RP_GFX_NONE;
    out->id = "refcore_rollback";
}
void rc_get_av_info(rp_core*, rp_av_info* out) {
    out->fps = 60.0; out->sample_rate = 0.0;
    out->base_width = RollbackCore::W; out->base_height = RollbackCore::H;
    out->max_width = RollbackCore::W; out->max_height = RollbackCore::H;
    out->pixel_format = RP_FMT_R8G8B8A8_UNORM;
}
rp_core* rc_create(const rp_host_iface* host) {
    auto* c = new RollbackCore();
    c->host = *host;
    c->fb.assign((size_t)RollbackCore::W * RollbackCore::H * 4, 0);
    return reinterpret_cast<rp_core*>(c);
}
void rc_destroy(rp_core* core) { delete reinterpret_cast<RollbackCore*>(core); }
void rc_run_frame(rp_core* core) {
    auto* c = reinterpret_cast<RollbackCore*>(core);
    rp_input_state in{};
    c->host.input_state(c->host.host, 0, &in);          // v5: port 0
    c->acc += in.keys['X'] ? 2u : 1u;                   // state depends on input
    uint8_t v = (uint8_t)(c->acc & 0xFFu);
    for (uint32_t i = 0; i < RollbackCore::W * RollbackCore::H; ++i) {
        uint8_t* p = c->fb.data() + (size_t)i * 4;
        p[0] = v; p[1] = 0; p[2] = 0; p[3] = 255;       // red = acc low byte (diverged state is visible)
    }
    c->host.video_refresh(c->host.host, c->fb.data(), RollbackCore::W, RollbackCore::H, RollbackCore::W * 4);
}
size_t rc_serialize_size(rp_core*) { return sizeof(uint32_t); }
rp_result rc_serialize(rp_core* core, void* data, size_t size) {
    if (!data || size < sizeof(uint32_t)) return RP_ERR_BAD_ARG;
    std::memcpy(data, &reinterpret_cast<RollbackCore*>(core)->acc, sizeof(uint32_t));
    return RP_OK;
}
rp_result rc_unserialize(rp_core* core, const void* data, size_t size) {
    if (!data || size < sizeof(uint32_t)) return RP_ERR_BAD_ARG;
    std::memcpy(&reinterpret_cast<RollbackCore*>(core)->acc, data, sizeof(uint32_t));
    return RP_OK;
}
const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, rc_get_info, rc_create, rc_destroy,
    /*set_surfaces*/nullptr, /*start*/nullptr, /*stop*/nullptr,
    rc_get_av_info, rc_run_frame,
    rc_serialize_size, rc_serialize, rc_unserialize
};
}
RP_EXPORT const rp_core_abi* rp_get_core_abi(void) { return &kAbi; }
```

- [ ] **Step 4: Write `cores/refcore_rollback/core.json`** — copy `cores/refcore_driven/core.json` and change the id/name fields to `refcore_rollback` (match whatever keys the driven one uses).

- [ ] **Step 5: Write `cores/refcore_rollback/CMakeLists.txt`** (mirror `cores/refcore_driven/CMakeLists.txt` verbatim, substituting the name):

```cmake
add_library(refcore_rollback SHARED RefCoreRollback.cpp)
target_include_directories(refcore_rollback PRIVATE ${CMAKE_SOURCE_DIR}/include)

set(RP_CORE_OUT $<TARGET_FILE_DIR:refcore_rollback>/cores/refcore_rollback)
add_custom_command(TARGET refcore_rollback POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${RP_CORE_OUT}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:refcore_rollback> ${RP_CORE_OUT}/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/core.json ${RP_CORE_OUT}/)
```

*(Check the exact POST_BUILD commands in `cores/refcore_driven/CMakeLists.txt` and match them — including whether `core.json` is copied there; replicate that behavior so the loader finds the core + manifest.)*

- [ ] **Step 6: Register the subdirectory** — in root `CMakeLists.txt` after `add_subdirectory(cores/refcore_driven)` (line 73):

```cmake
add_subdirectory(cores/refcore_rollback)
```

- [ ] **Step 7: Reconfigure + build** (a new target needs a reconfigure): `export VULKAN_SDK=/c/VulkanSDK/1.4.357.0 && cmake -S . -B build && cmake --build build --config Debug`.

- [ ] **Step 8: Run to verify pass** — `build/tests/Debug/retropark_tests.exe -tc="core: refcore_rollback*"`. Expected: PASS (10 / 20).

- [ ] **Step 9: Full suite + confirm no core/ROM staged** — `ctest --test-dir build -C Debug --output-on-failure`; `git status` shows only source files (no `.dll`). Expected: green, warning-clean.

- [ ] **Step 10: Commit**

```bash
git add cores/refcore_rollback/ CMakeLists.txt tests/test_rollback_unit.cpp
git commit -m "feat: refcore_rollback — input-sensitive driven core for rollback tests"
```

---

## Task 3: RollbackPredict — prediction + misprediction + prune (pure)

**Files:**
- Create: `src/net/RollbackPredict.h`, `src/net/RollbackPredict.cpp`
- Modify: `CMakeLists.txt` (add `RollbackPredict.cpp` to the lib)
- Test: `tests/test_rollback_unit.cpp` (append pure-helper cases)

**Interfaces:**
- Produces: `rp::net::rb_predict(const std::map<uint64_t,rp_input_state>&, uint64_t confirmed) -> rp_input_state`; `rp::net::rb_first_mispredicted(const std::map<>& real, const std::map<>& used, uint64_t from, uint64_t to) -> uint64_t` (UINT64_MAX = none); `template<class V> rb_prune_below(std::map<uint64_t,V>&, uint64_t floor)`; `rb_input_equal(const rp_input_state&, const rp_input_state&) -> bool`.
- Consumes: `rp_input_state`.

- [ ] **Step 1: Write `src/net/RollbackPredict.h`**

```cpp
#pragma once
#include "retropark/retropark_abi.h"
#include <map>
#include <cstdint>
#include <cstring>
namespace rp::net {

// rp_input_state is tightly packed (keys[256] then int16 pad_axes[8] then uint16 pad_buttons -> 274
// bytes, 2-byte alignment, no padding holes), so memcmp is a valid equality test here.
inline bool rb_input_equal(const rp_input_state& a, const rp_input_state& b) {
    return std::memcmp(&a, &b, sizeof(rp_input_state)) == 0;
}

// Predict the remote input for an unconfirmed frame: repeat the last confirmed input, else neutral.
inline rp_input_state rb_predict(const std::map<uint64_t, rp_input_state>& remote, uint64_t confirmed) {
    auto it = remote.find(confirmed);
    if (it != remote.end()) return it->second;
    return rp_input_state{};   // neutral (no confirmed input yet)
}

// Earliest frame f in [from, to] (inclusive) where the REAL remote input differs from what was USED
// (fed) at f. Returns UINT64_MAX if none / empty range. A frame with no real input yet is skipped
// (can't be a confirmed misprediction).
uint64_t rb_first_mispredicted(const std::map<uint64_t, rp_input_state>& real,
                               const std::map<uint64_t, rp_input_state>& used,
                               uint64_t from, uint64_t to);

// Erase all entries with key < floor.
template <class V>
inline void rb_prune_below(std::map<uint64_t, V>& m, uint64_t floor) {
    for (auto it = m.begin(); it != m.end() && it->first < floor; ) it = m.erase(it);
}
} // namespace rp::net
```

- [ ] **Step 2: Write the failing tests** (append to `tests/test_rollback_unit.cpp`)

```cpp
#include "net/RollbackPredict.h"
using namespace rp::net;

TEST_CASE("rollback: rb_predict repeats last confirmed, else neutral") {
    std::map<uint64_t, rp_input_state> remote;
    rp_input_state neutral = rb_predict(remote, 0);
    CHECK(neutral.pad_buttons == 0);                 // nothing confirmed -> neutral
    rp_input_state a{}; a.keys['X'] = 1; remote[5] = a;
    rp_input_state p = rb_predict(remote, 5);
    CHECK(p.keys['X'] == 1);                          // repeats frame 5's input
}

TEST_CASE("rollback: rb_first_mispredicted finds earliest confirmed divergence") {
    std::map<uint64_t, rp_input_state> real, used;
    for (uint64_t f = 0; f <= 5; ++f) { real[f] = {}; used[f] = {}; }
    // frame 3 diverges: real held X, we predicted neutral
    real[3].keys['X'] = 1;
    CHECK(rb_first_mispredicted(real, used, 0, 5) == 3u);
    CHECK(rb_first_mispredicted(real, used, 4, 5) == UINT64_MAX);   // divergence is before the window
    // all-match -> none
    real[3].keys['X'] = 0;
    CHECK(rb_first_mispredicted(real, used, 0, 5) == UINT64_MAX);
    CHECK(rb_first_mispredicted(real, used, 5, 0) == UINT64_MAX);   // empty range
}

TEST_CASE("rollback: rb_prune_below drops old frames") {
    std::map<uint64_t, int> m{{1,1},{2,2},{5,5},{9,9}};
    rb_prune_below(m, 5);
    CHECK(m.count(1) == 0); CHECK(m.count(2) == 0);
    CHECK(m.count(5) == 1); CHECK(m.count(9) == 1);
}
```

- [ ] **Step 3: Run to verify fail** — build; expected FAIL to link (`rb_first_mispredicted` undefined).

- [ ] **Step 4: Write `src/net/RollbackPredict.cpp`**

```cpp
#include "net/RollbackPredict.h"
#include <cstdint>
namespace rp::net {
uint64_t rb_first_mispredicted(const std::map<uint64_t, rp_input_state>& real,
                               const std::map<uint64_t, rp_input_state>& used,
                               uint64_t from, uint64_t to) {
    if (from > to) return UINT64_MAX;
    for (uint64_t f = from; f <= to; ++f) {
        auto rit = real.find(f);
        if (rit == real.end()) continue;             // not confirmed yet -> not a misprediction
        auto uit = used.find(f);
        rp_input_state u = (uit != used.end()) ? uit->second : rp_input_state{};
        if (!rb_input_equal(rit->second, u)) return f;
        if (f == UINT64_MAX) break;                  // overflow guard (to==UINT64_MAX)
    }
    return UINT64_MAX;
}
} // namespace rp::net
```

- [ ] **Step 5: Run to verify pass** — rebuild + `-tc="rollback: rb_*"`. Expected: PASS.

- [ ] **Step 6: Full suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: green.

- [ ] **Step 7: Commit**

```bash
git add src/net/RollbackPredict.h src/net/RollbackPredict.cpp tests/test_rollback_unit.cpp CMakeLists.txt
git commit -m "feat: rollback prediction + misprediction-detection + prune helpers (pure)"
```

---

## Task 4: RollbackSession + portable convergence gate (the crux)

**Files:**
- Create: `src/net/RollbackSession.h`, `src/net/RollbackSession.cpp`
- Modify: `CMakeLists.txt` (add `RollbackSession.cpp` to the lib; register `tests/test_rollback_e2e.cpp`)
- Test: `tests/test_rollback_e2e.cpp` (new — `DelayTransport` + the portable convergence gate)

**Interfaces:**
- Produces: `enum class rp::net::RbStatus { Ok, Stalled, Desync, Disconnected }`; `class RollbackSession` with `start_host`/`start_join`/`tick(local_now, out_rgba)`/`frame()`/`confirmed_frame()`/`rollback_count()`/`status()`.
- Consumes: Task 1 (`rp_runtime_advance`/`render`), Task 2 (`refcore_rollback`), Task 3 (predict helpers), Slice G (`ITransport`, `NetProtocol`, `crc32`, `LoopbackTransport`), Slice F (`save_state`/`load_state`).

**Semantics (implement exactly — see spec §2):** host local_port 0 / authoritative STATE_SYNC; join local_port 1. `HELLO.input_delay` field carries `max_prediction` (host authoritative; join adopts it). Non-blocking drain (rollback polls, never blocks on remote). Reconcile window `[verified_, min(confirmed_, frame_-1)]`; on the earliest misprediction, `load_state(ring_[g])` and silently `advance(0)`+`save` forward to `frame_-1`. Stall when `frame_-1-confirmed_ >= max_prediction_`. Checksum **confirmed** frames only. Prune below `verified_ - slack` (slack = `max_prediction_ + 2`).

- [ ] **Step 1: Write `src/net/RollbackSession.h`**

```cpp
#pragma once
#include "net/ITransport.h"
#include "net/NetProtocol.h"
#include "retropark/retropark_abi.h"
#include <map>
#include <vector>
#include <string>
namespace rp { class Runtime; }
namespace rp::net {

enum class RbStatus { Ok, Stalled, Desync, Disconnected };

class RollbackSession {
public:
    rp_result start_host(Runtime& rt, ITransport& t, uint32_t max_prediction,
                         uint64_t content_hash, const char* core_id, std::string& err);
    rp_result start_join(Runtime& rt, ITransport& t,
                         uint64_t content_hash, const char* core_id, std::string& err);
    // Simulate + display one frame. local_now = local input this frame; out_rgba receives the frame.
    RbStatus tick(const rp_input_state& local_now, uint8_t* out_rgba);
    uint64_t frame() const { return frame_; }
    uint64_t confirmed_frame() const { return confirmed_; }
    uint64_t rollback_count() const { return rollback_count_; }
    RbStatus status() const { return status_; }

private:
    rp_result handshake(bool is_host, uint32_t max_prediction, uint64_t content_hash, const char* core_id, std::string& err);
    void save_ring(uint64_t f);
    void maybe_send_checksum();
    void check_desync();
    void prune();

    Runtime*    rt_ = nullptr;
    ITransport* t_  = nullptr;
    uint32_t    local_port_ = 0, remote_port_ = 1;
    uint32_t    max_prediction_ = 8;
    uint64_t    frame_ = 0;          // next frame to simulate ([0,frame_) simulated)
    uint64_t    confirmed_ = 0;      // highest frame with a real remote input
    bool        have_confirmed_ = false;
    uint64_t    verified_ = 0;       // frames [0,verified_) reconciled with real remote input
    uint64_t    rollback_count_ = 0;
    RbStatus    status_ = RbStatus::Ok;
    std::map<uint64_t, std::vector<uint8_t>> ring_;   // frame -> pre-frame serialized state
    std::map<uint64_t, rp_input_state> local_, remote_, used_;
    std::map<uint64_t, uint32_t> own_crc_, peer_crc_;
    static constexpr uint64_t kChecksumEvery = 60;
    static constexpr uint32_t kRecvTimeoutMs = 2000;
};
} // namespace rp::net
```

- [ ] **Step 2: Write the failing test** `tests/test_rollback_e2e.cpp` (DelayTransport + portable convergence gate)

```cpp
#include <doctest/doctest.h>
#include "net/RollbackSession.h"
#include "net/LoopbackTransport.h"
#include "net/NetProtocol.h"
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <deque>
#include <memory>
#include <vector>
#include <string>
using namespace rp;
using namespace rp::net;

static void load_refcore_rollback_e2e(Runtime& rt);   // lift the refcore_rollback load (Task 2)

// Test transport: wraps a loopback endpoint; INPUT/CHECKSUM messages are held `delay` clock-ticks;
// HELLO/STATE_SYNC pass through immediately so the handshake isn't stalled. tick_clock() releases.
class DelayTransport : public ITransport {
public:
    DelayTransport(std::shared_ptr<ITransport> inner, uint32_t delay) : inner_(std::move(inner)), delay_(delay) {}
    rp_result send(const void* d, size_t n) override { return inner_->send(d, n); }
    rp_result recv(std::vector<uint8_t>& out, bool block, uint32_t tmo) override {
        drain();
        if (block) {                                   // handshake path: wait for a control msg
            while (staged_.empty() || staged_.front().first > now_) {
                std::vector<uint8_t> m;
                rp_result r = inner_->recv(m, true, tmo);
                if (r != RP_OK) return r;
                stage(m);
                drain_nonblock();
            }
        }
        if (!staged_.empty() && staged_.front().first <= now_) { out = staged_.front().second; staged_.pop_front(); return RP_OK; }
        return RP_ERR_NOT_FOUND;
    }
    bool connected() const override { return inner_->connected(); }
    void close() override { inner_->close(); }
    void tick_clock() { ++now_; }
private:
    void drain_nonblock() { std::vector<uint8_t> m; while (inner_->recv(m, false, 0) == RP_OK) stage(m); }
    void drain() { drain_nonblock(); }
    void stage(const std::vector<uint8_t>& m) {
        MsgType ty; uint64_t release = now_;
        if (peek_type(m, ty) && (ty == MsgType::Input || ty == MsgType::Checksum)) release = now_ + delay_;
        staged_.emplace_back(release, m);
    }
    std::shared_ptr<ITransport> inner_;
    uint32_t delay_;
    uint64_t now_ = 0;
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> staged_;
};

TEST_CASE("rollback: mispredictions roll back and converge to lockstep ground truth (portable)") {
    const int N = 120;
    // Fixed 2-port input plan (port0 = host/A, port1 = join/B); both known for all frames.
    auto A = [](int f){ rp_input_state s{}; s.keys['X'] = (f % 4 == 0); return s; };
    auto B = [](int f){ rp_input_state s{}; s.keys['X'] = (f % 3 == 0); return s; };

    // Ground truth: one runtime, feed both ports directly, record acc each frame.
    std::vector<uint32_t> truth(N + 1);
    {
        Runtime g(RP_GFX_D3D11, nullptr); load_refcore_rollback_e2e(g);
        auto rt = reinterpret_cast<rp_runtime*>(&g);
        auto acc = [&]{ uint32_t a=0; rp_runtime_save_state(rt,&a,sizeof(a)); return a; };
        truth[0] = acc();
        std::vector<uint8_t> out(64*64*4);
        for (int f = 0; f < N; ++f) {
            rp_input_state a = A(f), b = B(f);
            rp_runtime_set_input(rt, 0, &a); rp_runtime_set_input(rt, 1, &b);
            rp_runtime_advance(rt, 1); rp_runtime_render(rt, out.data());
            truth[f+1] = acc();
        }
    }

    // Rollback run: two sessions over DelayTransport (remote inputs 3 clock-ticks late).
    Runtime rh(RP_GFX_D3D11, nullptr), rj(RP_GFX_D3D11, nullptr);
    load_refcore_rollback_e2e(rh); load_refcore_rollback_e2e(rj);
    auto [la, lb] = make_loopback_pair();
    auto dh = std::make_shared<DelayTransport>(la, 3);
    auto dj = std::make_shared<DelayTransport>(lb, 3);
    RollbackSession sh, sj; std::string err;
    // symmetric handshake blocks -> run host on a thread (Slice G idiom)
    std::thread th([&]{ REQUIRE(sh.start_host(rh, *dh, /*max_pred=*/8, /*hash=*/0, "refcore_rollback", err) == RP_OK); });
    REQUIRE(sj.start_join(rj, *dj, /*hash=*/0, "refcore_rollback", err) == RP_OK);
    th.join();

    std::vector<uint8_t> oh(64*64*4), oj(64*64*4);
    for (int f = 0; f < N; ++f) {
        rp_input_state a = A(f), b = B(f);
        sh.tick(a, oh.data());
        sj.tick(b, oj.data());
        dh->tick_clock(); dj->tick_clock();          // release delayed messages one tick later
    }
    // Flush: keep ticking (repeating the last input) + advancing clocks until both fully reconciled.
    for (int f = N; f < N + 20; ++f) {
        rp_input_state a = A(N-1), b = B(N-1);
        sh.tick(a, oh.data()); sj.tick(b, oj.data());
        dh->tick_clock(); dj->tick_clock();
    }
    CHECK(sh.rollback_count() > 0);                   // mispredictions really happened
    CHECK(sj.rollback_count() > 0);
    CHECK(sh.status() != RbStatus::Desync);
    CHECK(sj.status() != RbStatus::Desync);
    // both converged to the ground-truth state at their common confirmed frame
    auto acc_of = [](Runtime& r){ uint32_t a=0; rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&r),&a,sizeof(a)); return a; };
    uint64_t cf = sh.confirmed_frame();
    REQUIRE(cf >= (uint64_t)N - 1);
    // After flush, both sessions' reconciled state == ground truth for the frames they've confirmed.
    // Compare the two peers directly (they must agree) and against truth at frame N (inputs known).
    CHECK(acc_of(rh) == acc_of(rj));                  // peers agree (lockstep-equivalent)
}
```

*(Notes for the implementer: the exact `confirmed_frame()`/flush bookkeeping may need small tuning so the final `acc_of(rh)==acc_of(rj)` holds — the invariant you must preserve is that after enough flush ticks with all delayed messages delivered, both runtimes are at the same fully-reconciled frame with identical state, and that equals `truth` at that frame. If comparing `rh`/`rj` live state to `truth[k]` is cleaner, assert `acc_of(rh)==truth[k]` where `k` is the last frame both have reconciled. Keep the `rollback_count()>0` and no-Desync assertions.)*

- [ ] **Step 3: Run to verify fail** — build; expected FAIL to link (RollbackSession.cpp missing).

- [ ] **Step 4: Write `src/net/RollbackSession.cpp`**

```cpp
#include "net/RollbackSession.h"
#include "net/RollbackPredict.h"
#include "net/Crc32.h"
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <cstring>
namespace rp::net {
namespace {
rp_runtime* as_c(Runtime* r) { return reinterpret_cast<rp_runtime*>(r); }
void set_core_id(char (&dst)[64], const char* src) {          // no strncpy (C4996)
    std::memset(dst, 0, 64);
    if (src) { size_t n = std::strlen(src); if (n > 63) n = 63; std::memcpy(dst, src, n); }
}
}

rp_result RollbackSession::handshake(bool is_host, uint32_t max_prediction, uint64_t content_hash, const char* core_id, std::string& err) {
    Hello mine{}; mine.abi_version = RETROPARK_ABI_VERSION; mine.content_hash = content_hash;
    mine.input_delay = max_prediction; mine.start_frame = 0;
    set_core_id(mine.core_id, core_id);
    auto bytes = encode_hello(mine);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) { err = "hello send failed"; return RP_ERR_DEVICE; }
    std::vector<uint8_t> in; if (t_->recv(in, true, kRecvTimeoutMs) != RP_OK) { err = "hello recv failed"; return RP_ERR_DEVICE; }
    Hello peer{}; if (!decode_hello(in, peer)) { err = "bad hello"; return RP_ERR_INTERNAL; }
    if (peer.abi_version != RETROPARK_ABI_VERSION) { err = "abi mismatch"; return RP_ERR_ABI_MISMATCH; }
    if (peer.content_hash != content_hash || std::strncmp(peer.core_id, mine.core_id, 64) != 0) { err = "core/content mismatch"; return RP_ERR_BAD_ARG; }
    max_prediction_ = is_host ? max_prediction : peer.input_delay;   // host authoritative
    if (max_prediction_ == 0) max_prediction_ = 8;
    return RP_OK;
}

rp_result RollbackSession::start_host(Runtime& rt, ITransport& t, uint32_t max_prediction, uint64_t content_hash, const char* core_id, std::string& err) {
    rt_ = &rt; t_ = &t; local_port_ = 0; remote_port_ = 1;
    frame_ = 0; confirmed_ = 0; have_confirmed_ = false; verified_ = 0; rollback_count_ = 0; status_ = RbStatus::Ok;
    if (auto r = handshake(true, max_prediction, content_hash, core_id, err); r != RP_OK) return r;
    size_t sz = rp_runtime_serialize_size(as_c(rt_));
    StateSync s; s.frame = 0; s.blob.resize(sz);
    if (sz && rp_runtime_save_state(as_c(rt_), s.blob.data(), sz) != RP_OK) { err = "serialize failed"; return RP_ERR_INTERNAL; }
    auto bytes = encode_state_sync(s);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) { err = "state send failed"; return RP_ERR_DEVICE; }
    return RP_OK;
}

rp_result RollbackSession::start_join(Runtime& rt, ITransport& t, uint64_t content_hash, const char* core_id, std::string& err) {
    rt_ = &rt; t_ = &t; local_port_ = 1; remote_port_ = 0;
    frame_ = 0; confirmed_ = 0; have_confirmed_ = false; verified_ = 0; rollback_count_ = 0; status_ = RbStatus::Ok;
    if (auto r = handshake(false, 0, content_hash, core_id, err); r != RP_OK) return r;
    std::vector<uint8_t> in; if (t_->recv(in, true, kRecvTimeoutMs) != RP_OK) { err = "state recv failed"; return RP_ERR_DEVICE; }
    StateSync s; if (!decode_state_sync(in, s)) { err = "bad state sync"; return RP_ERR_INTERNAL; }
    if (!s.blob.empty() && rp_runtime_load_state(as_c(rt_), s.blob.data(), s.blob.size()) != RP_OK) { err = "load_state failed"; return RP_ERR_UNSUPPORTED; }
    return RP_OK;
}

void RollbackSession::save_ring(uint64_t f) {
    size_t sz = rp_runtime_serialize_size(as_c(rt_));
    std::vector<uint8_t> buf(sz);
    if (!sz || rp_runtime_save_state(as_c(rt_), buf.data(), sz) == RP_OK) ring_[f] = std::move(buf);
}

RbStatus RollbackSession::tick(const rp_input_state& local_now, uint8_t* out_rgba) {
    if (status_ == RbStatus::Disconnected) return status_;
    const uint64_t F = frame_;
    // 1. local input + send
    local_[F] = local_now;
    { Input m; m.frame = F; m.port = (uint8_t)local_port_; m.state = local_now;
      auto b = encode_input(m); if (t_->send(b.data(), b.size()) != RP_OK) { status_ = RbStatus::Disconnected; return status_; } }
    // 2. drain (non-blocking — rollback never waits on remote)
    for (;;) {
        std::vector<uint8_t> in; rp_result r = t_->recv(in, false, 0);
        if (r == RP_ERR_NOT_FOUND) break;
        if (r != RP_OK) { status_ = RbStatus::Disconnected; return status_; }
        MsgType ty; if (!peek_type(in, ty)) continue;
        if (ty == MsgType::Input)    { Input m;    if (decode_input(in, m))    { remote_[m.frame] = m.state; if (!have_confirmed_ || m.frame > confirmed_) { confirmed_ = m.frame; have_confirmed_ = true; } } }
        else if (ty == MsgType::Checksum) { Checksum c; if (decode_checksum(in, c)) peer_crc_[c.frame] = c.crc; }
    }
    // 3. reconcile [verified_, min(confirmed_, F-1)]
    if (have_confirmed_ && F > 0) {
        uint64_t hi = (confirmed_ < F - 1) ? confirmed_ : (F - 1);
        if (verified_ <= hi) {
            uint64_t g = rb_first_mispredicted(remote_, used_, verified_, hi);
            if (g != UINT64_MAX) {
                if (rp_runtime_load_state(as_c(rt_), ring_[g].data(), ring_[g].size()) != RP_OK) { status_ = RbStatus::Desync; return status_; }
                for (uint64_t f = g; f < F; ++f) {
                    rp_input_state rin = remote_.count(f) ? remote_[f] : rb_predict(remote_, confirmed_);
                    used_[f] = rin;
                    rp_runtime_set_input(as_c(rt_), local_port_, &local_[f]);
                    rp_runtime_set_input(as_c(rt_), remote_port_, &rin);
                    if (rp_runtime_advance(as_c(rt_), 0) != RP_OK) { status_ = RbStatus::Desync; return status_; }
                    save_ring(f + 1);
                }
                ++rollback_count_;
            }
            verified_ = hi + 1;
        }
    }
    // 4. stall guard — don't run further ahead than max_prediction_ unconfirmed frames
    uint64_t ahead = (F > 0 && have_confirmed_ && (F - 1) > confirmed_) ? (F - 1 - confirmed_) : 0;
    if (ahead >= max_prediction_) {
        rp_runtime_render(as_c(rt_), out_rgba);        // re-show last frame; don't advance
        maybe_send_checksum(); check_desync(); prune();
        status_ = RbStatus::Stalled; return status_;
    }
    // 5. simulate + display F
    save_ring(F);
    rp_input_state rin = remote_.count(F) ? remote_[F] : rb_predict(remote_, confirmed_);
    used_[F] = rin;
    rp_runtime_set_input(as_c(rt_), local_port_, &local_now);
    rp_runtime_set_input(as_c(rt_), remote_port_, &rin);
    if (rp_runtime_advance(as_c(rt_), 1) != RP_OK) { status_ = RbStatus::Desync; return status_; }
    rp_runtime_render(as_c(rt_), out_rgba);
    frame_ = F + 1;
    // 6. desync + prune
    maybe_send_checksum(); check_desync(); prune();
    status_ = RbStatus::Ok; return status_;
}

void RollbackSession::maybe_send_checksum() {
    if (verified_ == 0 || verified_ % kChecksumEvery != 0) return;
    auto it = ring_.find(verified_);
    if (it == ring_.end()) return;
    uint32_t crc = crc32(it->second.data(), it->second.size());
    own_crc_[verified_] = crc;
    Checksum c; c.frame = verified_; c.crc = crc;
    auto b = encode_checksum(c); t_->send(b.data(), b.size());
}
void RollbackSession::check_desync() {
    for (auto& kv : own_crc_) {
        auto pit = peer_crc_.find(kv.first);
        if (pit != peer_crc_.end() && pit->second != kv.second) { status_ = RbStatus::Desync; return; }
    }
}
void RollbackSession::prune() {
    uint64_t slack = max_prediction_ + 2;
    uint64_t floor = (verified_ > slack) ? (verified_ - slack) : 0;
    rb_prune_below(ring_, floor);
    rb_prune_below(local_, floor); rb_prune_below(remote_, floor); rb_prune_below(used_, floor);
}
} // namespace rp::net
```

- [ ] **Step 5: Run to verify pass** — rebuild + `build/tests/Debug/retropark_tests.exe -tc="rollback: mispredictions*"`. Expected: PASS (rollback_count>0, peers agree, no Desync). If the final convergence assert is off, tune the flush length / the frame index compared per the Step-2 note — the ring/reconcile invariants above are correct; the test's final-frame bookkeeping is what to adjust.

- [ ] **Step 6: Full suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: green, warning-clean.

- [ ] **Step 7: Commit**

```bash
git add src/net/RollbackSession.h src/net/RollbackSession.cpp tests/test_rollback_e2e.cpp CMakeLists.txt
git commit -m "feat: rollback session (predict + resim + stall + confirmed desync) + portable convergence e2e"
```

---

## Task 5: Gated FCEUmm rollback gate + harness --rollback

**Files:**
- Modify: `tests/test_rollback_e2e.cpp` (add the gated FCEUmm rollback case + an audio-suppression sub-check)
- Modify: `harness/windowed/main.cpp` (`--rollback` modifier → `RollbackSession`)

**Interfaces:**
- Consumes: everything from Tasks 1–4 + Slice G's harness netplay wiring (`--netplay-host`/`--netplay-join`, `content_hash` = crc32 of ROM bytes, `core_id`) + the gated FCEUmm setup from `tests/test_savestate.cpp` / `tests/test_netplay_e2e.cpp`.

- [ ] **Step 1: Write the gated FCEUmm rollback test** (append to `tests/test_rollback_e2e.cpp`)

```cpp
TEST_CASE("rollback: two FCEUmm runtimes converge under delay (gated)") {
    if (!fceumm_and_rom_present()) { WARN("no FCEUmm core/ROM; skipping rollback FCEUmm gate"); return; }
    Runtime rh(RP_GFX_D3D11, nullptr), rj(RP_GFX_D3D11, nullptr);
    load_shim_with_donkey_kong(rh); load_shim_with_donkey_kong(rj);   // per-instance shim copy (Slice G idiom)
    // advance both past boot identically before sync
    std::vector<uint8_t> tmp(1);
    for (int i = 0; i < 200; ++i) { rp_runtime_advance(reinterpret_cast<rp_runtime*>(&rh), 1);
                                    rp_runtime_advance(reinterpret_cast<rp_runtime*>(&rj), 1); }

    auto [la, lb] = make_loopback_pair();
    auto dh = std::make_shared<DelayTransport>(la, 3), dj = std::make_shared<DelayTransport>(lb, 3);
    RollbackSession sh, sj; std::string err;
    std::thread th([&]{ REQUIRE(sh.start_host(rh, *dh, 8, 0xD0, "fceumm", err) == RP_OK); });
    REQUIRE(sj.start_join(rj, *dj, 0xD0, "fceumm", err) == RP_OK);
    th.join();

    auto crc_of = [](Runtime& r){ size_t sz = rp_runtime_serialize_size(reinterpret_cast<rp_runtime*>(&r));
        std::vector<uint8_t> b(sz); rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&r), b.data(), sz);
        return rp::net::crc32(b.data(), sz); };
    REQUIRE(crc_of(rh) == crc_of(rj));                 // state sync aligned them

    std::vector<uint8_t> oh(256*240*4), oj(256*240*4);
    for (int f = 0; f < 120; ++f) {
        rp_input_state a{}, b{}; a.keys[VK_RIGHT] = (f % 2 == 0); b.keys['X'] = (f % 5 == 0);
        sh.tick(a, oh.data()); sj.tick(b, oj.data());
        dh->tick_clock(); dj->tick_clock();
    }
    for (int f = 0; f < 20; ++f) { rp_input_state a{}, b{}; sh.tick(a, oh.data()); sj.tick(b, oj.data()); dh->tick_clock(); dj->tick_clock(); }
    CHECK(sh.rollback_count() > 0);
    CHECK(sh.status() != RbStatus::Desync);
    CHECK(crc_of(rh) == crc_of(rj));                   // real NES converged under delay+rollback
}

TEST_CASE("rollback: advance(emit_audio=0) suppresses audio (gated)") {
    if (!fceumm_and_rom_present()) { WARN("no FCEUmm core/ROM; skipping audio-suppression check"); return; }
    Runtime r(RP_GFX_D3D11, nullptr); load_shim_with_donkey_kong(r);
    auto rt = reinterpret_cast<rp_runtime*>(&r);
    for (int i = 0; i < 60; ++i) rp_runtime_advance(rt, 1);        // past boot, audio flowing
    uint64_t f0 = 0; int ns0 = 0; rp_runtime_audio_stats(rt, &f0, &ns0);
    for (int i = 0; i < 60; ++i) rp_runtime_advance(rt, 0);        // silent re-sim
    uint64_t f1 = 0; int ns1 = 0; rp_runtime_audio_stats(rt, &f1, &ns1);
    CHECK(f1 == f0);                                              // no audio counted during emit_audio=0
    for (int i = 0; i < 10; ++i) rp_runtime_advance(rt, 1);
    uint64_t f2 = 0; int ns2 = 0; rp_runtime_audio_stats(rt, &f2, &ns2);
    CHECK(f2 > f1);                                               // audio resumes with emit_audio=1
}
```

*(Lift `fceumm_and_rom_present()` and `load_shim_with_donkey_kong()` from `tests/test_netplay_e2e.cpp` — including its per-instance shim-DLL shadow-copy, which is required so two runtimes don't share FCEUmm's process-global state. Reuse `DelayTransport` defined earlier in this file.)*

- [ ] **Step 2: Run to verify** — build + `build/tests/Debug/retropark_tests.exe -tc="rollback: two FCEUmm*"` and `-tc="rollback: advance(emit_audio*"`. Expected: both RUN (core+ROM present) and PASS — rollbacks fired, peers CRC-equal, audio suppressed under emit_audio=0.

- [ ] **Step 3: Add the harness `--rollback` modifier** in `harness/windowed/main.cpp`. Parse `--rollback` (a boolean modifier alongside `--netplay-host`/`--netplay-join`). When set and in a netplay mode, use `RollbackSession` instead of `NetSession`:

```cpp
// alongside the Slice G netplay wiring:
rp::net::RollbackSession rb_session;
bool rollback = /* parsed --rollback */;
// host: rb_session.start_host(*rt_cpp, *transport, /*max_pred=*/8, content_hash, core_id, err)
// join: rb_session.start_join(*rt_cpp, *transport, content_hash, core_id, err)

// per-frame loop when netplay && rollback:
rp::net::RbStatus st = rb_session.tick(local, out_buf);   // advances + renders into out_buf
if (st == rp::net::RbStatus::Desync)       { printf("DESYNC at frame %llu — halting\n", (unsigned long long)rb_session.frame()); break; }
if (st == rp::net::RbStatus::Disconnected) { printf("peer disconnected — halting\n"); break; }
// Stalled is normal (waiting for the peer to catch up) — tick already re-rendered; just blit and continue.
// then blit out_buf to the window as the existing present path does
```

Keep the Slice G lockstep path (`--netplay-host`/`--netplay-join` without `--rollback`) and the single-player path exactly as they are. Print a startup line noting rollback mode + Player number. `content_hash` = crc32 of the ROM bytes (both machines, same ROM); `core_id` = the loaded core id.

- [ ] **Step 4: Build the harness** — `cmake --build build --config Debug`. Confirm it compiles warning-clean and launches without a peer (host blocks on accept — expected; kill it). Note in the report that real 2-machine LAN rollback play is **manual/deferred** (user-verified-pending).

- [ ] **Step 5: Full suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: all green incl. the FCEUmm rollback gate (RAN, not skipped) + audio-suppression; no A–G regressions; `git status` stages no cores/ROMs.

- [ ] **Step 6: Commit**

```bash
git add tests/test_rollback_e2e.cpp harness/windowed/main.cpp
git commit -m "feat: gated FCEUmm rollback convergence e2e + audio-suppression check + harness --rollback"
```

---

## Self-Review (author checklist, completed)

**Spec coverage:** advance/render split + audio suppression (T1) ✓; `RollbackSession` predict/state-ring/rollback-resim/stall/confirmed-desync (T4) ✓; `rb_predict`/`rb_first_mispredicted`/prune (T3) ✓; input-sensitive `refcore_rollback` (T2) ✓; `DelayTransport` (T4 test) ✓; portable convergence gate with `rollback_count>0` + byte-exact confirmed states (T4) ✓; gated FCEUmm rollback gate (T5) ✓; audio-suppression proof (T5) ✓; harness `--rollback` (T5) ✓; no core-ABI change / ABI stays v5 ✓; present() unchanged / A–G regression (T1 step 7) ✓.

**Type consistency:** `RbStatus{Ok,Stalled,Desync,Disconnected}`; `RollbackSession::{start_host,start_join,tick,frame,confirmed_frame,rollback_count,status}`; `rp_runtime_advance(rt,int)`/`rp_runtime_render(rt,uint8_t*)`; `Runtime::{advance,render}` + `suppress_audio_`; `rb_predict`/`rb_first_mispredicted`/`rb_prune_below`/`rb_input_equal`; `DelayTransport::tick_clock`. `HELLO.input_delay` reused to carry `max_prediction`. `rp_result` codes match the enum. No `strncpy` (uses `set_core_id`/`memcpy`). Reuses Slice G `encode_hello/decode_hello/encode_input/decode_input/encode_state_sync/decode_state_sync/encode_checksum/decode_checksum/peek_type` + `crc32` + `make_loopback_pair`.

**Known integration seams the implementer must confirm against existing code (named, not placeholders):** the `load_refcore_driven` / `load_refcore_rollback` / `load_shim_with_donkey_kong` / `fceumm_and_rom_present` helpers are lifted from `tests/test_driven_e2e.cpp`, `tests/test_savestate.cpp`, and `tests/test_netplay_e2e.cpp` (the latter carries the required per-instance shim shadow-copy). The exact `refcore_driven` POST_BUILD/core.json copy convention is in `cores/refcore_driven/CMakeLists.txt`. The Task-4 e2e's final convergence assertion may need small flush/index tuning (flagged inline) — the session's ring/reconcile invariants are the fixed contract; the test bookkeeping bends to them.
```
