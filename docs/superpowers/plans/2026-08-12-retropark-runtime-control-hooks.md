# RetroPark Runtime Control Hooks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use `- [ ]` checkbox syntax.

**Goal:** Add `pause`/`resume`/`reset`/`get_status` to the RetroPark runtime so a frontend can build a
RetroArch-style hotkey + menu layer on top, with no `rp_core_abi` bump.

**Architecture:** Four C functions + one status struct on the public runtime API, backed by a `paused_` flag on
`rp::Runtime`. Pause branches on core type: driven cores stop advancing (re-composite the last driven frame);
presenting cores re-composite the last ring frame (a *repeat*, which `VulkanBackend::composite_and_present`
already supports) and the runtime mutes forwarded audio. Reset reboots the current content through the existing
loader lifecycle. No render-backend or ABI changes.

**Tech Stack:** C ABI (`include/retropark/retropark.h`), C++ runtime (`src/runtime/Runtime.{h,cpp}`), doctest
(`tests/`), CMake/VS. Build: the `retropark_tests` target (Debug) is the gate; `ctest`/run the exe.

## Global Constraints

- **No `rp_core_abi` bump** — ABI stays v5. Reuse existing loader/backend primitives only.
- Public API is C: `extern "C"`, `rp_result` returns, null-arg guards in the C wrappers.
- Follow existing Runtime patterns (the C wrappers reinterpret_cast to `rp::Runtime`; methods return `rp_result`).
- Idempotency: `pause` while paused / `resume` while running → `RP_OK`. Control calls with no content loaded →
  `RP_OK` no-op. `get_status(out==null)` → `RP_ERR_BAD_ARG`.
- No AI attribution in commits.

---

### Task 1: `get_status` + the API scaffold (header, flag, C wrappers)

**Files:**
- Modify: `include/retropark/retropark.h` (add 4 decls + `rp_runtime_status`)
- Modify: `src/runtime/Runtime.h` (methods + `paused_`, `content_path_`, `core_dir_`, `core_id_`, fps fields,
  last-ready fields)
- Modify: `src/runtime/Runtime.cpp` (impls + C wrappers)
- Test: `tests/test_runtime_control.cpp` (new); register in `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `rp_runtime_pause/resume/reset/get_status` (C), `Runtime::pause/resume/reset/get_status` (C++),
  `rp_runtime_status`.
- Consumes: existing `Runtime` members `core_type_`, `content_loaded_`, `api_`.

- [ ] **Step 1: Write the failing test.**

```cpp
// tests/test_runtime_control.cpp
#include "doctest.h"
#include <retropark/retropark.h>
#include <vector>
#ifndef RP_VK_CORE_DIR
#define RP_VK_CORE_DIR "."
#endif

TEST_CASE("runtime control: get_status reflects core type + pause flag") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    rp_runtime_status st{};
    // no core yet
    CHECK(rp_runtime_get_status(rt, &st) == RP_OK);
    CHECK(st.content_loaded == 0);
    CHECK(rp_runtime_get_status(rt, nullptr) == RP_ERR_BAD_ARG);
    // load the reference presenting core (content-free: it renders without load_content)
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_VK_CORE_DIR) == RP_OK);
    REQUIRE(rp_runtime_get_status(rt, &st) == RP_OK);
    CHECK(st.core_type == RP_CORE_PRESENTING);
    CHECK(st.graphics_api == RP_GFX_VULKAN);
    CHECK(st.paused == 0);
    CHECK(rp_runtime_pause(rt) == RP_OK);
    REQUIRE(rp_runtime_get_status(rt, &st) == RP_OK);
    CHECK(st.paused == 1);
    CHECK(rp_runtime_resume(rt) == RP_OK);
    REQUIRE(rp_runtime_get_status(rt, &st) == RP_OK);
    CHECK(st.paused == 0);
    rp_runtime_destroy(rt);
}
```

- [ ] **Step 2: Add the header surface** — in `include/retropark/retropark.h`, before the closing `#ifdef
  __cplusplus`:

```c
/* --- Runtime control (frontend builds hotkeys/menus on top of these) --------------------------------- */
/* Pause/resume the running core. Driven: advancing stops (last frame re-composites). Presenting: the
   compositor freezes on the last frame and forwarded audio is muted; the core keeps simulating underneath.
   Idempotent; RP_OK with no content loaded (no-op). */
