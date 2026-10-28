# 2026-10-28 — SIMD-DISPATCH CI: the six-step transpose's twiddle tile and the command payload buffer

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `efb2b6d8`.

## CI read

Every completed run in the last 15 is a cancelled push run with no failed job. Push run 40701630725 (`3c1debc6`)
is still in progress with four finished failures, two causes:

- linux-gcc-release and linux-gcc-shipping (jobs 122882441613, 122882441423): `-Werror=maybe-uninitialized` on
  `twtr`/`twti` in `fs6_transpose_inplace`, avx2 unit, from `d769accb`.
- win-clang-cl-shipping and linux-gcc-public-checks (jobs 122882441346, 122882441602): SIGSEGV in the
  `[scene][commands][sink]` case of `test_schedule.cpp`, after its first flush.

Run 40717403917 (`efb2b6d8`, the ARM64 lanes' first run) is pending.

## fft: each use builds its transposed twiddle tile

The tile was filled under `twiddle && (r != c || steps > 1)` and read on paths GCC could not tie to that test.
`transposed_state` now fills a tile declared inside each `if (twiddle)` that reads it (the off-diagonal U-half and
the two-tile diagonal), on exactly the old paths, so the bits are unchanged. GCC 13.3 with the hosted command
(`-O3 -march=x86-64-v3 -Werror`, avx2 unit) reports 8 warnings on HEAD and none on the fix.

## scene: an aligned payload buffer that relocates through each type

`Commands` kept payloads in an `Array<u8>`, allocated at alignment 1: an offset aligned to the type was not an
aligned address, and every growth moved the payloads by byte copy. The 64-aligned test component's typed move then
faulted in optimized builds. Three defects, fixed at their cause:

- The buffer is an `Array` of `kMaxComponentAlignment` (64) blocks, so an offset aligned to the type is aligned in
  memory; `register_type` refuses a type aligned above it at compile time, and the chunk layout asserts it holds.
- A growth relocates every live payload through its move constructor and destructor, as the storages do.
- A flushed payload is destroyed after the storage moved from it (it leaked before), and archetype storage now
  destroys its stored components when torn down (they leaked at `World` destruction).

Test: `[scene][commands][payload]` queues 24 self-pointing, live-counted components and 24 64-aligned ones before
one flush (several growths), checks each stored one anchored, aligned and valued, and the live count after the
flush, after a dropped unflushed queue and after the world. win-debug crd-scene-tests: 300 cases pass.
