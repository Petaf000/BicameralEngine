# Draft notes: letting Work Graphs decide where heat flows (T-0005)

*Notes for a future English blog post. Rough; numbers are from one machine (RTX 3070 Ti, driver 32.0.15.9597) and WARP.*

## What was built
- A 64³ integer heat grid (uint32 per cell), split into 4³ blocks (4096 blocks). One tick = one `DispatchGraph`.
- Graph: `WakeBlocks` (thread launch, entry) → `ConductBlock` (broadcasting, one 64-thread group per block).
  The entry records come from **GPU memory** (`D3D12_DISPATCH_MODE_NODE_GPU_INPUT`): a list the previous tick's
  `ConductBlock` groups appended to whenever a block's values changed. The CPU never learns how many blocks are active.
- Rule: compute block B at tick t iff B or a face neighbour changed at t−1 (or was poked at t).
  A block outside that set would compute exactly the same values again, so skipping it is lossless.
  Double-buffered generations stay valid because a block that changed is always recomputed on the next tick.
- Flux across a face = sign(Δ)·(|Δ| >> 3). Both sides compute the same magnitude, so heat is conserved exactly;
  small differences (< 8) produce zero flux, so a single hot spot spreads and then **stops** — the active set shrinks to zero
  (1268 ticks for one 2^24 poke, peaking at 3198 of 4096 blocks).

## How it is tested
- The CPU reference computes **every cell every tick** (no activity tracking). The GPU computes only active blocks.
  They match bit-for-bit (state hash per tick, total heat, and even the number of blocks the GPU scheduled),
  across five different ways of slicing ticks into frames, on hardware and WARP.

## Things I learned
- `D3D12_NODE_GPU_INPUT` can live at the head of the same buffer that holds the records; `NumRecords` is just an atomic counter
  the shaders bump. The buffer has to be in a shader-resource (or COMMON) state during the dispatch, so it flips
  UAV → NON_PIXEL_SHADER_RESOURCE → UAV around each graph (two buffers, ping-ponged by tick parity).
- WARP hung on a GPU-input dispatch with `NumRecords == 0` (hardware was fine). Keeping one "no-op" record at the head of the list avoids it.
- WARP also mis-scheduled when a thread-launch node collected targets in a local array and emitted them with one
  `GetThreadNodeOutputRecords(n)`. Emitting 0-or-1 record per unrolled iteration (still group-uniform calls) fixed it. Root cause unknown.
- Dedup without clearing: each block stores "last tick scheduled + 1"; `InterlockedExchange` returns the old value, so the first arrival wins.
- Cost on RTX 3070 Ti: ~40 µs per tick regardless of scale (7 blocks or 4000), ~15 ns per block on top. Backing memory: 139,912 bytes.
  The fixed cost dominates at this grid size; worth decomposing later (SetProgram? transitions? graph launch?).
