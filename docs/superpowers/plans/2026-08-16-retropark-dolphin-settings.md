# Dolphin/GC graphics settings via core-options — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development (or executing-plans). Steps use `- [ ]` checkboxes.

**Goal:** The RetroPark Dolphin presenting vehicle exposes internal-resolution + aspect-ratio as core options (v9 channel it currently stubs NULL); EverythingBox surfaces them for GameCube through the same in-game + global core-options UI already shipped for NES, and stops writing the standalone `GFX.ini` the in-process Dolphin ignores.

**Architecture:** No ABI/submodule/runtime change (v9 already carries the channel; EB already links `rp_runtime_core_options_*`). Phase A edits the git-ignored Dolphin vehicle (`rp_dolphin.cpp`) + rebuilds the DLL + updates the committed patch. Phase B extends EB's presenting branch + hides the quartet + redeploys the rebuilt vehicle.

**Design doc:** `docs/superpowers/specs/2026-08-16-retropark-dolphin-settings-design.md`

## Global Constraints

- **No ABI bump, no submodule bump.** The v9 `rp_core_abi` core-options slots and the `rp_runtime_core_options_*` C API already exist and ship. Only the vehicle's `kAbi` gains its (previously NULL) trailing 3 fptrs; ABI stays v9.
- **The Dolphin vehicle is git-ignored** (`external/dolphin/**` is `.gitignore`d). `rp_dolphin.cpp` edits are LOCAL; the committed record is `docs/patches/dolphin-external-present.patch` — regenerate it after the change. Reviewers diff `rp_dolphin.cpp` via a pre-edit backup + `git diff --no-index`, not `git diff`.
- **Vehicle rebuild recipe** (PowerShell; `-` switches; Git Bash mangles `/` switches): `MSBuild RetroParkDolphin.vcxproj -p:Configuration=Release -p:Platform=x64 -p:SolutionDir="C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\\" -p:BuildProjectReferences=false -m -v:minimal -nologo` — recompiles only `rp_dolphin.cpp` + relinks (avoids the glslang VS-cache mismatch); AfterBuild copies the DLL to `external/dolphin/Binary/x64/`.
- **Config knobs:** internal-res → `Config::GFX_EFB_SCALE` (int: 1=native, N=N×); aspect → `Config::GFX_ASPECT_RATIO` (`AspectMode`). **Verify the `AspectMode` ordinal values in `external/dolphin/Source/Core/VideoCommon/VideoConfig.h` — do not assume the ordering.**
- **EverythingBox tree is shared** — Phase B builds/commits in a throwaway worktree off `origin/main`; targeted deploy (exe + rebuilt `dolphin_present.dll`), never `robocopy /MIR`.
- No AI attribution anywhere. Cores/ROMs never committed.

---

## Phase A — Dolphin vehicle exposes internal-res + aspect as core options

### Task A1: Vehicle implements core_options_json / get / set + Config apply

**Files:**
- Modify (git-ignored, local): `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp`
- Regenerate (committed): `docs/patches/dolphin-external-present.patch`

**Interfaces (produced, matching the v9 `rp_core_abi` trailing slots):**
- `const char* dp_core_options_json(rp_core*)`, `const char* dp_core_option_get(rp_core*, const char*)`, `rp_result dp_core_option_set(rp_core*, const char*, const char*)`.

- [ ] **Step 1: Add option state + a boot-time apply hook**

Near the vehicle's other file-scope state, add a small override store and a helper that pushes the effective values into Dolphin's Config (used both at boot and on a live set):
```cpp
// Effective option values (string), keyed by option id. Empty => Dolphin default.
static std::unordered_map<std::string,std::string> g_dp_options;
static std::mutex g_dp_options_mtex;
static void ApplyDolphinOptionToConfig(const std::string& key, const std::string& val) {
    if (key == "dolphin_internal_resolution") {
        int scale = std::max(1, atoi(val.c_str()));
        Config::SetBaseOrCurrent(Config::GFX_EFB_SCALE, scale);
    } else if (key == "dolphin_aspect_ratio") {
        // AspectMode ordinal — VERIFY against VideoConfig.h in this tree before trusting the cast.
        Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, static_cast<AspectMode>(atoi(val.c_str())));
    }
}
```
In the boot config block (the existing `Config::SetBaseOrCurrent(...)` sequence in `HostThread`, ~lines 218-274), after the fixed settings, apply any pre-boot overrides:
```cpp
{ std::lock_guard<std::mutex> lk(g_dp_options_mtex);
  for (auto& [k,v] : g_dp_options) ApplyDolphinOptionToConfig(k, v); }
```