rp_result rp_runtime_pause (rp_runtime* rt);
rp_result rp_runtime_resume(rp_runtime* rt);
/* Reboot the current content (Phase 1: full stop + reload of the same path). Clears pause. RP_OK with no
   content loaded (no-op). */
rp_result rp_runtime_reset (rp_runtime* rt);

typedef struct rp_runtime_status {
    uint32_t core_type;       /* rp_core_type */
    uint32_t graphics_api;    /* rp_graphics_api */
    int32_t  paused;          /* 0/1 */
    int32_t  content_loaded;  /* 0/1 */
    double   fps;             /* measured present rate (0 until measured) */
} rp_runtime_status;
/* Fill *out with the runtime's current state. RP_ERR_BAD_ARG if rt or out is null. */
rp_result rp_runtime_get_status(rp_runtime* rt, rp_runtime_status* out);
```

- [ ] **Step 3: Add Runtime members** — in `src/runtime/Runtime.h` `private:` section, alongside the existing
  flags:

```cpp
    bool paused_ = false;
    std::string content_path_;   // last-loaded content path, for reset()
    std::string core_dir_;       // last dynamic core dir (reset fallback; empty for static)
    std::string core_id_;        // last static core id (empty for dynamic)
    // Presenting-core pause freeze: re-composite the last ring frame instead of acquiring a new one.
    uint32_t last_ready_idx_ = 0; uint64_t last_ready_sv_ = 0; bool have_last_ready_ = false;
    // fps measurement (present rate).
    double fps_ = 0.0; uint64_t fps_count_ = 0; uint64_t fps_t0_ns_ = 0;
```
  and declare the methods in `public:`:
```cpp
    rp_result pause();
    rp_result resume();
    rp_result reset();
    rp_result get_status(rp_runtime_status* out);
```

- [ ] **Step 4: Implement pause/resume/get_status** — in `src/runtime/Runtime.cpp` (before the `} // namespace
  rp`). `reset()` is stubbed here (Task 4 fills it); pause/resume are real:

```cpp
rp_result Runtime::pause()  { if (core_loaded_) paused_ = true;  return RP_OK; }
rp_result Runtime::resume() { paused_ = false; return RP_OK; }

rp_result Runtime::get_status(rp_runtime_status* out) {
    if (!out) return RP_ERR_BAD_ARG;
    out->core_type      = (uint32_t)core_type_;
    out->graphics_api   = (uint32_t)api_;
    out->paused         = paused_ ? 1 : 0;
    out->content_loaded = content_loaded_ ? 1 : 0;
    out->fps            = fps_;
    return RP_OK;
}

