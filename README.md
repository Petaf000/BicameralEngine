# Bicameral Engine

[![CI](https://github.com/Petaf000/BicameralEngine/actions/workflows/ci.yml/badge.svg)](https://github.com/Petaf000/BicameralEngine/actions/workflows/ci.yml)

A GPU-resident simulation engine for a game about **magic as research** in an open world.

The world — elements, air, chains of reactions, physics — lives in VRAM and never round-trips to the CPU.
Chains of reactions run to completion on the GPU with **DirectX 12 Work Graphs**, so their depth and size are
decided at run time rather than capped per frame. The player observes phenomena, forms hypotheses, and discovers
the hidden rules of the world; magic is an intervention in those rules (catalysis, inhibition, conversion,
amplification), not a spell that spawns fire.

> Status: early. The build, test and CI plumbing works and the engine can probe the GPU for
> SM 6.8 / DXR / Mesh Shaders / Work Graphs. The reaction system is being designed.

## Architecture map

**https://petaf000.github.io/BicameralEngine/**

A hand-written flow diagram of the engine where every node links to its source lines on GitHub
(implemented parts) or to the design document / ticket (planned parts). The diagram is defined in
[`docs/architecture/map.yaml`](docs/architecture/map.yaml) as `file::symbol` references;
[`tools/archmap`](tools/archmap/archmap.py) resolves them to line numbers on every CI run and **fails the build
if a function was moved or renamed**, so the diagram cannot silently drift from the code.
A Doxygen reference with call graphs is published next to it.

## Design principles

- **The runtime is GPU-resident.** The CPU only handles the window, input, command-list submission, Present,
  audio output and save I/O. Tools and the editor may use the CPU freely and *bake* data for the GPU.
- **Deterministic propagation.** Reactions are propagated generation by generation (read the previous
  generation, write the next), so the same input and seed produce the same world — players must be able to trust
  their experiments.
- **Every GPU kernel has a CPU reference implementation** and a test that compares the two on small boards.
- **Work Graphs for work whose amount is decided at run time** (active-node selection, reaction chains,
  spawn/destroy, contact generation); compute dispatches for fixed-iteration solvers.

Design documents (Japanese): [vision](docs/design/00-vision.md) ·
[architecture](docs/design/01-architecture.md) · [reaction system](docs/design/02-reaction-system.md) ·
[decision records](docs/decisions/)

## Building

Requirements: Windows 11, Visual Studio 2022 or later with the C++ workload, CMake 3.25+, Ninja, and
[vcpkg](https://github.com/microsoft/vcpkg) (`VCPKG_ROOT` set). Running the engine needs a GPU with
Shader Model 6.8 and Work Graphs (RTX 30 series / RDNA 3 or newer).

Dependencies (DirectX Agility SDK 1.619, DirectX headers, DXC) come from vcpkg, pinned by the
`builtin-baseline` in [`vcpkg.json`](vcpkg.json).

From a *Developer PowerShell for VS*:

```powershell
cmake --preset debug           # or: release, profile
cmake --build --preset debug
ctest --preset debug
out\build\debug\bin\bicameral.exe --caps   # print what the GPU supports
```

Or open the folder in Visual Studio ("Open Folder") and pick the `debug` configuration.

## Repository layout

| Path | Contents |
|---|---|
| `engine/src/` | Runtime, one folder per subsystem |
| `shaders/` | HLSL (SM 6.8), one folder per subsystem |
| `tests/` | CPU tests, including CPU-reference vs. GPU comparisons |
| `tools/` | Editor and build tools (e.g. `archmap`) |
| `docs/` | Design notes, decision records (ADR), tickets |
| `runner/`, `scripts/` | A small job runner used to build and test from an AI pair-programming session |

This project is developed together with Claude (Anthropic); the working agreement is in
[`CLAUDE.md`](CLAUDE.md) and [`docs/claude/`](docs/claude/) (Japanese).

## License

No license is granted at this time: the code is published for reading, and all rights are reserved.
