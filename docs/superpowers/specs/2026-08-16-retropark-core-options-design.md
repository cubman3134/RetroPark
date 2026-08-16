# RetroPark core-options parity (libretro-shim cores) — Design

**Date:** 2026-08-16
**Status:** Approved (pre-plan)

## Goal

RetroPark-backed **libretro-shim** cores (NES/fceumm, N64/mupen, and any future
libretro-wrapping shim) get the same user-editable **core options** that
EverythingBox's native libretro backend already exposes: a global per-core
editor, an in-game pause-menu editor, and per-game overrides. The shim owns
option *harvesting* behind the RetroPark ABI; EverythingBox reuses its existing
options UI + persistence unchanged.

Out of scope (deferred): presenting cores (Dolphin/RPCS3) — they have no
libretro options channel; and the pre-existing GameCube gfx-"quartet" misdirect
bug (standalone Dolphin INI edits that never reach the in-process core).

## Why this gap exists (context)

Three EB emulation backends, asymmetric settings support:

- **Standalone emulators** read their own on-disk INI; EB writes documented keys
  before launch. Settings are cheap (INI authoring).
- **Native libretro cores** publish tunables via the libretro core-options C ABI
  (`SET_VARIABLES` / `SET_CORE_OPTIONS` / `_V2` / `_INTL`). EB's `LibretroCore`
  harvests them into `CoreOption` structs and already has a rich three-surface
  UI (global editor, classic dialog, in-game pause menu with per-core/per-game
  override scope — issue #95), persisted via `Settings` + `OverrideLayer`.
- **RetroPark** is the newer in-process runtime; `RetroParkView` is run-only. The
  `rp_core_abi` has no options mechanism, and the libretro shim intercepts the
  option-negotiation callbacks but keeps only each option's *default* (reports
  core-options version 0, serves the default verbatim on `GET_VARIABLE`, offers
  no override path).

RetroPark's NES/N64 cores *are* libretro cores (fceumm/mupen) under the shim, so
they emit full option sets that the shim currently discards. The asymmetry is
not fundamental — it's an unbuilt config channel.

## Architecture & data flow

The shim is already the libretro frontend for its wrapped core and already
intercepts option negotiation. It becomes the owner of option **harvesting** and
**serving**. The RetroPark ABI carries options as a JSON descriptor + get/set.
EB reuses its entire existing options UI + persistence.

```
core (fceumm/mupen) --SET_VARIABLES / SET_CORE_OPTIONS_V2--> shim harvest (defs)
                     <--GET_VARIABLE-- shim serves (override ?? default)
shim   --core_options_json / core_option_get / core_option_set--> rp_core_abi (v9)
runtime --rp_runtime_core_options_json / _get / _set--> EverythingBox
EB: parse JSON -> CoreOption -> reuse editCoreOptions UI + RetroParkView pause page
    + OverrideLayer (per-game delta) + Settings (persist, keyed by underlying core name)
```

Option lifetime: a libretro core registers its options during
`retro_set_environment` / `retro_init` (at core create, before content), so the
JSON descriptor is harvestable headlessly without a game — the same property EB's
`editCoreOptions()` relies on today. Overrides are applied by `core_option_set`
before `load_content` (read at `retro_load_game`) and live during a session (the
core re-reads on `GET_VARIABLE_UPDATE`).

## Component 1 — ABI additions (v8 → v9)

### `rp_core_abi` (include/retropark/retropark_abi.h)

Three trailing function pointers appended to the struct (a driven-core exposes
them; presenting cores set them to no-ops):

- `const char* (*core_options_json)(rp_core* core);`
  Returns a JSON array describing the core's options, or `"[]"` / `NULL` if the
  core has none. Shape:
  ```json
  [ { "key": "fceumm_sndvolume", "desc": "Sound Volume", "info": "...",
      "default": "100%",
      "values": [ {"value":"0%","label":"0%"}, {"value":"100%","label":"100%"} ] } ]
  ```
  Descriptor only (key/desc/info/default/values) — it carries no current value,
  so it is invariant after harvest. The returned pointer is owned by the core and
  stays valid until `destroy` (the shim caches the serialized string). Current
  values are read separately via `core_option_get`.

- `const char* (*core_option_get)(rp_core* core, const char* key);`
  Current effective value for `key` (override if set, else default). `NULL` if
  the key is unknown. Pointer valid until the next `core_option_set`/`destroy`.

- `rp_result (*core_option_set)(rp_core* core, const char* key, const char* value);`
  Sets an override and marks the option state dirty (so the core re-reads via
  `GET_VARIABLE_UPDATE`). `RP_ERR_NOT_FOUND` if `key` is not a declared option;
  `RP_OK` otherwise.

### `retropark.h` runtime wrappers

Forward to the loaded core; graceful when the core lacks the channel:

- `const char* rp_runtime_core_options_json(rp_runtime* rt);` — `"[]"` if no core
  loaded or the core exposes no options.
- `const char* rp_runtime_core_option_get(rp_runtime* rt, const char* key);` —
  `NULL` if unknown / no core.
- `rp_result rp_runtime_core_option_set(rp_runtime* rt, const char* key, const char* value);`
  — `RP_ERR_UNSUPPORTED` if the loaded core does not expose the channel (e.g. a
  presenting core), `RP_ERR_NOT_FOUND` for an unknown key.

### Version-bump obligations (known cost)

