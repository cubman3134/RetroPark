# RetroPark Slice F (Savestate + Rewind) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Serialize/restore a driven core's state (savestate) and add a bounded per-frame ring for frame-by-frame rewind — wiring the already-declared serialize hooks + an additive C API, no core-ABI change.

**Architecture:** Fill in `rp_core_abi.serialize_size/serialize/unserialize` on the libretro shim (→ FCEUmm's `retro_serialize*`) and the reference driven core (its frame counter, for a portable test). `CoreLoader` gains serialize passthroughs. The runtime gets a buffer-based `save_state`/`load_state` C API and a bounded rewind ring captured at the top of each forward `present()`; `rewind()` restores the previous snapshot and re-runs one frame so the game steps back. Deterministic proof: save at A → advance → load → pixel-exact match.

**Tech Stack:** C++17, CMake, the existing driven/libretro/runtime pipeline, doctest. Real validation: FCEUmm + a NES ROM (present; git-ignored).

## Global Constraints

- **C++17. No Qt/EverythingBox.** MSVC `/W4 /permissive-`, warning-clean.
- **No core-ABI change.** `RETROPARK_ABI_VERSION` stays `4`; `rp_core_abi`/`rp_host_iface` unchanged (serialize hooks already exist). Only five additive `rp_runtime_*` C-API functions in `retropark.h`.
- **Driven cores only.** `save_state`/`load_state`/`serialize_size`/`set_rewind`/`rewind` on a presenting core, a no-serialize core, or no core → `RP_ERR_UNSUPPORTED`/`RP_ERR_INTERNAL`, never a crash. `save_state` buffer `< serialize_size` → `RP_ERR_BAD_ARG`. `load_state` the core rejects → `RP_ERR_UNSUPPORTED`. `rewind` with no history → `RP_ERR_NOT_FOUND`; when disabled → `RP_ERR_INTERNAL`.
- **Rewind ring** is uncompressed, bounded (`rewind_max_ × serialize_size`), captured at the TOP of each forward `present()` (pre-frame state). `serialize_size` assumed stable after load.
- **The whole A–E suite stays green** (serialize hooks fill existing nulls; save/rewind is new C API; presenting + existing driven/libretro/audio paths unaffected — `refcore_driven` gains serialize but its existing e2e is unchanged).
- **Gated FCEUmm tests** `WARN`-skip when the core/ROM are absent. Never commit FCEUmm/ROMs. Vulkan paths stay validation-clean; `export VULKAN_SDK=/c/VulkanSDK/1.4.357.0` before any fresh `cmake -S . -B build`.
- **Commits:** conventional prefixes. **No AI attribution** anywhere.

---

## File Structure

```
src/loader/CoreLoader.h/.cpp          # serialize_size/serialize/unserialize passthroughs   (Task 1)
cores/refcore_driven/RefCoreDriven.cpp   # serialize the frame counter                       (Task 2)
cores/libretro_shim/LibretroShim.cpp     # serialize -> retro_serialize*/unserialize          (Task 2)
include/retropark/retropark.h         # + 5 rp_runtime_* savestate/rewind fns                (Task 3,4)
src/runtime/Runtime.h/.cpp            # save/load (Task 3), rewind ring + capture (Task 4)
harness/windowed/main.cpp             # F5 save / F7 load / rewind key                        (Task 5)
tests/
  test_savestate.cpp                  # loader unit (T1), portable savestate (T3), rewind (T4), FCEUmm (T5)
```

---

## Task 1: CoreLoader serialize passthroughs

**Files:**
- Modify: `src/loader/CoreLoader.h`, `src/loader/CoreLoader.cpp`, `tests/test_loader.cpp`

**Interfaces:**
- Produces:
  - `size_t CoreLoader::serialize_size();` (0 if unsupported / wrong state)
  - `rp_result CoreLoader::serialize(void* data, size_t size, std::string& err);`
  - `rp_result CoreLoader::unserialize(const void* data, size_t size, std::string& err);`

- [ ] **Step 1: Write the failing test**

In `tests/test_loader.cpp`, extend the inline fake core with serialize hooks and add a round-trip test. Add to the fake's state and functions:
```cpp
// in the FakeCoreState struct: uint32_t save_val = 0;
size_t   fake_serialize_size(rp_core*) { return sizeof(uint32_t); }
rp_result fake_serialize(rp_core*, void* d, size_t n) { if (n<4) return RP_ERR_BAD_ARG; uint32_t v=0xABCD1234; memcpy(d,&v,4); return RP_OK; }
rp_result fake_unserialize(rp_core* c, const void* d, size_t n) { if (n<4) return RP_ERR_BAD_ARG; memcpy(&reinterpret_cast<FakeCoreState*>(c)->save_val, d, 4); return RP_OK; }
```
Add these to the `kGoodAbi` initializer (positions serialize_size, serialize, unserialize — replacing those three nullptrs; leave load_content nullptr last). New test:
```cpp
TEST_CASE("loader: serialize passthroughs round-trip") {
    g_fake = {};
    CoreLoader ld; std::string err; FakeModule mod(good_entry);
    REQUIRE(ld.load(&mod, err) == RP_OK);
    rp_host_iface host{}; REQUIRE(ld.create(&host, err) == RP_OK);
    CHECK(ld.serialize_size() == 4u);
    uint8_t buf[4] = {0};
    CHECK(ld.serialize(buf, 4, err) == RP_OK);
    CHECK(ld.unserialize(buf, 4, err) == RP_OK);
    CHECK(g_fake.save_val == 0xABCD1234u);   // round-tripped through the fake
    ld.destroy();
}
```

- [ ] **Step 2: Run — expect fail; implement**

Run: `cmake --build build --config Debug` → FAIL (unresolved). `src/loader/CoreLoader.h` declares the three methods; `src/loader/CoreLoader.cpp`:
```cpp
size_t CoreLoader::serialize_size() {
    if ((state_ != LoaderState::Created && state_ != LoaderState::Started) || !abi_ || !abi_->serialize_size) return 0;
    return abi_->serialize_size(core_);
}
rp_result CoreLoader::serialize(void* data, size_t size, std::string& error) {
    if (state_ != LoaderState::Created && state_ != LoaderState::Started) { error="serialize needs Created"; return RP_ERR_INTERNAL; }
    if (!abi_->serialize) { error="core has no serialize"; return RP_ERR_UNSUPPORTED; }
    return abi_->serialize(core_, data, size);
}
rp_result CoreLoader::unserialize(const void* data, size_t size, std::string& error) {
    if (state_ != LoaderState::Created && state_ != LoaderState::Started) { error="unserialize needs Created"; return RP_ERR_INTERNAL; }
    if (!abi_->unserialize) { error="core has no unserialize"; return RP_ERR_UNSUPPORTED; }
    return abi_->unserialize(core_, data, size);
}
```

- [ ] **Step 3: Build, run, commit**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the round-trip test passes; suite green.
```bash
git add src/loader/CoreLoader.* tests/test_loader.cpp
git commit -m "feat: CoreLoader serialize/unserialize/serialize_size passthroughs"
```

---

## Task 2: Implement serialize on the reference core + libretro shim

**Files:**
- Modify: `cores/refcore_driven/RefCoreDriven.cpp`, `cores/libretro_shim/LibretroShim.cpp`

**Interfaces:**
- Produces: both cores now implement the three serialize hooks in their `kAbi`. `refcore_driven` serializes its `uint32_t` frame counter; the shim forwards to `retro_serialize_size`/`retro_serialize`/`retro_unserialize`.

- [ ] **Step 1: refcore_driven serializes its counter**

In `cores/refcore_driven/RefCoreDriven.cpp`, add:
```cpp
size_t dc_serialize_size(rp_core*) { return sizeof(uint32_t); }
rp_result dc_serialize(rp_core* core, void* data, size_t size) {
    if (size < sizeof(uint32_t)) return RP_ERR_BAD_ARG;
    uint32_t f = reinterpret_cast<DrivenCore*>(core)->frame;
    memcpy(data, &f, sizeof(uint32_t)); return RP_OK;
}
rp_result dc_unserialize(rp_core* core, const void* data, size_t size) {
    if (size < sizeof(uint32_t)) return RP_ERR_BAD_ARG;
    memcpy(&reinterpret_cast<DrivenCore*>(core)->frame, data, sizeof(uint32_t)); return RP_OK;
}
```
Replace the three serialize nullptrs in `kAbi` with `dc_serialize_size, dc_serialize, dc_unserialize` (positions after `run_frame`; keep `load_content` last as nullptr). Include `<cstring>` if not already.

- [ ] **Step 2: libretro shim forwards to retro_serialize***

In `cores/libretro_shim/LibretroShim.cpp`: add the fn-pointer members + resolve them in `sh_create` (alongside the existing loads):
```cpp
// in Shim struct:
size_t (*retro_serialize_size)() = nullptr;
bool   (*retro_serialize)(void*, size_t) = nullptr;
bool   (*retro_unserialize)(const void*, size_t) = nullptr;
// in sh_create (with the other load_fn calls):
load_fn(s, s->retro_serialize_size, "retro_serialize_size");
load_fn(s, s->retro_serialize, "retro_serialize");
load_fn(s, s->retro_unserialize, "retro_unserialize");
```
(If `retro_serialize_size` is already resolved from Slice D, don't duplicate the member — reuse it.) Add the ABI fns:
```cpp
size_t sh_serialize_size(rp_core* core) {
    auto* s = reinterpret_cast<Shim*>(core);
    return s->retro_serialize_size ? s->retro_serialize_size() : 0;
}
rp_result sh_serialize(rp_core* core, void* data, size_t size) {
    auto* s = reinterpret_cast<Shim*>(core);
    if (!s->retro_serialize) return RP_ERR_UNSUPPORTED;
    return s->retro_serialize(data, size) ? RP_OK : RP_ERR_UNSUPPORTED;
}
rp_result sh_unserialize(rp_core* core, const void* data, size_t size) {
    auto* s = reinterpret_cast<Shim*>(core);
    if (!s->retro_unserialize) return RP_ERR_UNSUPPORTED;
    return s->retro_unserialize(data, size) ? RP_OK : RP_ERR_UNSUPPORTED;
}
```
Replace the three serialize nullptrs in the shim's `kAbi` with `sh_serialize_size, sh_serialize, sh_unserialize` (keep `sh_load_content` last).

- [ ] **Step 3: Build — confirm both cores emit, suite green**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: builds; refcore_driven + libretro_shim re-emit; the whole A–E suite stays green (existing e2e unaffected — behavior proven in Tasks 3/5).

- [ ] **Step 4: Commit**

```bash
git add cores/refcore_driven/RefCoreDriven.cpp cores/libretro_shim/LibretroShim.cpp
git commit -m "feat: serialize hooks on the reference driven core + libretro shim"
```

---

## Task 3: Runtime save/load C API + portable savestate e2e

**Files:**
- Modify: `include/retropark/retropark.h`, `src/runtime/Runtime.h`, `src/runtime/Runtime.cpp`
- Create: `tests/test_savestate.cpp`; Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `size_t rp_runtime_serialize_size(rp_runtime*);`
  - `rp_result rp_runtime_save_state(rp_runtime*, void* buf, size_t size);`
  - `rp_result rp_runtime_load_state(rp_runtime*, const void* buf, size_t size);`

- [ ] **Step 1: Write the failing portable savestate e2e**

`tests/test_savestate.cpp`:
```cpp
#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include <vector>
#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif
TEST_CASE("savestate: refcore_driven save->advance->load restores deterministically") {
    const uint32_t W=64,H=64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);
    for (int i=0;i<10;i++) { std::vector<uint8_t> t(W*H*4,0); rp_runtime_present(rt, t.data()); }  // advance
    size_t sz = rp_runtime_serialize_size(rt);
    REQUIRE(sz == 4);                                   // the frame counter
    std::vector<uint8_t> state(sz, 0);
    REQUIRE(rp_runtime_save_state(rt, state.data(), sz) == RP_OK);
    std::vector<uint8_t> r1(W*H*4,0); rp_runtime_present(rt, r1.data());   // frame right after save
    for (int i=0;i<30;i++) { std::vector<uint8_t> t(W*H*4,0); rp_runtime_present(rt, t.data()); }  // advance further
    std::vector<uint8_t> mid(W*H*4,0); rp_runtime_present(rt, mid.data());
    CHECK(mid != r1);                                   // game moved on
    REQUIRE(rp_runtime_load_state(rt, state.data(), sz) == RP_OK);
    std::vector<uint8_t> r2(W*H*4,0); rp_runtime_present(rt, r2.data());   // frame right after load
    CHECK(r2 == r1);                                    // deterministic restore
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
TEST_CASE("savestate: unsupported on a runtime with no core") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr); REQUIRE(rt);
    uint8_t b[4]={0};
    CHECK(rp_runtime_serialize_size(rt) == 0);
    CHECK(rp_runtime_save_state(rt, b, 4) == RP_ERR_UNSUPPORTED);
    rp_runtime_destroy(rt);
}
```

- [ ] **Step 2: Header decls + Runtime impl**

`include/retropark/retropark.h` (extern "C"):
```c
size_t    rp_runtime_serialize_size(rp_runtime* rt);
rp_result rp_runtime_save_state(rp_runtime* rt, void* buf, size_t size);
rp_result rp_runtime_load_state(rp_runtime* rt, const void* buf, size_t size);
```
`src/runtime/Runtime.h`: declare `size_t serialize_size(); rp_result save_state(void* buf, size_t size); rp_result load_state(const void* buf, size_t size);`.
`src/runtime/Runtime.cpp`:
```cpp
size_t Runtime::serialize_size() {
    if (!core_loaded_ || core_type_ != RP_CORE_DRIVEN) return 0;
    return loader_.serialize_size();
}
rp_result Runtime::save_state(void* buf, size_t size) {
    if (!core_loaded_ || core_type_ != RP_CORE_DRIVEN) return RP_ERR_UNSUPPORTED;
    size_t sz = loader_.serialize_size();
    if (sz == 0) return RP_ERR_UNSUPPORTED;
    if (size < sz) return RP_ERR_BAD_ARG;
    std::string err; return loader_.serialize(buf, sz, err);
}
rp_result Runtime::load_state(const void* buf, size_t size) {
    if (!core_loaded_ || core_type_ != RP_CORE_DRIVEN) return RP_ERR_UNSUPPORTED;
    std::string err; return loader_.unserialize(buf, size, err);
}
```
C API shims:
```cpp
size_t rp_runtime_serialize_size(rp_runtime* rt) { return reinterpret_cast<Runtime*>(rt)->serialize_size(); }
rp_result rp_runtime_save_state(rp_runtime* rt, void* b, size_t n) { return reinterpret_cast<Runtime*>(rt)->save_state(b,n); }
rp_result rp_runtime_load_state(rp_runtime* rt, const void* b, size_t n) { return reinterpret_cast<Runtime*>(rt)->load_state(b,n); }
```

- [ ] **Step 3: Wire CMake, build, run**

`tests/CMakeLists.txt`: add `test_savestate.cpp` (RP_DRIVEN_CORE_DIR is already a compile-def + `add_dependencies(retropark_tests refcore_driven)` from Slice C).
Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the portable savestate e2e passes (r2 == r1, deterministic restore); the no-core case returns UNSUPPORTED; the whole suite stays green.

- [ ] **Step 4: Commit**

```bash
git add include/retropark/retropark.h src/runtime/Runtime.* tests/test_savestate.cpp tests/CMakeLists.txt
git commit -m "feat: runtime save_state/load_state C API + deterministic savestate e2e"
```

---

## Task 4: Rewind ring + rewind e2e

**Files:**
- Modify: `include/retropark/retropark.h`, `src/runtime/Runtime.h`, `src/runtime/Runtime.cpp`, `tests/test_savestate.cpp`

**Interfaces:**
- Produces:
  - `rp_result rp_runtime_set_rewind(rp_runtime*, int enabled, uint32_t max_snapshots);`
  - `rp_result rp_runtime_rewind(rp_runtime*);`

- [ ] **Step 1: Write the failing rewind e2e**

Add to `tests/test_savestate.cpp`:
```cpp
TEST_CASE("rewind: refcore_driven steps the frame counter backward") {
    const uint32_t W=64,H=64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr); REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);
    REQUIRE(rp_runtime_set_rewind(rt, 1, 100) == RP_OK);
    std::vector<uint8_t> t(W*H*4,0);
    for (int i=0;i<50;i++) rp_runtime_present(rt, t.data());          // 50 forward frames captured
    uint32_t before=0; { uint8_t b[4]; rp_runtime_save_state(rt,b,4); memcpy(&before,b,4); }
    for (int i=0;i<10;i++) { REQUIRE(rp_runtime_rewind(rt) == RP_OK); rp_runtime_present(rt, t.data()); }  // rewind 10
    uint32_t after=0; { uint8_t b[4]; rp_runtime_save_state(rt,b,4); memcpy(&after,b,4); }
    CHECK(after < before);                        // went backward
    CHECK(before - after >= 8);                   // ~10 frames back (allow slack for off-by-one)
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
TEST_CASE("rewind: empty history -> NOT_FOUND, disabled -> INTERNAL") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr); REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);
    CHECK(rp_runtime_rewind(rt) == RP_ERR_INTERNAL);       // rewind not enabled
    REQUIRE(rp_runtime_set_rewind(rt, 1, 100) == RP_OK);
    CHECK(rp_runtime_rewind(rt) == RP_ERR_NOT_FOUND);      // enabled but no history yet
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
```

- [ ] **Step 2: Header decls + Runtime rewind**

`retropark.h` (extern "C"):
```c
rp_result rp_runtime_set_rewind(rp_runtime* rt, int enabled, uint32_t max_snapshots);
rp_result rp_runtime_rewind(rp_runtime* rt);
```
`Runtime.h`: `#include <deque>`; members:
```cpp
bool rewind_enabled_ = false;
bool rewind_replay_ = false;
uint32_t rewind_max_ = 0;
std::deque<std::vector<uint8_t>> rewind_ring_;
```
declare `rp_result set_rewind(int enabled, uint32_t max_snapshots); rp_result rewind();`.
`Runtime.cpp`:
```cpp
rp_result Runtime::set_rewind(int enabled, uint32_t max_snapshots) {
    if (!core_loaded_ || core_type_ != RP_CORE_DRIVEN || loader_.serialize_size() == 0) return RP_ERR_UNSUPPORTED;
    rewind_enabled_ = (enabled != 0);
    rewind_max_ = max_snapshots ? max_snapshots : 600;
    rewind_ring_.clear(); rewind_replay_ = false;
    return RP_OK;
}
rp_result Runtime::rewind() {
    if (!rewind_enabled_) return RP_ERR_INTERNAL;
    if (rewind_ring_.size() < 2) return RP_ERR_NOT_FOUND;      // need a prior frame to step back to
    rewind_ring_.pop_back();                                   // discard the just-displayed frame's pre-state
    const std::vector<uint8_t>& prev = rewind_ring_.back();    // previous frame's pre-state
    std::string err;
    rp_result r = loader_.unserialize(prev.data(), prev.size(), err);
    if (r != RP_OK) return r;
    rewind_replay_ = true;   // next present() re-runs this frame WITHOUT capturing
    return RP_OK;
}
```
In `present()`'s DRIVEN branch, at the very top (before `run_frame`), add the capture/replay logic:
```cpp
if (rewind_replay_) {
    rewind_replay_ = false;                 // this present replays the restored frame; do NOT capture
} else if (rewind_enabled_) {
    size_t sz = loader_.serialize_size();
    if (sz > 0) {
        std::vector<uint8_t> snap(sz);
        std::string se;
        if (loader_.serialize(snap.data(), sz, se) == RP_OK) {
            rewind_ring_.push_back(std::move(snap));
            while (rewind_ring_.size() > rewind_max_) rewind_ring_.pop_front();
        }
    }
}
// ... existing driven run_frame + composite_driven unchanged below ...
```
In `unload_core` / `set_rewind` reset: also clear `rewind_ring_`, `rewind_enabled_=false`, `rewind_replay_=false` in `unload_core`.
C API shims:
```cpp
rp_result rp_runtime_set_rewind(rp_runtime* rt, int e, uint32_t m) { return reinterpret_cast<Runtime*>(rt)->set_rewind(e,m); }
rp_result rp_runtime_rewind(rp_runtime* rt) { return reinterpret_cast<Runtime*>(rt)->rewind(); }
```

- [ ] **Step 3: Build and run**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the rewind e2e passes (`after < before`, stepped back ~10); the empty/disabled cases return `RP_ERR_NOT_FOUND`/`RP_ERR_INTERNAL`; the whole suite stays green (rewind disabled by default, so no other path changes).

- [ ] **Step 4: Commit**

```bash
git add include/retropark/retropark.h src/runtime/Runtime.* tests/test_savestate.cpp
git commit -m "feat: bounded per-frame rewind ring (set_rewind + rewind)"
```

---

## Task 5: Gated FCEUmm savestate e2e + harness save/load/rewind

**Files:**
- Modify: `tests/test_savestate.cpp`, `tests/CMakeLists.txt`, `harness/windowed/main.cpp`

**Interfaces:**
- Consumes: the C API + the `libretro_shim` package (FCEUmm) + a ROM.

- [ ] **Step 1: Gated FCEUmm savestate e2e**

Add to `tests/test_savestate.cpp` (reuse the `RP_SHIM_DIR`/`RP_NES_ROM_DIR` defs + `first_nes`/`file_exists` helpers — duplicate the small `static` helpers here to avoid ODR clashes, as the other test files do):
```cpp
TEST_CASE("savestate: FCEUmm save->advance->load restores deterministically (gated)") {
    std::string rom = savestate_first_nes(RP_NES_ROM_DIR);
    if (rom.empty() || !savestate_file_exists(std::string(RP_SHIM_DIR)+"/fceumm_libretro.dll")) { WARN("no core/rom; skip"); return; }
    const uint32_t W=256,H=240;
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr); REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_SHIM_DIR) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, rom.c_str()) == RP_OK);
    std::vector<uint8_t> t(W*H*4,0);
    for (int i=0;i<60;i++) rp_runtime_present(rt, t.data());          // past boot
    size_t sz = rp_runtime_serialize_size(rt);
    REQUIRE(sz > 0);
    std::vector<uint8_t> state(sz,0);
    REQUIRE(rp_runtime_save_state(rt, state.data(), sz) == RP_OK);
    std::vector<uint8_t> r1(W*H*4,0); rp_runtime_present(rt, r1.data());   // frame after save
    for (int i=0;i<60;i++) rp_runtime_present(rt, t.data());               // advance ~1s
    std::vector<uint8_t> mid(W*H*4,0); rp_runtime_present(rt, mid.data());
    CHECK(mid != r1);                                                       // game advanced
    REQUIRE(rp_runtime_load_state(rt, state.data(), sz) == RP_OK);
    std::vector<uint8_t> r2(W*H*4,0); rp_runtime_present(rt, r2.data());   // frame after load
    CHECK(r2 == r1);                                                        // deterministic restore
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
```
`tests/CMakeLists.txt`: `test_savestate.cpp` already added (Task 3); the `RP_SHIM_DIR`/`RP_NES_ROM_DIR` defs + `add_dependencies(retropark_tests LibretroShim)` exist from Slice D — confirm they cover this file (they're target-wide).

- [ ] **Step 2: Build and run — verify (or SKIP)**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the FCEUmm savestate e2e RUNS (core/ROM present) and passes — `r2 == r1` (a real NES game restored pixel-exact from a savestate); the whole suite green. **Report RAN vs SKIPPED and that r2==r1.**

- [ ] **Step 3: Harness save/load/rewind keys**

In `harness/windowed/main.cpp`: keep a savestate buffer. In the message/present loop, poll keys (via `GetAsyncKeyState` or `WM_KEYDOWN`):
- **F5** (save): `size_t sz = rp_runtime_serialize_size(g_rt); if (sz){ save_buf.resize(sz); rp_runtime_save_state(g_rt, save_buf.data(), sz); }`
- **F7** (load): `if (!save_buf.empty()) rp_runtime_load_state(g_rt, save_buf.data(), save_buf.size());`
- **Rewind** (e.g. hold `Backspace`): on first press, `rp_runtime_set_rewind(g_rt, 1, 600)` once (or enable rewind at startup for `--content`); while held, each loop iteration call `rp_runtime_rewind(g_rt)` (ignore `RP_ERR_NOT_FOUND`) BEFORE `rp_runtime_present(...)` — so the game steps backward. When not held, normal forward `present` (which captures). Enable rewind (`set_rewind(1,600)`) after `load_content` for the content path so the ring captures from the start.
Keep the present loop otherwise unchanged.

- [ ] **Step 4: Build + manually verify rewind**

Run: `cmake --build build --config Debug`. Launch `retropark_harness.exe --api d3d11 --content "<a NES rom>"`, let it run a few seconds, then hold Backspace and confirm the game visibly runs **backward** (e.g. Donkey Kong's barrels roll back up), release and it resumes forward; press F5 then advance then F7 and confirm it jumps back to the saved point. Capture a screenshot to `C:\Users\cubma\source\repos\RetroPark\.superpowers\sdd\harness-rewind-shot.png`. Note the manual result in the report (rewind can't be shown in a single screenshot; describe what you observed). Do not block the commit on the manual check if the automated FCEUmm savestate e2e passed.

- [ ] **Step 5: Commit**

```bash
git add tests/test_savestate.cpp tests/CMakeLists.txt harness/windowed/main.cpp
git commit -m "test+feat: gated FCEUmm savestate e2e + harness save/load/rewind"
```

---

## Self-Review

**Spec coverage:**
- §1 fill serialize hooks: refcore_driven (counter) + shim (retro_*) → Task 2; CoreLoader passthroughs → Task 1. ✓
- §1 runtime + C API (serialize_size/save_state/load_state) → Task 3; (set_rewind/rewind) → Task 4. ✓
- §2 savestate flow (query size → save → load) → Task 3; rewind ring (capture at top of forward present; rewind pops newest, restores previous, replay one frame) → Task 4. ✓
- §3 no core-ABI change (ABI stays 4; additive C API only) → Tasks 3/4 (no RETROPARK_ABI_VERSION edit). ✓
- §4 error handling: unsupported (no core/presenting/null hooks), buffer too small (BAD_ARG), incompatible load (UNSUPPORTED), rewind empty (NOT_FOUND) / disabled (INTERNAL) → Tasks 1,3,4 (loader guards + runtime checks + the rewind test cases). ✓
- §5 testing: loader unit (T1), portable savestate deterministic R1==R2 (T3), rewind stepped-back + empty/disabled (T4), gated FCEUmm savestate R1==R2 (T5), harness manual (T5), A–E regression (every task). ✓
- §6 scope: all in-scope built; presenting savestate / delta-compression / audio-rewind / files-slots / netplay NOT built. ✓

**Placeholder scan:** the harness key-poll (Task 5 Step 3) names the exact keys (F5/F7/Backspace) and the exact C calls; not a vague TODO. All runtime/loader/core code is complete. No "add error handling"-style gaps.

**Type consistency:** `CoreLoader::serialize_size()/serialize(void*,size_t,std::string&)/unserialize(const void*,size_t,std::string&)`, `Runtime::serialize_size()/save_state(void*,size_t)/load_state(const void*,size_t)/set_rewind(int,uint32_t)/rewind()`, the five `rp_runtime_*` C fns, and the `kAbi` serialize slot order (serialize_size, serialize, unserialize — between `run_frame` and `load_content`) are used identically across Tasks 1–5. The rewind ring is `std::deque<std::vector<uint8_t>>` throughout.
```