rp_result Runtime::reset() { return RP_OK; }   // Task 4
```

- [ ] **Step 5: Add C wrappers** — in the `extern "C"` block of `Runtime.cpp`:

```cpp
rp_result rp_runtime_pause (rp_runtime* rt) { return rt ? reinterpret_cast<Runtime*>(rt)->pause()  : RP_ERR_BAD_ARG; }
rp_result rp_runtime_resume(rp_runtime* rt) { return rt ? reinterpret_cast<Runtime*>(rt)->resume() : RP_ERR_BAD_ARG; }
rp_result rp_runtime_reset (rp_runtime* rt) { return rt ? reinterpret_cast<Runtime*>(rt)->reset()  : RP_ERR_BAD_ARG; }
rp_result rp_runtime_get_status(rp_runtime* rt, rp_runtime_status* out) {
    if (!rt) return RP_ERR_BAD_ARG;
    return reinterpret_cast<Runtime*>(rt)->get_status(out);
}
```

- [ ] **Step 6: Register the test** — in `tests/CMakeLists.txt`, add `test_runtime_control.cpp` to the
  `retropark_tests` sources (follow how `test_vulkan_e2e.cpp` is listed; it inherits the `RP_VK_CORE_DIR`
  define already set for the target).

- [ ] **Step 7: Build + run the gate.** Run: build `retropark_tests` (Debug) and run it filtered to
  `"runtime control*"`. Expected: PASS.

- [ ] **Step 8: Commit.** `git add include/retropark/retropark.h src/runtime/Runtime.* tests/ && git commit -m
  "feat(runtime): pause/resume/reset/get_status control surface (scaffold)"`

### Task 2: Driven-core pause freeze

**Files:** Modify `src/runtime/Runtime.cpp` (`present`). Test: `tests/test_runtime_control.cpp`.

**Interfaces:** Consumes `paused_`; changes `present()` so a paused driven core does not `advance`.

- [ ] **Step 1: Write the failing test** (uses `refcore_driven`; `RP_DRIVEN_CORE_DIR` — add the define to
  `tests/CMakeLists.txt` mirroring `RP_VK_CORE_DIR`, pointing at the built `refcore_driven` dir):

```cpp
TEST_CASE("runtime control: driven pause freezes the frame") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);   // content-free driven ref core
    std::vector<uint8_t> a(64*64*4), b(64*64*4), c(64*64*4);
    REQUIRE(rp_runtime_present(rt, a.data()) == RP_OK);
    REQUIRE(rp_runtime_present(rt, b.data()) == RP_OK);
    CHECK(a != b);                                   // animates while running
    REQUIRE(rp_runtime_pause(rt) == RP_OK);
    REQUIRE(rp_runtime_present(rt, a.data()) == RP_OK);
    REQUIRE(rp_runtime_present(rt, b.data()) == RP_OK);
    CHECK(a == b);                                   // frozen while paused
    REQUIRE(rp_runtime_resume(rt) == RP_OK);
    REQUIRE(rp_runtime_present(rt, c.data()) == RP_OK);
    CHECK(c != b);                                   // resumes
    rp_runtime_destroy(rt);
}
```
  Run it: expect FAIL (currently `present` still advances while paused).

- [ ] **Step 2: Skip advance when paused** — in `Runtime::present`, the driven branch:

```cpp
rp_result Runtime::present(uint8_t* out_rgba) {
    if (core_loaded_ && core_type_ == RP_CORE_DRIVEN) {
        if (!paused_) {                     // paused: do not advance; re-composite the retained frame
            rp_result r = advance(1);
            if (r != RP_OK) return r;
        }
        return render(out_rgba);
    }
    return render(out_rgba);
}
```
  (The retained `dr_data_`/`dr_have_` from the last advance make `render` re-composite the last driven frame.)

- [ ] **Step 3: Run the test.** Expected: PASS. **Step 4: Commit** (`feat(runtime): driven pause freeze`).

### Task 3: Presenting-core pause freeze + audio mute

**Files:** Modify `src/runtime/Runtime.cpp` (`render`, `on_audio_sample`). Test: `tests/test_runtime_control.cpp`.

**Interfaces:** Consumes `paused_`; caches last ring `(idx, sv)`; mutes audio when paused.

- [ ] **Step 1: Write the failing test** (presenting freeze via `refcore_present_vk`, which animates a
  rising-blue clear — same core the e2e test uses):

```cpp
TEST_CASE("runtime control: presenting pause freezes the frame") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_VK_CORE_DIR) == RP_OK);
    std::vector<uint8_t> a(64*64*4), b(64*64*4), c(64*64*4);
    // let the producer get a few frames in, then sample two moving frames
    for (int i = 0; i < 8; i++) rp_runtime_present(rt, a.data());
    rp_runtime_present(rt, a.data());
    for (int i = 0; i < 8; i++) rp_runtime_present(rt, b.data());
    // (animation is slow; the assertion that matters is the freeze below)
    REQUIRE(rp_runtime_pause(rt) == RP_OK);
    rp_runtime_present(rt, a.data());
    rp_runtime_present(rt, b.data());
    CHECK(a == b);                                   // frozen while paused: consecutive presents identical
    rp_runtime_destroy(rt);
}
```
  Run it: expect FAIL (paused presenting still consumes new ring frames → not identical).

- [ ] **Step 2: Freeze via last-ready repeat + mute audio** — in `Runtime::render`, presenting branch:

```cpp
    // presenting
    uint32_t idx = 0; uint64_t sv = 0;
    bool has;
    if (paused_) {
        // Re-present the LAST acquired frame (a repeat): composite_and_present re-composites
        // surfaces_[idx] without re-acquiring / advancing the timeline. Nothing yet => black.
        idx = last_ready_idx_; sv = last_ready_sv_; has = have_last_ready_;
    } else {
        has = ring_.latest_ready(idx, sv);
        if (has) { last_ready_idx_ = idx; last_ready_sv_ = sv; have_last_ready_ = true; }
    }
    return backend_->composite_and_present(idx, sv, has, out_rgba, err);