- [ ] **Step 2: Implement the three ABI functions**

```cpp
const char* dp_core_options_json(rp_core*) {
    // Static descriptor; internal-res + aspect only (renderer locked Vulkan, vsync host-owned).
    static const char* kJson =
      "[{\"key\":\"dolphin_internal_resolution\",\"desc\":\"Internal Resolution\",\"info\":\"\",\"default\":\"1\","
        "\"values\":[{\"value\":\"1\",\"label\":\"Native (640x480)\"},{\"value\":\"2\",\"label\":\"2x (1280x960)\"},"
        "{\"value\":\"3\",\"label\":\"3x\"},{\"value\":\"4\",\"label\":\"4x\"},{\"value\":\"5\",\"label\":\"5x\"},"
        "{\"value\":\"6\",\"label\":\"6x\"}]},"
       "{\"key\":\"dolphin_aspect_ratio\",\"desc\":\"Aspect Ratio\",\"info\":\"\",\"default\":\"0\","
        "\"values\":[{\"value\":\"0\",\"label\":\"Auto\"},{\"value\":\"1\",\"label\":\"Force 16:9\"},"
        "{\"value\":\"2\",\"label\":\"Force 4:3\"},{\"value\":\"3\",\"label\":\"Stretch\"}]}]";
    return kJson;
}
const char* dp_core_option_get(rp_core*, const char* key) {
    if (!key) return nullptr;
    std::lock_guard<std::mutex> lk(g_dp_options_mtex);
    auto it = g_dp_options.find(key);
    if (it != g_dp_options.end()) { static thread_local std::string r; r = it->second; return r.c_str(); }
    if (!strcmp(key,"dolphin_internal_resolution")) return "1";
    if (!strcmp(key,"dolphin_aspect_ratio")) return "0";
    return nullptr;   // unknown key
}
rp_result dp_core_option_set(rp_core*, const char* key, const char* value) {
    if (!key || !value) return RP_ERR_BAD_ARG;
    if (strcmp(key,"dolphin_internal_resolution") && strcmp(key,"dolphin_aspect_ratio")) return RP_ERR_NOT_FOUND;
    { std::lock_guard<std::mutex> lk(g_dp_options_mtex); g_dp_options[key] = value; }
    // Live apply on the CPU thread if the core is running (mirror dp_serialize's RunOnCPUThread guard);
    // otherwise the boot block will apply it. A running config change triggers Dolphin's video refresh.
    if (Core::IsRunning(Core::System::GetInstance())) {
        std::string k = key, v = value;
        Core::RunOnCPUThread(Core::System::GetInstance(), [k,v]{ ApplyDolphinOptionToConfig(k, v); }, /*wait*/true);
    }
    return RP_OK;
}
```
(Match the exact `Core::IsRunning` / `Core::RunOnCPUThread` signatures already used by `dp_serialize`/`dp_serialize_size` in this file.)

- [ ] **Step 3: Wire into kAbi**

Change the `kAbi` initializer (`rp_dolphin.cpp` ~:933) from ending at `dp_load_content` to:
```cpp
const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, dp_get_info, dp_create, dp_destroy, dp_set_surfaces,
    dp_start, dp_stop, dp_get_av_info, nullptr, dp_serialize_size,
    dp_serialize, dp_unserialize, dp_load_content,
    dp_core_options_json, dp_core_option_get, dp_core_option_set};
```

- [ ] **Step 4: Rebuild the vehicle**

Run the PowerShell MSBuild recipe (Global Constraints). Expect `dolphin_present.dll` rebuilt + copied to `external/dolphin/Binary/x64/`, still ABI v9.

- [ ] **Step 5: Gated test — options exposed + applied**

Extend the Dolphin e2e harness/test (the gated one that boots a GC ISO via `dolphin_present`; reuse its env gate + ISO path). Assert: `rp_runtime_core_options_json` contains `dolphin_internal_resolution` and `dolphin_aspect_ratio`; `rp_runtime_core_option_set(rt,"dolphin_internal_resolution","2")` returns `RP_OK` and `..._get` echoes `"2"`; after booting the ISO with internal-res 2 pre-set, Dolphin's `g_Config.iEFBScale` (or `VideoConfig`) reads 2. Unknown key → `RP_ERR_NOT_FOUND`. Run it; confirm green.

- [ ] **Step 6: Regenerate the committed patch + commit**

Regenerate `docs/patches/dolphin-external-present.patch` to include the new `rp_dolphin.cpp` changes (follow how the patch is currently produced — a `git diff`/`format-patch` of the vehicle tree against its base tag, or the existing generator script; match its format exactly). Then:
```bash
git add docs/patches/dolphin-external-present.patch
git commit -m "feat: Dolphin vehicle exposes internal-res + aspect as core options (GC settings)"
git push origin main
```

