---
name: Bug report
about: A core, backend or the runtime behaves differently from how it should
title: ''
labels: bug
assignees: ''
---

## What happened

<!-- Describe what you actually saw. For anything visual — a torn frame, a wrong
     composite, a black surface — a screenshot or a short clip is worth far more
     than a description. -->

## What you expected instead

<!-- Sometimes obvious from the above; often not. The gap between the two is the
     actual bug report. -->

## Steps to reproduce

1.
2.
3.

<!-- If it only happens sometimes (a race in the shared-surface handshake, a
     resize timing bug), say so and roughly how often. An intermittent bug
     reported as a reliable one wastes everyone's first hour. -->

## Environment

- **OS:** <!-- Windows 11 / … -->
- **Backend:** <!-- D3D11 / Vulkan -->
- **Core:** <!-- refcore_present_vk / libretro_shim / dolphin_present / rpcs3_present / … -->
- **GPU + driver:** <!-- e.g. NVIDIA RTX 4070, 552.44 — the presenting path is GPU-sensitive, so this matters -->
- **How you got it:** <!-- built from source (commit) — there are no prebuilt downloads -->
- **Vulkan SDK version:** <!-- if the failure is at build/shader-compile time -->

## Test suite

<!-- Does `ctest --test-dir build -C Debug --output-on-failure` pass on your
     machine? If GPU tests SKIPPED (unsupported adapter / headless), say so —
     a pass where everything skipped tells us something different from a real
     pass. If a specific test reproduces the bug, name it. -->

## Relevant log / output

<!-- Paste the lines around the failure rather than a whole dump, and check them
     for anything you'd rather not publish (local paths with your name, ROM
     paths). -->

```
```
