# Camtesia

A native Windows (10 2004+, x64) screen recorder and video editor written in C++20 with Win32, Direct3D 11,
Media Foundation and Dear ImGui. It ships as a single executable with no runtime dependencies.

Build (VS 2022 Build Tools required; run `build.cmd doctor` to check the toolchain):

    build.cmd release      (or: build.cmd debug | test | clean)

The executable lands in `out/build/ninja-release/bin/Camtesia.exe` (Debug: `out/build/ninja-debug/bin/`).

Start with [MASTER_PLAN.md](MASTER_PLAN.md) for scope, architecture and the phase-by-phase build plan.
