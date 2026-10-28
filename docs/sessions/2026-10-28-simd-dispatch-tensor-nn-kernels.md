# 2026-10-28 — SIMD-DISPATCH: nn's convolution, pooling and relu move into the tensor kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `1ee25fbe`.

## CI first

No completed run among the last 15 has a failed job: all are push runs the concurrency group cancelled. Push run
40701630725 (`3c1debc6`) is still running; its six failed jobs have causes fixed at HEAD, as the
[batched note](2026-10-28-simd-dispatch-tensor-batched-kernels.md#ci-first) records. No new green run, so DIAG.11c and
DIAG.12 stay Needs CI.

## What moved

`nn.hpp` no longer holds a vector body and no longer includes `simd.hpp`. A fifth unit of the tensor family,
`kernels/tensor_nn_kernels.cpp`, holds two new table entries, appended after `sparse_f64`:

- `nn_conv2d_images` (`NnConv2d`): per image, the direct 3x3/s1/p1 kernel (row staging, up to eight output channels'
  fma chains in registers, the fused relu and 2x2/s2 pool) or im2col, the bias prefill and the batched unit's tiny
  GEMM. At avx512 the output columns step a `Vec16f` while 16 remain, then `Vec8f`; the staging slack already covers
  the wider loads, so the scratch formula, which moves to `tensor_kernels.hpp` beside `NnConv2d`, is unchanged.
- `nn_pool2d_planes` (`NnPool2d`): max and average pooling, the 2x2/s2 shape's vertical maximum in registers.

`conv_run` stays in `nn.hpp` as the parallel callback: it picks the worker's scratch slot and makes one table call per
image range. `relu_span` is the elementwise Max walk with a broadcast zero (`maxps`, the same bits as before), which
may run in place; `EwBinaryWalk` now says so.

## One max-pool rule

The three max-pool paths disagreed on NaNs and signed-zero ties: the 2x2/s2 path (w <= 256) and the fused pool took
the second operand (`maxps`), the generic path kept the first, so `nn_maxpool2d(k=2, s=2)` gave different bits at
w <= 256 and above it. All three now use IEEE 754 maximum (a NaN wins, +0 over -0), which no window order changes,
NaN payloads aside, so the fused pool still gives the bits of conv, relu, pool. Results change only for windows with a
NaN or a signed-zero tie; the frozen torch corpus has neither, and `test_nn.cpp`'s pool reference uses the new rule.

## Tests

- `test_tensor_kernels.cpp` gains a `[kernels]` case: every level the host runs against scalar oracles, with NaNs,
  infinities, signed zeros and subnormals in every operand, split image and plane ranges and guard words after the
  output and the scratch slot. Pooling: heights 2 to 5, widths 2 to 257 (both sides of the 256-wide stage), six
  windows and strides, max and average, on inputs full of ties. Convolution: every output-channel block 1 to 8 (and
  17), widths 1 to 33, relu and the fused pool on and off, im2col at kernels 1 to 5 and 1 x 3, pads 0 to 2, strides
  1 to 3. The archive case checks both entries.
- `crd-hesap-tensor-nn-kernels-at-<level>` now runs the whole `[nn]` suite (the conv and pool gates, the CNN forward,
  the worker moat) under each forced level and under SDE.
- `include-budget.json` holds `nn.hpp` with the SSE2 intrinsics headers forbidden.

Local: win-debug native (portable, sse2, sse4.2, avx2, avx2+AVX-VNNI) and SDE 10.13.1 `-skx` at avx512 pass the new
case and the archive case; the native `[nn]` suite and `crd-check-include-budget` pass.

## Next

sddmm moves the same way, then the jets need a per-level evaluation boundary, then the flip. Row-close board
candidates: the fused pool's horizontal step and the generic windows stay scalar.
