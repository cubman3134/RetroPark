# RetroPark core-options parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** RetroPark libretro-shim cores (NES/fceumm, N64/mupen, future shims) get user-editable core options — global editor, in-game pause editor, per-game overrides — with the shim owning option harvesting behind the RetroPark ABI, and EverythingBox reusing its existing options UI + persistence.

**Architecture:** The shim harvests its wrapped core's option definitions and serves overrides on `GET_VARIABLE`. RetroPark's ABI (bumped v8→v9) carries options as a JSON descriptor plus get/set. EverythingBox parses the JSON into its existing `CoreOption` structs and drives its existing `editCoreOptions` UI, a new `RetroParkView` pause page, `OverrideLayer` per-game deltas, and `Settings` persistence.

**Tech Stack:** C++ (RetroPark runtime + libretro shim, MSVC/CMake, doctest); C++/Qt (EverythingBox, `QJsonDocument`, nav-kit UI).

**Design doc:** `docs/superpowers/specs/2026-08-16-retropark-core-options-design.md`

## Global Constraints

- **ABI bump v8→v9, strict-equality gate.** `RETROPARK_ABI_VERSION` = `9u`. Every bundled RetroPark core recompiles (trailing struct additions → existing cores' positional `kAbi` initializers leave the 3 new pointers `NULL`); the Dolphin vehicle DLL recompiles against v9 (Phase B). For the EB ExternalProject to actually rebuild, wipe `build/retropark_ext-prefix`.
- **Options JSON shape (exact):** `[{"key":"…","desc":"…","info":"…","default":"…","values":[{"value":"…","label":"…"}]}]`. Descriptor-only (no current value). Empty option set → `"[]"`.
- **Persistence key = underlying libretro core name** from `core.json`'s `libretro_core` (e.g. `mupen64plus_next`), shared with EB's native libretro backend.
- **No AI attribution** in any commit message, PR body, or issue comment (no `Co-Authored-By`, no "generated with").
- **Cores and ROMs are never committed** (git-ignored under `external/`).
- **RetroPark:** at the end of Phase A, merge to `main` + push `origin` (Phase B bumps the EB submodule to that pushed commit).
- **EverythingBox tree is shared across sessions:** do Phase B builds/commits in a throwaway worktree off current `origin/main` (e.g. `C:/Users/cubma/goliath-wt-coreopts`), never the shared tree. Deploy via **targeted copy** (exe + changed DLLs + shim), NEVER `robocopy /MIR`.

---

## Phase A — RetroPark (ABI + shim + runtime); merged & pushed at phase end

### Task A1: ABI v9 plumbing — core ABI + runtime C API + forwarding

**Files:**
- Modify: `include/retropark/retropark_abi.h` (version + `rp_core_abi`)
- Modify: `include/retropark/retropark.h` (runtime wrappers)
- Modify: `src/loader/CoreLoader.h`, `src/loader/CoreLoader.cpp`
- Modify: `src/runtime/Runtime.h`, `src/runtime/Runtime.cpp`
- Test: `tests/test_core_options.cpp` (create)
- Modify: `tests/CMakeLists.txt` (register the new test source)

**Interfaces:**
- Produces (core ABI, trailing members of `rp_core_abi`):
  - `const char* (*core_options_json)(rp_core* core);`
  - `const char* (*core_option_get)(rp_core* core, const char* key);`
  - `rp_result   (*core_option_set)(rp_core* core, const char* key, const char* value);`
- Produces (runtime C API): `rp_runtime_core_options_json(rp_runtime*)`, `rp_runtime_core_option_get(rp_runtime*, const char*)`, `rp_runtime_core_option_set(rp_runtime*, const char*, const char*)`.
- Produces (CoreLoader): `const char* core_options_json();`, `const char* core_option_get(const char* key);`, `rp_result core_option_set(const char* key, const char* value);` — each null-checks `abi_`/state/fptr and degrades gracefully.

- [ ] **Step 1: Write the failing test**

`tests/test_core_options.cpp` — plumbing degrades gracefully on a no-options core (refcore_driven leaves the new fptrs NULL):

```cpp
#include <doctest/doctest.h>
#include "retropark/retropark.h"
#include <cstring>
using namespace rp;

// A driven core with no option channel (fptrs NULL) must degrade gracefully through the whole
// runtime -> loader -> core forwarding chain: json "[]", get NULL, set RP_ERR_UNSUPPORTED.
TEST_CASE("core options: no-options core degrades gracefully") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_NONE, nullptr);
    REQUIRE(rt != nullptr);
    // refcore_driven has no options; load it from its build dir.
    REQUIRE(rp_runtime_load_core(rt, RP_REFCORE_DRIVEN_DIR) == RP_OK);

    const char* json = rp_runtime_core_options_json(rt);
    REQUIRE(json != nullptr);
    CHECK(std::strcmp(json, "[]") == 0);
    CHECK(rp_runtime_core_option_get(rt, "anything") == nullptr);
    CHECK(rp_runtime_core_option_set(rt, "anything", "1") == RP_ERR_UNSUPPORTED);

    rp_runtime_destroy(rt);
}
```

`RP_REFCORE_DRIVEN_DIR` is provided the same way the other e2e tests get their core dirs — a compile define set in `tests/CMakeLists.txt` pointing at `build/cores/refcore_driven` (mirror an existing `RP_*_DIR`/`add_compile_definitions` entry already used by a driven-core test; if the existing driven e2e uses a literal path, follow that exact pattern).

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target retropark_tests --config Release` then `./build/tests/Release/retropark_tests.exe --test-case="core options: no-options*"`
Expected: FAIL to COMPILE (the three `rp_runtime_core_option*` symbols don't exist yet).

- [ ] **Step 3: Bump the ABI version and add the core function pointers**

In `include/retropark/retropark_abi.h`:

```c
#define RETROPARK_ABI_VERSION 9u  /* was 8 -- rp_core_abi gains core_options_json + core_option_get/set (core-options parity) */
```

Append to the end of `struct rp_core_abi` (after `load_content`):

```c
    /* Core options (driven libretro-shim cores). A core with no options leaves these NULL.
       core_options_json returns a JSON array [{key,desc,info,default,values:[{value,label}]}]
       ("[]" if none), owned by the core, valid until destroy. core_option_get returns the current
       effective value (override else default) for key, or NULL if unknown. core_option_set records
       an override (RP_ERR_NOT_FOUND for an unknown key) so the core re-reads it. */
    const char* (*core_options_json)(rp_core* core);
    const char* (*core_option_get)(rp_core* core, const char* key);
    rp_result   (*core_option_set)(rp_core* core, const char* key, const char* value);
```

- [ ] **Step 4: Declare the runtime C API wrappers**

Append to `include/retropark/retropark.h` (before the closing `extern "C"`):

```c
/* Core options for the loaded driven core. options_json returns "[]" when no core is loaded or the
   core exposes no options; option_get returns NULL for unknown keys / no core; option_set returns
   RP_ERR_UNSUPPORTED if the loaded core has no option channel (e.g. a presenting core), or
   RP_ERR_NOT_FOUND for an unknown key. Returned strings are owned by the core (valid until the next
   core unload / runtime destroy). */
const char* rp_runtime_core_options_json(rp_runtime* rt);
const char* rp_runtime_core_option_get(rp_runtime* rt, const char* key);
rp_result   rp_runtime_core_option_set(rp_runtime* rt, const char* key, const char* value);
```

- [ ] **Step 5: Add the CoreLoader forwarding (null-checked, mirrors serialize)**

In `src/loader/CoreLoader.h`, add to the public section:

```cpp
    const char* core_options_json();
    const char* core_option_get(const char* key);
    rp_result   core_option_set(const char* key, const char* value);
```

In `src/loader/CoreLoader.cpp` (mirror the `serialize` guard: needs Created/Started + non-null fptr):

```cpp
const char* CoreLoader::core_options_json() {
    if ((state_ != LoaderState::Created && state_ != LoaderState::Started) || !abi_ || !abi_->core_options_json)
        return "[]";
    const char* j = abi_->core_options_json(core_);
    return j ? j : "[]";
}
const char* CoreLoader::core_option_get(const char* key) {
    if ((state_ != LoaderState::Created && state_ != LoaderState::Started) || !abi_ || !abi_->core_option_get)
        return nullptr;
    return abi_->core_option_get(core_, key);
}
rp_result CoreLoader::core_option_set(const char* key, const char* value) {
    if ((state_ != LoaderState::Created && state_ != LoaderState::Started) || !abi_ || !abi_->core_option_set)
        return RP_ERR_UNSUPPORTED;
    return abi_->core_option_set(core_, key, value);
}
```

- [ ] **Step 6: Add the Runtime methods + rp_runtime_* C entry points**

In `src/runtime/Runtime.h`, add public methods:

```cpp
    const char* core_options_json();
    const char* core_option_get(const char* key);
    rp_result   core_option_set(const char* key, const char* value);
```

In `src/runtime/Runtime.cpp` — the methods forward to `loader_`, and the C entry points forward to `Runtime` (mirror `rp_runtime_serialize_size` / `Runtime::serialize_size`):

```cpp
const char* Runtime::core_options_json() { return loader_.core_options_json(); }
const char* Runtime::core_option_get(const char* key) { return loader_.core_option_get(key); }
rp_result   Runtime::core_option_set(const char* key, const char* value) { return loader_.core_option_set(key, value); }

const char* rp_runtime_core_options_json(rp_runtime* rt) {
    if (!rt) return "[]";
    return reinterpret_cast<Runtime*>(rt)->core_options_json();
}
const char* rp_runtime_core_option_get(rp_runtime* rt, const char* key) {
    if (!rt || !key) return nullptr;
    return reinterpret_cast<Runtime*>(rt)->core_option_get(key);
}
rp_result rp_runtime_core_option_set(rp_runtime* rt, const char* key, const char* value) {
    if (!rt || !key || !value) return RP_ERR_BAD_ARG;
    return reinterpret_cast<Runtime*>(rt)->core_option_set(key, value);
}
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `cmake --build build --target retropark_tests --config Release` then `./build/tests/Release/retropark_tests.exe --test-case="core options: no-options*"`
Expected: PASS (json "[]", get NULL, set RP_ERR_UNSUPPORTED).

- [ ] **Step 8: Verify all existing cores still compile at v9 and the suite is green**

Run: `cmake --build build --config Release` (rebuilds every core against the v9 header) then `RP_RUN_N64=1 ./build/tests/Release/retropark_tests.exe`
Expected: full build succeeds; all cases pass (existing cores now report abi_version 9 with NULL option fptrs). If any core's `kAbi` used designated initializers or a literal `8`, fix it to the `RETROPARK_ABI_VERSION` macro / positional form.

- [ ] **Step 9: Commit**

```bash
git add include/retropark/retropark_abi.h include/retropark/retropark.h src/loader/CoreLoader.h src/loader/CoreLoader.cpp src/runtime/Runtime.h src/runtime/Runtime.cpp tests/test_core_options.cpp tests/CMakeLists.txt
git commit -m "feat: ABI v9 core-options channel (json + get/set), runtime + loader plumbing"
```

### Task A2: Shim harvests full option definitions + serves core_options_json (reports options v2)

**Files:**
- Modify: `cores/libretro_shim/LibretroShim.cpp`
- Test: `tests/test_core_options.cpp` (extend)

**Interfaces:**
- Consumes: the v9 `rp_core_abi` slots + `rp_runtime_core_options_json` (Task A1).
- Produces: a populated `core_options_json` for a shim core; `core_option_get`/`set` still to come in A3 (their fptrs stay NULL until A3).

- [ ] **Step 1: Write the failing test** (gated, like the N64 e2e — uses the real fceumm/N64 shim core)

Extend `tests/test_core_options.cpp`:

```cpp
#include <string>
// Gate exactly like the N64 e2e: only runs when RP_RUN_N64 is set; uses the same shim core dir + ROM.
TEST_CASE("core options: shim harvests real core options as JSON") {
    if (!std::getenv("RP_RUN_N64")) { WARN("RP_RUN_N64 not set; skipping shim core-options harvest"); return; }
    rp_runtime* rt = rp_runtime_create(RP_GFX_NONE, nullptr);
    REQUIRE(rt != nullptr);
    REQUIRE(rp_runtime_load_core(rt, RP_N64_SHIM_DIR) == RP_OK);  // build/cores/libretro_shim_n64

    std::string json = rp_runtime_core_options_json(rt);
    CHECK(json.size() > 2);                 // not "[]"
    CHECK(json.front() == '[');
    CHECK(json.find("\"key\"") != std::string::npos);
    CHECK(json.find("mupen64plus") != std::string::npos);   // mupen option keys are prefixed mupen64plus-*
    rp_runtime_destroy(rt);
}
```

`RP_N64_SHIM_DIR` = `build/cores/libretro_shim_n64` via a compile define in `tests/CMakeLists.txt` (the N64 e2e already references this dir as its `RP_N64_CORE_DIR` default — reuse that exact literal).

- [ ] **Step 2: Run test to verify it fails**

Run: build, then `RP_RUN_N64=1 ./build/tests/Release/retropark_tests.exe --test-case="core options: shim harvests*"`
Expected: FAIL (json is `"[]"` — the shim doesn't harvest or export `core_options_json` yet).

- [ ] **Step 3: Add the option-definition model to the shim**

In `cores/libretro_shim/LibretroShim.cpp`, near the `Shim` struct, replace the defaults-only map with a full model (keep behavior for legacy cores):

```cpp
struct ShimOption {
    std::string key, desc, info, def;
    std::vector<std::pair<std::string,std::string>> values;  // (value, label); values[0] == default source
};
```

In `struct Shim`, replace `std::unordered_map<std::string,std::string> option_defaults;` with:

```cpp
    std::vector<ShimOption> option_defs;               // harvested, menu order
    std::unordered_map<std::string,size_t> option_index; // key -> index into option_defs
    std::string options_json_cache;                    // built lazily by core_options_json
    // (A3 adds: overrides map + dirty flag)
```

Add a helper to register one option (dedup by key):

```cpp
void shim_add_option(Shim* s, const std::string& key, const std::string& desc, const std::string& info,
                     std::vector<std::pair<std::string,std::string>> values, const std::string& def) {
    if (key.empty() || s->option_index.count(key)) return;
    s->option_index[key] = s->option_defs.size();
    s->option_defs.push_back({key, desc, info, def, std::move(values)});
}
```

- [ ] **Step 4: Harvest from every options API variant + report version 2**

In `env_cb`:
- `GET_CORE_OPTIONS_VERSION` → set `*data = 2` (was 0).
- `SET_VARIABLES` (legacy): for each `retro_variable`, split `value` on `';'` → `desc`; split the remainder on `'|'` → values (label == value); `def` = values[0]; call `shim_add_option`.
- `SET_CORE_OPTIONS` / `_INTL`: iterate `retro_core_option_definition*` (the `_INTL` form wraps `.us`); each has `key, desc, info, values[ {value,label} ]` (label may be NULL → use value), `default_value`; call `shim_add_option`.
- `SET_CORE_OPTIONS_V2` / `_V2_INTL`: iterate `retro_core_option_v2_definition*` from the struct's `.definitions` (the `_INTL` form wraps `.us`); same fields plus category (ignored). Call `shim_add_option`.

Mirror the field-walking in EB's `LibretroCore::registerVariablesLegacy` / `registerOptionDefs` / `registerOptionDefsV2` (native/src/libretro/LibretroCore.cpp) for exact loop-termination (`key != nullptr`) and label-fallback rules. `GET_VARIABLE` keeps returning the default for now (read `def` from `option_defs` via `option_index` instead of the old map).

- [ ] **Step 5: Implement core_options_json (hand-rolled escaped JSON) + export the fptr**

Add a JSON string-escaper and builder (no JSON lib in the shim):

```cpp
static void json_escape(const std::string& in, std::string& out) {
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;  case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;  case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b,sizeof b,"\\u%04x",c); out += b; }
                     else out += c;
        }
    }
}
const char* sh_core_options_json(rp_core* core) {
    Shim* s = reinterpret_cast<Shim*>(core);
    std::string& j = s->options_json_cache;
    j = "[";
    for (size_t i = 0; i < s->option_defs.size(); ++i) {
        const ShimOption& o = s->option_defs[i];
        if (i) j += ",";
        std::string k,d,inf,def; json_escape(o.key,k); json_escape(o.desc,d); json_escape(o.info,inf); json_escape(o.def,def);
        j += "{\"key\":\""+k+"\",\"desc\":\""+d+"\",\"info\":\""+inf+"\",\"default\":\""+def+"\",\"values\":[";
        for (size_t v = 0; v < o.values.size(); ++v) {
            if (v) j += ",";
            std::string vv,vl; json_escape(o.values[v].first,vv); json_escape(o.values[v].second,vl);
            j += "{\"value\":\""+vv+"\",\"label\":\""+vl+"\"}";
        }
        j += "]}";
    }
    j += "]";
    return j.c_str();
}
```

In the `kAbi` initializer, append `sh_core_options_json` (and, as A3 fills them, `sh_core_option_get`, `sh_core_option_set`). For A2, append `sh_core_options_json, nullptr, nullptr` so the struct matches the v9 layout.

- [ ] **Step 6: Run the test to verify it passes**

Run: build, then `RP_RUN_N64=1 ./build/tests/Release/retropark_tests.exe --test-case="core options: shim harvests*"`
Expected: PASS (JSON contains `"key"` and a `mupen64plus` key).

- [ ] **Step 7: Commit**

```bash
git add cores/libretro_shim/LibretroShim.cpp tests/test_core_options.cpp
git commit -m "feat: libretro shim harvests full core-option definitions + serves core_options_json (options v2)"
```

### Task A3: Shim serves overrides — GET_VARIABLE override, live update, core_option_get/set

**Files:**
- Modify: `cores/libretro_shim/LibretroShim.cpp`
- Test: `tests/test_core_options.cpp` (extend)

**Interfaces:**
- Consumes: `option_defs`/`option_index` (A2), the v9 ABI slots (A1).
- Produces: functional `core_option_get`/`core_option_set` fptrs; live GET_VARIABLE override behavior.

- [ ] **Step 1: Write the failing test** (gated)

```cpp
TEST_CASE("core options: set override is echoed by get and flagged to the core") {
    if (!std::getenv("RP_RUN_N64")) { WARN("RP_RUN_N64 not set; skipping shim core-options override"); return; }
    rp_runtime* rt = rp_runtime_create(RP_GFX_NONE, nullptr);
    REQUIRE(rt != nullptr);
    REQUIRE(rp_runtime_load_core(rt, RP_N64_SHIM_DIR) == RP_OK);

    // Pick the first option key + a non-default value from the JSON (parse minimally in-test),
    // or use a known mupen key. Use a known-stable mupen toggle:
    const char* key = "mupen64plus-43screensize";
    const char* before = rp_runtime_core_option_get(rt, key);
    REQUIRE(before != nullptr);
    const char* newv = std::strcmp(before, "320x240") == 0 ? "640x480" : "320x240";
    CHECK(rp_runtime_core_option_set(rt, key, newv) == RP_OK);
    CHECK(std::strcmp(rp_runtime_core_option_get(rt, key), newv) == 0);
    CHECK(rp_runtime_core_option_set(rt, "no-such-key", "x") == RP_ERR_NOT_FOUND);
    rp_runtime_destroy(rt);
}
```

(If `mupen64plus-43screensize` is not present in this core build, the implementer substitutes the first key from `rp_runtime_core_options_json` — parse the first `"key":"…"` — and picks any `values[]` entry different from `default`. Do not hardcode a key that the harvest step didn't produce.)

- [ ] **Step 2: Run test to verify it fails**

Run: build, then `RP_RUN_N64=1 ./build/tests/Release/retropark_tests.exe --test-case="core options: set override*"`
Expected: FAIL (`core_option_get`/`set` fptrs are NULL → runtime returns NULL / RP_ERR_UNSUPPORTED).

- [ ] **Step 3: Add the overrides map + dirty flag**

In `struct Shim` add:

```cpp
    std::unordered_map<std::string,std::string> option_overrides;  // key -> user value
    bool options_dirty = false;                                    // GET_VARIABLE_UPDATE latch
```

- [ ] **Step 4: Serve overrides on GET_VARIABLE + live update**

- `GET_VARIABLE`: look up `option_index[key]`; if found, return `option_overrides[key]` when present else `option_defs[idx].def` (c_str from a stable string — store the served value in a per-Shim scratch string to guarantee lifetime). Unknown key → `value=nullptr`, return false (unchanged).
- `GET_VARIABLE_UPDATE`: `*data = s->options_dirty; s->options_dirty = false; return true;`

- [ ] **Step 5: Implement core_option_get/set + export the fptrs**

```cpp
const char* sh_core_option_get(rp_core* core, const char* key) {
    Shim* s = reinterpret_cast<Shim*>(core);
    auto it = s->option_index.find(key ? key : "");
    if (it == s->option_index.end()) return nullptr;
    auto ov = s->option_overrides.find(key);
    static thread_local std::string ret;   // stable lifetime for the returned c_str
    ret = (ov != s->option_overrides.end()) ? ov->second : s->option_defs[it->second].def;
    return ret.c_str();
}
rp_result sh_core_option_set(rp_core* core, const char* key, const char* value) {
    Shim* s = reinterpret_cast<Shim*>(core);
    if (!key || !value) return RP_ERR_BAD_ARG;
    if (!s->option_index.count(key)) return RP_ERR_NOT_FOUND;
    s->option_overrides[key] = value;
    s->options_dirty = true;
    return RP_OK;
}
```

Replace the `nullptr, nullptr` placeholders in `kAbi` with `sh_core_option_get, sh_core_option_set`.

- [ ] **Step 6: Run the test to verify it passes**

Run: build, then `RP_RUN_N64=1 ./build/tests/Release/retropark_tests.exe --test-case="core options: set override*"`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add cores/libretro_shim/LibretroShim.cpp tests/test_core_options.cpp
git commit -m "feat: libretro shim serves core-option overrides (GET_VARIABLE + live update + get/set)"
```

### Task A4: Phase-A verification + merge to main + push

**Files:** none (verification + release)

- [ ] **Step 1: Full suite green (including gated N64 + the existing zero-copy proof)**

Run: `cmake --build build --config Release` then `RP_RUN_N64=1 ./build/tests/Release/retropark_tests.exe`
Expected: all cases pass; the N64 zero-copy e2e still reports `gl_frame_count>0` (options changes must not regress rendering).

- [ ] **Step 2: Merge + push** (RetroPark standing rule; no AI attribution)

```bash
git checkout main && git pull --ff-only
git merge --no-ff <working-branch> -m "feat: RetroPark core-options ABI v9 + libretro-shim options"
git push origin main
git rev-parse HEAD    # record this SHA for the Phase B submodule bump
```

(If work was done directly on `main`, just verify + `git push origin main` and record the SHA.)

---

## Phase B — EverythingBox integration (submodule bump + harvest + UI + deploy)

**Repo:** `C:\Users\cubma\Project Goliath` (EverythingBox). **Build in a throwaway worktree** off current `origin/main` (e.g. `C:/Users/cubma/goliath-wt-coreopts`) — the shared tree must not be built/committed in. Sources under `native/src`.

**Persistence-key refinement (supersedes spec §5's "parse core.json"):** key options by the system's core name via `Settings::coreFor(systemId)` (fallback `sys.cores.value(0)`) — the *exact* derivation the native editor (`editCoreOptions`) already uses. This guarantees a RetroPark-set option and a native-set option share the same `opt/<core>/*` key with no new core.json parser. `RetroParkView::openGame` already receives `systemId`.

**Reused EB APIs (from extraction; file:line):**
- `struct CoreOption` — `native/src/libretro/LibretroCore.h:14`.
- `Settings::optionValue(core,key)` / `setOptionValue(core,key,value)` — `native/src/core/Settings.h:405`; per-game `gameOptionValue/setGameOptionValue/clearGameOptionValue/gameOptionDelta/gameHasOption` — `Settings.h:437`; `gameToken(identity)` — `Settings.h:419`.
- `editCoreOptions(systemId)` themed editor — `native/src/ui/MainWindow.cpp:16088`; picker `presentEmulatorCorePicker()` — `:16031`.
- In-game options pattern to mirror: `RetroView::showCoreOptions()` — `native/src/emu/RetroView.cpp:404-526` (widget menu: `QScrollArea` of `QPushButton` rows; scope toggle `coreOptGameScope_`; `baselineOf`; live `core_.setOptionValue` + scoped persist).
- `RetroParkView` — `native/src/emu/RetroParkView.h/.cpp`: runtime `rp_runtime* rt_` (`.h:95`), `rp_runtime_load_core(rt_, coreDir)` (`.cpp:235,282`), `rp_runtime_load_content(rt_, rom)` (`.cpp:242,288`), pause menu `buildMenu()` (`.cpp:106-157`, `menuButtons_` focus-cycle), shim dir = `CoreManager::coresDir()+"/libretro_shim"` (`.cpp:252`).

### Task B1: Bump submodule to v9, rebuild EB + Dolphin vehicle, verify launch

**Files:**
- Modify: `external/RetroPark` (submodule pointer)
- Build only: EB app + v9 Dolphin vehicle DLL

- [ ] **Step 1: Create the worktree + bump the submodule**

```bash
cd "C:/Users/cubma/Project Goliath"
git worktree add C:/Users/cubma/goliath-wt-coreopts origin/main
cd C:/Users/cubma/goliath-wt-coreopts
git -C external/RetroPark fetch origin && git -C external/RetroPark checkout <PHASE-A-SHA>
git add external/RetroPark
git commit -m "chore: bump RetroPark submodule to v9 (core-options ABI)"
```

- [ ] **Step 2: Force the ExternalProject to rebuild + rebuild the v9 Dolphin vehicle**

```bash
rm -rf build/retropark_ext-prefix
# Rebuild the git-ignored Dolphin vehicle against the v9 header (MSBuild RetroParkDolphin.vcxproj);
# EB_DOLPHIN_VEHICLE_DIR = C:/Users/cubma/source/repos/RetroPark/external/dolphin
```
Expected: `retropark.lib` + `LibretroShim` + the Dolphin vehicle all compile at abi_version 9.

- [ ] **Step 3: Build EB (Release) + verify the app launches with no ABI mismatch**

Run the worktree Release build (per `mmv-worktree-build-config`), launch the built exe with a driven RetroPark core, confirm the loader reports no `RP_ERR_ABI_MISMATCH` (v9 exe + v9 vehicle + v9 shim). N64/NES render as before.

- [ ] **Step 4: Commit** (submodule bump already committed in Step 1; no code change here)

### Task B2: JSON→CoreOption parser + headless runtime harvest helper

**Files:**
- Create: `native/src/emu/RetroParkOptions.h`, `native/src/emu/RetroParkOptions.cpp`
- Test: follow the repo's existing unit-test/probe pattern (e.g. a `probe_*` under `native/` like `probe_nav`); if a unit-test target exists, add there.
- Modify: the CMake target that lists `native/src/emu/*.cpp` (add the new .cpp)

**Interfaces:**
- Produces:
  ```cpp
  namespace RetroParkOptions {
    // Parse the rp_runtime_core_options_json array into CoreOption structs (QJsonDocument).
    std::vector<CoreOption> parse(const QByteArray& json);
    // Harvest options for a shim core dir WITHOUT launching a game: headless RP_GFX_NONE runtime,
    // load_core(coreDir), rp_runtime_core_options_json, parse, destroy. Empty vector on failure.
    std::vector<CoreOption> harvest(const QString& coreDir);
  }
  ```

- [ ] **Step 1: Write the failing test** (pure `parse`, harness-independent logic)

```cpp
// Given the ABI's JSON shape, parse yields the CoreOption fields in order.
const QByteArray j = R"([{"key":"fceumm_palette","desc":"Palette","info":"",
  "default":"default","values":[{"value":"default","label":"Default"},{"value":"rgb","label":"RGB"}]}])";
std::vector<CoreOption> v = RetroParkOptions::parse(j);
assert(v.size()==1);
assert(v[0].key=="fceumm_palette" && v[0].desc=="Palette");
assert(v[0].defaultValue=="default");
assert(v[0].values.size()==2 && v[0].values[0].first=="default" && v[0].values[0].second=="Default");
assert(RetroParkOptions::parse("[]").empty());
```

- [ ] **Step 2: Run test to verify it fails** — build the test/probe; expect FAIL (symbol missing).

- [ ] **Step 3: Implement parse (QJsonDocument) + harvest (headless runtime)**

```cpp
#include "RetroParkOptions.h"
#include "libretro/LibretroCore.h"           // CoreOption
#include <retropark/retropark.h>
#include <QJsonDocument> #include <QJsonArray> #include <QJsonObject>

std::vector<CoreOption> RetroParkOptions::parse(const QByteArray& json) {
    std::vector<CoreOption> out;
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    for (const auto& e : arr) {
        const QJsonObject o = e.toObject(); CoreOption co;
        co.key = o["key"].toString().toStdString();
        co.desc = o["desc"].toString().toStdString();
        co.info = o["info"].toString().toStdString();
        co.defaultValue = o["default"].toString().toStdString();
        for (const auto& ve : o["values"].toArray()) {
            const QJsonObject vo = ve.toObject();
            co.values.emplace_back(vo["value"].toString().toStdString(), vo["label"].toString().toStdString());
        }
        out.push_back(std::move(co));
    }
    return out;
}
std::vector<CoreOption> RetroParkOptions::harvest(const QString& coreDir) {
    rp_runtime* rt = rp_runtime_create(RP_GFX_NONE, nullptr);
    if (!rt) return {};
    std::vector<CoreOption> out;
    if (rp_runtime_load_core(rt, coreDir.toUtf8().constData()) == RP_OK) {
        const char* j = rp_runtime_core_options_json(rt);
        out = parse(QByteArray(j ? j : "[]"));
    }
    rp_runtime_destroy(rt);
    return out;
}
```
(Verify `rp_runtime_load_core` reaches the core's Created state so options are registered — the harvest test asserting a non-empty set for the N64 shim dir is the check. If load_core defers create, add the minimal call that reaches Created before querying.)

- [ ] **Step 4: Run test to verify it passes.** Expected: PASS.

- [ ] **Step 5: Commit**
```bash
git add native/src/emu/RetroParkOptions.h native/src/emu/RetroParkOptions.cpp <cmake> <test>
git commit -m "feat: RetroPark core-options JSON parser + headless harvest helper"
```

### Task B3: Global editor harvests RetroPark-backed systems via the runtime

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (`editCoreOptions`, ~:16088)

**Interfaces:**
- Consumes: `RetroParkOptions::harvest` (B2), the shim dir resolver.

- [ ] **Step 1: Branch the harvest source in editCoreOptions**

In `editCoreOptions(systemId)`, where it currently does `LibretroCore tmp; tmp.loadCore(corePath); opts = tmp.options();`, branch: when the system is RetroPark-backed (use the existing backend predicate — `EmuBackend`/`Settings` backend-for-system, and `EmuBackend::retroParkSupportsSystem`), harvest instead via
`RetroParkOptions::harvest(CoreManager::coresDir()+"/"+<shim-subdir-for-system>)`. The persistence `core` name stays `Settings::coreFor(sys.id)` (fallback `sys.cores.value(0)`) exactly as today — so keys match the native backend. UI + `setOptionValue` persistence path unchanged.

- [ ] **Step 2: Manual/UITEST verify** the NES (driven RetroPark) system shows its harvested option rows in Emulator Settings → NES → Options…, and a change writes `opt/<core>/<key>`.

- [ ] **Step 3: Commit**
```bash
git add native/src/ui/MainWindow.cpp
git commit -m "feat: global core-options editor harvests RetroPark-backed systems via the runtime"
```

### Task B4: Apply persisted options at RetroPark launch

**Files:**
- Modify: `native/src/emu/RetroParkView.h`, `native/src/emu/RetroParkView.cpp`

**Interfaces:**
- Consumes: `Settings::optionValue`/`gameOptionDelta`, `rp_runtime_core_option_set`, `RetroParkOptions::harvest`.
- Produces: new members `QString coreName_`, `QString overrideToken_`.

- [ ] **Step 1: Set coreName_ + overrideToken_ in openGame**

In `RetroParkView::openGame(...)`, after `systemId_`/`gameKey_` are set: `coreName_ = Settings::coreFor(systemId_); if (coreName_.isEmpty()) coreName_ = <sys.cores[0] for systemId_>;` and `overrideToken_ = Settings::gameToken(<game identity>)` (mirror how RetroView derives `overrideToken_`).

- [ ] **Step 2: Push effective values before load_content**

Immediately before `rp_runtime_load_content(rt_, rom)`, compute the effective value set and push each:
```cpp
const auto opts = RetroParkOptions::harvest(coreDir);              // the shim's own option list
const QMap<QString,QString> delta = overrideToken_.isEmpty()
    ? QMap<QString,QString>() : Settings::gameOptionDelta(overrideToken_, coreName_);
for (const CoreOption& o : opts) {
    const QString key = QString::fromStdString(o.key);
    QString val = Settings::optionValue(coreName_, key);           // per-core baseline
    if (val.isEmpty()) val = QString::fromStdString(o.defaultValue);
    if (delta.contains(key)) val = delta.value(key);               // per-game override wins
    if (val != QString::fromStdString(o.defaultValue) || Settings::optionValue(coreName_,key).size() || delta.contains(key))
        rp_runtime_core_option_set(rt_, o.key.c_str(), val.toUtf8().constData());
}
```
(Setting every non-default effective value before content load means the core reads them at `retro_load_game`.)

- [ ] **Step 3: Verify** a persisted option (set in B3's global editor) visibly takes effect when the game launches via RetroPark (e.g. an NES palette/aspect option). Manual/UITEST.

- [ ] **Step 4: Commit**
```bash
git add native/src/emu/RetroParkView.h native/src/emu/RetroParkView.cpp
git commit -m "feat: RetroParkView applies persisted core options at launch (per-core + per-game)"
```

### Task B5: In-game pause-menu "Core Options" page in RetroParkView

**Files:**
- Modify: `native/src/emu/RetroParkView.h`, `native/src/emu/RetroParkView.cpp`

**Interfaces:**
- Consumes: B2 harvest (of the running `rt_`), `rp_runtime_core_option_set`, the scoped-persist logic mirrored from `RetroView::showCoreOptions`.

- [ ] **Step 1: Add a "Core Options" entry to buildMenu**

In `buildMenu()` add a `coreOptsBtn_` to `menuButtons_` (only when the running core has options — `rp_runtime_core_options_json(rt_) != "[]"`). Add members `coreOptsBtn_`, `bool coreOptGameScope_`.

- [ ] **Step 2: Build the options sub-page (mirror RetroView::showCoreOptions)**

A widget sub-menu (same `QFrame`/`QPushButton` shape as `buildMenu`) listing each harvested option with ≥2 values; a "Scope: This game / This core" toggle active only when `overrideToken_` non-empty; `baselineOf(o)` = `Settings::optionValue(coreName_,key)` else `defaultValue` else `values.front()`. On a row activate:
```cpp
core option: cycle to next value `next`;
rp_runtime_core_option_set(rt_, key.c_str(), next.c_str());        // LIVE (shim GET_VARIABLE_UPDATE)
if (gameScope) { if (next == baseline) Settings::clearGameOptionValue(overrideToken_, coreName_, qkey);
                 else Settings::setGameOptionValue(overrideToken_, coreName_, qkey, next); }
else Settings::setOptionValue(coreName_, qkey, next);
```
Row label appends "• modified for this game" when `Settings::gameHasOption(overrideToken_, coreName_, key)`.

- [ ] **Step 3: Verify via EB_UITEST** — open a RetroPark NES game, pause, Core Options, change a value live, confirm it applies and persists (per-core and per-game scope).

- [ ] **Step 4: Commit**
```bash
git add native/src/emu/RetroParkView.h native/src/emu/RetroParkView.cpp
git commit -m "feat: RetroParkView in-game Core Options pause page (live apply + per-game scope)"
```

### Task B6: Deploy + verify + memory

**Files:** none (release)

- [ ] **Step 1: Full worktree build green** (Release) + any EB probe gates (`probe_nav` etc.) pass.

- [ ] **Step 2: Merge the worktree branch to EB main + push** (no AI attribution). Remove the worktree.

- [ ] **Step 3: Targeted deploy to `C:\EverythingBox-app`** — copy ONLY: the Release `EverythingBox.exe`, the v9 `dolphin_present.dll` + its `core.json`, the v9 `libretro_shim*` dir(s) (`LibretroShim.dll` + `core.json`; cores stay as-is), and any changed EB DLLs. NEVER `robocopy /MIR` (would delete downloaded cores/savestates/settings).

- [ ] **Step 4: Health-check** the deployed app launches (no ABI mismatch), NES via RetroPark shows Core Options in-game, a changed option persists across a relaunch.

- [ ] **Step 5: Update memory** — `retropark-project.md` (ABI v9 + shim options) and `retropark-eb-integration.md` (core-options parity shipped + deployed).

---

## Self-review notes

- **Spec coverage:** ABI channel (A1) ✓; shim harvest+serve (A2/A3) ✓; EB harvest via runtime (B2/B3) ✓; per-game overrides via OverrideLayer/Settings (B4/B5) ✓; in-game + global + per-game surfaces (B3/B5) ✓; deploy (B6) ✓; testing (A1/A2/A3 gated + B2 parser) ✓.
- **Deviation from spec §5 (flagged):** persistence key derived via `Settings::coreFor(sys.id)` (same as native editor) rather than parsing `core.json`'s `libretro_core` — identical outcome (shared key on the underlying core name), less code, no new parser.
- **Type consistency:** `CoreOption` fields (`key/desc/info/values/defaultValue`) used identically in the shim JSON, B2 parser, and B3/B4/B5. Runtime C API names match between Phase A declaration and Phase B use (`rp_runtime_core_options_json/_get/_set`).
