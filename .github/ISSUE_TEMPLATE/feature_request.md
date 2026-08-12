---
name: Feature request
about: Something the runtime, the ABI, or a core should be able to do and can't
title: ''
labels: enhancement
assignees: ''
---

## The problem

<!-- Describe the situation you're stuck in, not the feature you've designed.
     "There's no way for a frontend to tell whether a core is still loading
     content" is more useful than "add a loading callback" — the first admits
     several solutions, one of which may already half-exist in the runtime. -->

## Which side of the boundary

<!-- Is this a change to the frontend API (retropark.h), the core ABI
     (retropark_abi.h), a specific core, or a render backend? An ABI change
     affects every existing core and bumps RETROPARK_ABI_VERSION, so it's worth
     saying up front whether you think this needs one — and whether the same
     thing could be done host-side without touching the ABI at all. -->

## Which core model

<!-- Does this apply to driven cores, presenting cores, or both? The two models
     have different lifecycles and guarantees; a feature that makes sense for one
     often makes no sense for the other. -->

## What you tried

<!-- Existing API, a workaround host-side, how another runtime (libretro?) solves
     the same thing. If you went looking for an existing hook and couldn't find
     one, say where you looked — sometimes it already exists somewhere nobody
     thinks to check. -->
