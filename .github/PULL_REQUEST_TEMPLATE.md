## What changed

<!-- The change itself, in plain terms. Link the issue if there is one. -->

## Why

<!-- What was wrong or missing. If you worked around something surprising — a
     driver quirk, an ordering constraint in the shared-surface handshake — say
     what surprised you. That context is the part a future reader can't
     reconstruct from the diff. -->

## Core model / ABI impact

<!-- Does this touch the two-core contract or the ABI?
     - Frontend API only (retropark.h) — no ABI change
     - Core ABI change (retropark_abi.h) — RETROPARK_ABI_VERSION bumped, and every
       core rebuilt against the new header
     - A specific core / a render backend
     If you bumped the ABI, say what forced it and why it couldn't be done
     host-side. If you didn't, confirm the core and host still agree on layout. -->

## How it was verified

<!-- What you actually ran. "Builds clean" is not verification. For a rendering
     or presenting change, say which backend and which GPU you ran it on — and
     confirm the GPU tests actually executed rather than SKIPPED, since a pass
     where they all skipped proves nothing about the presenting path.
     Screenshots/clips for anything visual. -->

## Checklist

- [ ] `ctest --test-dir build -C Debug --output-on-failure` passes locally (all test cases)
- [ ] GPU/presenting tests actually **ran** (not skipped) if this touches rendering or the shared surface
- [ ] `RETROPARK_ABI_VERSION` bumped **iff** the core ABI changed — and not otherwise
- [ ] A new pure component has a headless doctest; a new core is wired into CMake
- [ ] Commit messages use a conventional prefix (`feat:` / `fix:` / `docs:` / `refactor:`)
- [ ] No AI/tool attribution in commits or this description