```
  and in `Runtime::on_audio_sample`, extend the early-return:
```cpp
    if (suppress_audio_ || paused_) return;      // rollback silence OR paused mute
```

- [ ] **Step 3: Run the test.** Expected: PASS. Also re-run Task 1/2 tests (no regression). **Step 4: Commit**
  (`feat(runtime): presenting pause freeze + audio mute`).

### Task 4: Reset (reboot current content)

**Files:** Modify `src/runtime/Runtime.cpp` (`load_content` to store the path; `reset`), `Runtime.h` already has
`content_path_`/`core_dir_`/`core_id_`. Test: `tests/test_runtime_control.cpp`.

**Interfaces:** `reset()` reboots the content; clears `paused_`, `have_last_ready_`.

- [ ] **Step 1: Store the content path.** At the top of `Runtime::load_content` (after the `!core_loaded_`
  guard), add `content_path_ = path ? path : "";`. In `load_core` set `core_dir_ = core_dir; core_id_.clear();`
  and in `load_static_core` set `core_id_ = core_id; core_dir_.clear();` (both right after `finish_load_core`
  succeeds, i.e. change the `return finish_load_core(...)` to capture the result, set the id, then return it).

- [ ] **Step 2: Write the failing test** (reboot resets the ref core's animation to its start; `refcore_driven`
  is content-free so `reset` re-boots the instance — assert the post-reset first frame equals the fresh-boot
  first frame):

```cpp
TEST_CASE("runtime control: reset reboots content") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);
    std::vector<uint8_t> first(64*64*4), later(64*64*4), afterReset(64*64*4);
    REQUIRE(rp_runtime_present(rt, first.data()) == RP_OK);
    for (int i = 0; i < 10; i++) rp_runtime_present(rt, later.data());
    CHECK(first != later);
    REQUIRE(rp_runtime_reset(rt) == RP_OK);
    REQUIRE(rp_runtime_present(rt, afterReset.data()) == RP_OK);
    CHECK(afterReset == first);                      // rebooted to frame 0
    rp_runtime_destroy(rt);
}
```

- [ ] **Step 3: Implement reset** — replace the Task-1 stub:

```cpp
rp_result Runtime::reset() {
    if (!core_loaded_ || !content_loaded_) { paused_ = false; return RP_OK; }
    std::string err;
    paused_ = false;
    have_last_ready_ = false;
    if (core_type_ == RP_CORE_PRESENTING) {
        // Stop the running instance, then re-run the content-load+start path (surfaces stay valid).
        if (loader_.state() == LoaderState::Started) loader_.stop(err);
        content_loaded_ = false;
        return load_content(content_path_.c_str());
    }
    // Driven: re-load the same content (reboots the core's sim). Content-free ref core: this
    // re-loads with an empty path, which the ref core treats as a fresh boot.
    content_loaded_ = false;
    return load_content(content_path_.c_str());
}
```
  Note: `load_content` on a content-free driven core is not currently reachable (the driven ref core is
  content-free and `finish_load_core` opens it directly). If the driven ref core is content-free, add a small
  reboot path: for a content-free driven core, `reset()` calls `loader_.destroy()` + `loader_.create()` +
  `finish_load_core` again (a clean in-place reboot without reloading the DLL). Prefer whichever the ref core
  supports; the test asserts frame-0 equality either way. **Pick the content-free reboot** if `refcore_driven`
  takes no content:
```cpp
    // content-free driven reboot (no reload of the DLL):
    loader_.destroy();
    if (loader_.create(&host_iface_, err) != RP_OK) return RP_ERR_INTERNAL;
    return finish_load_core(core_type_, err);
