# Getting help with RetroPark

RetroPark is a runtime and a set of cores, not an end-user app — most questions
are about building it, integrating a core, or the ABI.

**Build problems, "how do I write a core?", ABI and integration questions** →
the EverythingBox Discord: <https://discord.gg/bW7KMVhgwH>. Post in
`#dev-general`, and say which **backend** (D3D11 / Vulkan), which **core**, and
your **GPU/driver** — the presenting path is GPU-sensitive, so that context is
usually the fastest route to an answer.

**A reproducible bug, or a design proposal** → the
[issue tracker](https://github.com/cubman3134/RetroPark/issues/new/choose). Those
need to stay searchable and stay open until they're fixed, and chat is bad at
both.

**Contributing code** → [CONTRIBUTING.md](../CONTRIBUTING.md) first; it lists the
build gotchas and the two contracts (the core model and the ABI version) that
actually get a pull request rejected.

RetroPark is the emulation runtime for
[EverythingBox](https://github.com/cubman3134/EverythingBox); if your question is
really about the app, start there instead.
