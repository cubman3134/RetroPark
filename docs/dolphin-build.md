# Building & driving standalone Dolphin (Slice I) — reproducible recipe

This is the exact, verified recipe that builds standalone Dolphin from source (no libretro) and boots a
real GameCube ROM headless-ish on the **Vulkan** backend, rendering real frames. Verified 2026-08-08 on
this machine (Windows 11, VS2022 Community, RTX 5080). Slice I proof: Billy Hatcher renders its title
screen on Vulkan (999 framedump PNGs, near-black 9 KB boot frame → 202 KB rendered title frame).

`external/dolphin` is **git-ignored** — this doc is the reproducible record, the tree is not committed.

## 1. Clone + pin

Pinned to Dolphin **stable tag `2606`** (commit `6094cfcf7b`), cloned with submodules:

```bash
git clone --recurse-submodules --jobs 4 https://github.com/dolphin-emu/dolphin.git external/dolphin
cd external/dolphin && git checkout 2606 && git submodule update --init --recursive --jobs 4
```

Externals are vendored (Vulkan-Headers, VMA, FFmpeg-bin, SDL3, etc. under `Externals/`), so no system
libraries are needed to build. `git submodule status --recursive | grep '^-'` must be empty (all
initialized) before building.

## 2. Build — DolphinNoGUI only (skips Qt)

Toolchain: **VS2022 Community, PlatformToolset v143** (MSVC 14.44), `stdcpplatest`. The solution's
`DolphinNoGUI` project references only `DolphinLib` + `SCMRevGen` + `Languages` — **never `DolphinQt`** —
so building it avoids Qt entirely. Vulkan is always compiled in (`HAS_VULKAN` is unconditional; the
loader dlopen's `vulkan-1.dll` at runtime, so no Vulkan SDK is needed to *build* — the SDK at
`C:\VulkanSDK\1.4.357.0` is only useful for validation layers).

**Gotchas (both cost a failed build before the real one worked):**
1. **Git Bash mangles `/`-prefixed MSBuild switches** (MSYS turns `/m` into `M:/`). Use **`-` switches**
   (`-t:` `-p:` `-m`), which MSBuild accepts and MSYS leaves alone.
2. **`msbuild dolphin-emu.sln -t:DolphinNoGUI` fails** — MSBuild runs `DolphinNoGUI` as a *target* on
   every project (`error MSB4057: The target "DolphinNoGUI" does not exist in the project`). **Build the
   `.vcxproj` directly instead** (it pulls in its project references transitively), passing `SolutionDir`
   so Dolphin's `$(SolutionDir)` props resolve:

```bash
MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"
"$MSBUILD" Source/Core/DolphinNoGUI/DolphinNoGUI.vcxproj \
  -p:Configuration=Release -p:Platform=x64 \
  -p:SolutionDir='C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\' \
  -m -v:minimal -clp:Summary
```

Build time ~3:47 (parallel). Output: **`external/dolphin/Binary/x64/DolphinNoGUI.exe`** (also
`Build/x64/Release/DolphinNoGUI/bin/`).

> ⚠️ The parallel build spawns ~10 MSVC + persistent MSBuild worker nodes and is memory-heavy — it may
> stress a loaded machine. After the build, kill leftover idle nodes: `taskkill //F //IM MSBuild.exe`.

## 3. Run — boot a ROM on Vulkan and capture frames (Stage A)

```bash
cd external/dolphin/Binary/x64
./DolphinNoGUI.exe \
  -u "<userdir>" \
  -e "C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz" \
  -v Vulkan -p win32 \
  -C Dolphin.Interface.UsePanicHandlers=False \
  -C Dolphin.Analytics.PermissionAsked=True -C Dolphin.Analytics.Enabled=False \
  -C Dolphin.Movie.DumpFrames=True -C Dolphin.Movie.DumpFramesSilent=True \
  -C Graphics.Settings.DumpFramesAsImages=True
```

Frames land as PNGs at `<userdir>/Dump/Frames/framedump_<N>.png`. `.rvz` is Dolphin's native disc format,
loaded directly.

**Two hard-won findings (each produced 0 frames until fixed):**
1. **`-C Dolphin.Interface.UsePanicHandlers=False` is required.** Without it, Dolphin pops a **modal
   "Warning" dialog** on boot and blocks forever (the process runs, allocates memory, but renders nothing
   — no console output, just a stuck 400×160 dialog window). Disabling panic handlers routes warnings to
   the log instead of a blocking message box, and the game boots and renders. (Analytics prompt
   suppressed similarly for good measure.)
2. **`-p win32`, not `-p headless`, for frame capture.** In pure `-p headless`
   (`WindowSystemType::Headless`, `VKGfx::IsHeadless()` true), `VideoCommon::Presenter::Present()`
   early-returns before the frame-dump path runs, so **framedumps never fire** (verified: headless run =
   0 frames after 90 s while actively emulating). `-p win32` creates a real render window and runs the
   full Vulkan present path (`VKGfx::BindBackbuffer`/`PresentBackbuffer`) — which is *also* exactly the
   seam Slice J will retarget into our shared `VkImage` (see `docs/dolphin-seam.md`). On Windows, if `-p`
   is omitted it defaults to `win32` (visible window), NOT headless.

   > For a truly windowless capture later, the in-process route (`Core::SaveScreenShot`, or hooking the
   > XFB texture) sidesteps the present-path early-return — that's Slice J/K territory, not needed for the
   > Slice I proof.

There is **no built-in "run N frames / auto-stop" flag** — drive the run for a bounded wall-clock time
and terminate (`taskkill //F //IM DolphinNoGUI.exe`); the framedump writes per-frame as it goes.

## 4. Verification (Slice I proof)

- 999 framedump PNGs written from a ~20 s run.
- Early frame `framedump_1.png` = 9 KB (near-black boot); late `framedump_999.png` = **202 KB** (a black
  frame does not compress to 200 KB), and `cmp` shows they differ → real emulation advancing.
- Eyeball confirmation: the late frame is the **Billy Hatcher title screen**, rendered correctly (logo,
  sky, clouds, 3D trees) — standalone Dolphin's Vulkan backend producing real GameCube frames.

This is heavily gated + partly manual (real GPU + Vulkan + the ROM present, plus the eyeball). The
size-progression + `cmp`-differ is the automatable signal; the rendered title screen is the human proof.