---

## Phase B — EverythingBox surfaces GC options + hides the quartet + deploy

**Repo:** `C:\Users\cubma\Project Goliath`; build in a throwaway worktree off `origin/main` (e.g. `C:/Users/cubma/goliath-wt-dolphinsettings`). Re-stage the rebuilt vehicle (EB stages `dolphin_present.dll` from `EB_DOLPHIN_VEHICLE_DIR` = `C:/Users/cubma/source/repos/RetroPark/external/dolphin`, so a fresh EB build picks up the Phase-A DLL).

### Task B1: Extend launch-apply + in-game Core Options to the presenting branch

**Files:** Modify `native/src/emu/RetroParkView.cpp` / `.h`.

The driven branch already: sets `coreName_`/`overrideToken_`, and after `load_content` harvests the running core, caches descriptors, and applies persisted effective values; and builds the in-game "Core Options" page. Extend the SAME logic to the presenting branch:
- [ ] **Step 1:** In `openGame`, set `coreName_`/`overrideToken_` on the PRESENTING path too (the GC core id = `Settings::coreFor(systemId_)` fallback `cores[0]`; mirror the driven derivation from the core-options work).
- [ ] **Step 2:** After the presenting `rp_runtime_load_content` succeeds (RetroParkView.cpp ~:224-254), run the SAME harvest→cache-descriptors→apply-effective-values block the driven branch uses (factor it into a shared helper if that avoids duplication). The in-game "Core Options" button already gates on `rp_runtime_core_options_json(rt_) != "[]"`, so it lights up automatically once the vehicle returns options.
- [ ] **Step 3:** Build (`cmake --build build --config Release --target EverythingBox --parallel`), relink clean. Report what compiled vs the deferred in-app UITEST.
- [ ] **Step 4:** Commit.

### Task B2: Global editor shows GC options; hide the gfx-quartet for RetroPark-GC

**Files:** Modify `native/src/ui/MainWindow.cpp` (`editCoreOptions`, `editLaunchOptions`), `native/src/ui/SettingsDialog.cpp`.

- [ ] **Step 1:** In `editLaunchOptions` (~:6324), when a GC game resolves to RetroPark-presenting (the `retroParkStandaloneDivert` / effective-backend decision used elsewhere), do NOT render the gfx-quartet rows (`appendEmuGfxRows`). Standalone-Dolphin GC keeps them.
- [ ] **Step 2:** In the global core-options editor (`editCoreOptions` + `SettingsDialog::editOptions`), let the RetroPark arm fire for GC (it is currently excluded because GC's `externalEmulator` is non-empty). For RetroPark-presenting GC, harvest via the runtime (Dolphin exposes options headlessly — no "launch once" row needed) keyed by the GC core id. Reuse the exact harvest→parse→row-build path from the driven core-options work.
- [ ] **Step 3:** Build clean. Commit.

### Task B3: Deploy + verify + memory

- [ ] **Step 1:** Full worktree build green; probe gates pass.
- [ ] **Step 2:** Merge the worktree branch to EB `origin/main` (no AI attribution). Bump the `external/RetroPark` submodule to the Phase-A commit (provenance for the vehicle patch), even though the ABI is unchanged.
- [ ] **Step 3:** Targeted deploy to `C:\EverythingBox-app`: the Release `EverythingBox.exe` (+pdb) and the rebuilt v9 `dolphin_present.dll` (from the worktree's staged `cores/dolphin_present/`). Downloaded cores/Sys/settings untouched; never `/MIR`.
- [ ] **Step 4:** Health-check: deployed app boots clean; (hands-on, user) launch a GC game via RetroPark → pause → Core Options → change Internal Resolution → confirm it applies and persists.
- [ ] **Step 5:** Update memory (`retropark-project.md` Dolphin core-options; `retropark-eb-integration.md` GC-settings shipped + deployed).

---

## Self-review notes
- **Spec coverage:** vehicle options (A1) ✓; Config apply boot+live (A1) ✓; in-game + launch-apply presenting (B1) ✓; global editor + quartet-hide (B2) ✓; deploy (B3) ✓; testing (A1 gated) ✓.
- **No ABI/submodule dependency:** Phase B needs the rebuilt vehicle DLL (staged), not new committed RetroPark symbols; the submodule bump in B3 is provenance-only.
- **Reuse:** the EB harvest/parse/apply/in-game-page code all exists from the core-options feature — B1/B2 EXTEND its gating to presenting, not reimplement.