```

- [ ] **Step 4: Run the test** (adjust to whichever reboot path the ref core supports). Expected: PASS.
  **Step 5: Commit** (`feat(runtime): reset reboots current content`).

### Task 5: fps measurement

**Files:** Modify `src/runtime/Runtime.cpp` (`present`/`render`). Test: `tests/test_runtime_control.cpp`.

- [ ] **Step 1: Write the failing test** (lenient — fps is 0 before any present, > 0 after a burst; timing, so
  only assert non-negativity + that it becomes positive):

```cpp
TEST_CASE("runtime control: get_status reports fps after presents") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_VK_CORE_DIR) == RP_OK);
    std::vector<uint8_t> buf(64*64*4);
    rp_runtime_status st{};
    for (int i = 0; i < 120; i++) rp_runtime_present(rt, buf.data());
    REQUIRE(rp_runtime_get_status(rt, &st) == RP_OK);
    CHECK(st.fps >= 0.0);            // populated, never garbage
    rp_runtime_destroy(rt);
}
```

- [ ] **Step 2: Measure present rate** — add a private helper and call it at the end of `Runtime::present`
  (both branches) or once in `render` (the single common path). Use `std::chrono::steady_clock`; recompute
  `fps_` over a ~0.5s window:

```cpp
// at top of Runtime.cpp: #include <chrono>
void Runtime::tick_fps() {                       // declare in Runtime.h private
    using namespace std::chrono;
    uint64_t now = (uint64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    if (fps_t0_ns_ == 0) { fps_t0_ns_ = now; fps_count_ = 0; return; }
    fps_count_++;
    uint64_t dt = now - fps_t0_ns_;
    if (dt >= 500000000ull) {                    // 0.5s window
        fps_ = (double)fps_count_ * 1e9 / (double)dt;
        fps_t0_ns_ = now; fps_count_ = 0;
    }
}
```
  Call `tick_fps();` at the start of `Runtime::render` (the one path both driven and presenting present go
  through). Reset `fps_ = 0; fps_t0_ns_ = 0; fps_count_ = 0;` in `unload_core` and `reset`.

- [ ] **Step 3: Run the test.** Expected: PASS. **Step 4: Commit** (`feat(runtime): fps in get_status`).

### Task 6: Harness smoke test (Pause + Reset hotkeys)

**Files:** Modify `harness/windowed/main.cpp`.

- [ ] **Step 1:** In the `WndProc` `WM_KEYDOWN` handler (next to the existing `VK_F5`/`VK_F7` cases), add:
```cpp
        } else if (w == 'P') {                    // toggle pause
            rp_runtime_status st{}; rp_runtime_get_status(g_rt, &st);
            if (st.paused) rp_runtime_resume(g_rt); else rp_runtime_pause(g_rt);
        } else if (w == VK_F8) {                  // reset
            rp_runtime_reset(g_rt);
```
- [ ] **Step 2:** Build the harness; run it with a core (`--core <dir> --content <...>` or a driven core);
  press P (freezes/unfreezes) and F8 (reboots). Manual smoke — no automated assertion. **Step 3: Commit**
  (`feat(harness): Pause (P) + Reset (F8) hotkeys`).

## Self-Review notes
- Spec §1-6 covered: control surface (T1), pause split (T2/T3), audio mute (T3), reset (T4), status+fps
  (T1/T5), harness smoke (T6). No ABI bump anywhere. No `VulkanBackend` change (repeat path reused).
- Type names verified against the tree: `rp_core_type`/`rp_graphics_api` (abi header), `LoaderState::Started`,
  `loader_.stop/create/destroy`, `finish_load_core(rp_core_type, std::string&)`, `ring_.latest_ready`,
  `backend_->composite_and_present`.
- Open impl choice flagged in T4: driven reset path depends on whether `refcore_driven` is content-free
  (content-free → in-place destroy/create/finish reboot; content → re-`load_content`). The test asserts
  frame-0 equality regardless; the implementer picks the path the ref core supports.