The loader gate is strict-equality on `abi_version`. Bumping to v9 requires:
- `RETROPARK_ABI_VERSION` → `9u`, comment updated.
- Every bundled core recompiled (its `abi_version` field must equal 9):
  refcore_driven/present/present_vk/rollback, the libretro shim, and the
  **Dolphin vehicle DLL** (rebuild `RetroParkDolphin.vcxproj`) — or Dolphin/gc
  fails the gate.
- Wipe `build/retropark_ext-prefix` so the EB ExternalProject actually rebuilds.
- Deploy ships the v9 exe + v9 `dolphin_present.dll` + shim.
- Existing cores that do not implement options leave the three pointers `NULL`;
  the runtime wrappers null-check before calling.

## Component 2 — Shim changes (cores/libretro_shim/LibretroShim.cpp)

- **Harvest full definitions** from all option APIs, mirroring EB's
  `LibretroCore` registration: legacy `SET_VARIABLES` (parse `desc; a|b|c`) plus
  `SET_CORE_OPTIONS`, `SET_CORE_OPTIONS_V2`, and their `_INTL` variants — storing
  `key`, `desc`, `info`, ordered `(value,label)` pairs, and default (values[0]).
- **Report core-options version 2** on `GET_CORE_OPTIONS_VERSION` (was 0), so
  cores emit the richer V2 descriptors. The shim still accepts legacy vars for
  cores that use them.
- **Split state**: `defs` (harvested, immutable) + `overrides` (key→value).
  - `GET_VARIABLE` returns `overrides[key]` if present, else the default.
  - `core_option_set` writes `overrides[key]` and sets a dirty flag. (The JSON
    descriptor is defs-only, so it is unaffected.)
  - `GET_VARIABLE_UPDATE` returns-and-clears dirty (today hardwired false, which
    is why native cores never saw live changes through the shim).
- **`core_options_json`** serializes `defs` to the JSON array above (values +
  labels + default). Built lazily, cached.
- Wire the three functions into the driven-core `rp_core_abi` export; bump the
  exported `abi_version` to 9.

## Component 3 — EverythingBox integration

- **Persistence key = underlying libretro core name** (from `core.json`'s
  `libretro_core`, e.g. `mupen64plus_next`), reusing
  `Settings::setOptionValue/optionValue` + `OverrideLayer`. A setting therefore
  applies whether that core runs via the native backend or via RetroPark
  (least-surprising, DRY). This is the single seam to change if RetroPark should
  instead keep a separate option namespace.
- **Harvest:** create a headless `RP_GFX_NONE` runtime, `load_core(shim dir)`,
  `rp_runtime_core_options_json`, parse with `QJsonDocument` into the existing
  `CoreOption` structs, destroy. (Same headless-before-content property as the
  native path.)
- **Global editor:** the RetroPark shim cores surface in the existing per-core
  options editor (`presentEmulatorCorePicker` / `editCoreOptions`), harvesting via
  the runtime instead of `LibretroCore`.
- **In-game:** `RetroParkView` gains a nav-kit pause-menu "Core Options" page
  mirroring `RetroView`'s #95 — same `CoreOption` model, same per-core vs.
  per-game scope toggle, applying live via `rp_runtime_core_option_set` and
  persisting through the shared `OverrideLayer` + `Settings`.
- **Launch:** before `load_content`, resolve the effective value set (per-core
  baseline + per-game delta) and push each via `rp_runtime_core_option_set`, so
  the core reads them at `retro_load_game`.

## Error handling

- A core with no options → `core_options_json` returns `"[]"`; EB shows the same
  empty state the native path already handles for optionless cores.
- Unknown key → `core_option_set` = `RP_ERR_NOT_FOUND`; get = `NULL`.
- Presenting core (no channel) → runtime wrappers return `"[]"` /
  `RP_ERR_UNSUPPORTED`; `RetroParkView` shows no "Core Options" entry.
- The shim supports a single active instance (existing constraint); headless
  harvest creates + destroys its own runtime, so it does not collide with a
  running game.

## Testing

**RetroPark (gated, alongside the existing NES/N64 e2e):**
- Load fceumm headless through the shim; assert `core_options_json` parses and
  contains a known key (e.g. `fceumm_sndvolume` / `fceumm_palette`).
- `core_option_set(known_key, non-default)` then `core_option_get` echoes it;
  `GET_VARIABLE` observed by the core returns the override; `GET_VARIABLE_UPDATE`
  fires exactly once after a set, then false.
- JSON validity / round-trip (parse the emitted JSON, re-check fields).
- If a harvested option has a cheaply observable effect on output, assert a frame
  changes after setting it (best-effort; skip if none is cheap).

**EverythingBox:**
- Unit test: JSON descriptor → `CoreOption` parse (fields, value/label order,
  default).
- Smoke: harvest-through-runtime returns a non-empty option set for the shim N64
  core.
- Pause-menu page verified via `EB_UITEST`.

## Scope & done-bar

In scope: libretro-shim cores only. **Done-bar:** EB-integrated **and deployed**
to `C:\EverythingBox-app` via targeted copy (v9 exe + v9 `dolphin_present.dll` +
shim), matching the standing pattern. RetroPark changes merged to `main` and
pushed; EB submodule bumped to the v9 RetroPark commit. No AI attribution in any
commit / PR body. Cores and ROMs never committed.
