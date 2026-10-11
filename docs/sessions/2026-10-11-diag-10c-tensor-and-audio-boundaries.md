# DIAG.10c probes at the tensor and audio boundaries, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10c value domains and the result oracle](2026-10-11-diag-10c-value-domains-and-result-oracle.md).

## Goal

DIAG.10c's next clauses: shape/stride and units/time-domain probes, the float domains at the tensor and audio
boundaries, and the numerical-mode probe at the audio callback and around third-party calls. Committed locally, not
pushed.

## Layout probes (crd-core)

`engine/foundation/core/include/crd/core/layout_probe.hpp`, in crd-core so any module's views inherit it:

| API | What it does |
| --- | --- |
| `probe_layout(shape, strides, rank, expectation)` | `LayoutViolation` bits: RankOverflow, ElementCountOverflow, OffsetOverflow, OutsideStorage, Overlap; the element count, first and last addressed offsets, `LayoutOverlap` (Disjoint, Overlapping, NotProven) and whether storage was checked |
| `LayoutExpectation` | rank bound, element size, whether the boundary writes through the view, and an optional storage range in elements |
| `probe_strided_values(domain, base, shape, strides, rank, lanes)` | every scalar of a strided view against a `FloatDomain`, from its bits; the first offender's coordinates, row-major index and lane |

Decisions:
- The element count is the product of the extents and must fit a u64 with its byte size. An empty view's other
  extents must still multiply out: a compact layout of the same dims is computed from them, and that product is
  where the DLPack defect below overflowed.
- Offsets are formed one dimension at a time with checked i64 arithmetic (`INT64_MIN` as a reach is exact), then in
  bytes. They are compared with storage only when the caller names it; unnamed storage is "unchecked", never inside.
- Overlap is reported only when proven. Proven overlapping: a zero stride on an extent above 1, or strides `a`, `b`
  with `|b|` dividing `|a|` and `extent_b - 1 >= |a| / |b|` (index 1 along `a` meets index `k` along `b`, or the origin
  when the signs differ). Proven disjoint: sorted by magnitude, each stride exceeds the largest offset the smaller
  ones reach. Otherwise NotProven, which is never reported as an overlap (offsets `3i + 5j` over extents 3 and 2 are
  distinct but no rule shows it). An overlap is a violation only when the boundary writes through the view.
- The walk runs only over a layout the probe passed, so every offset it forms is an addressed element's offset. It
  reads bits only; the report under flush-to-zero, denormals-are-zero and round-down is the report under the declared
  mode.
- `call_keeping_fp_environment(call)` (`fp_environment.hpp`) runs code the engine does not own, returns the
  `FpDifference` bits it left and puts the caller's mode back.

## The tensor boundary (crd-hesap-tensor)

`engine/numerics/hesap-tensor/include/crd/hesap/tensor/probe.hpp`: `probe_tensor_layout(view)` (a const view is a
read, so a broadcast is no violation; an overload names storage) and `probe_tensor_values(view, domain)` for f32,
f64, c32 and c64 views (a complex value is two lanes). Integer and mask tensors are not float domains; the template
refuses them at compile time.

DLPack import (`dlpack.hpp`, `wrap_dl_tensor`) is where another framework's tensor arrives. Defects found and fixed:

| Capsule | Before | Now |
| --- | --- | --- |
| `[2^32, 2^32, 2]`, strides `[0, 0, 1]` | the count `2^65` wrapped to 0; imported as an empty tensor | BadInput (ElementCountOverflow) |
| `[3]`, stride `2^62` | imported; element 2 sits at offset `2^63`, so any walk is undefined pointer arithmetic | BadInput (OffsetOverflow) |
| `[0, 2^40, 2^40]`, NULL strides | the compact strides overflowed a signed i64 product (undefined), then imported as empty | BadInput (checked before the multiply) |
| `ndim = 2`, `shape = NULL` | dereferenced the null shape array | BadInput |
| two elements of stride -16 below address 32; eight elements above the last 16-aligned address | imported a view whose bytes wrap the address space | BadInput |

An expanded `[3, 4]` capsule with strides `[0, 1]` (what `torch.expand` produces) still imports; the probe reports
its overlap (a violation for the writable view, none for a const import). An empty `[0, 4, 5]` capsule with NULL
strides still imports. The npy and safetensors readers already guard the count (`iodetail::shape_elems`).

A producer's deleter is the producing framework's code. `DlpackImported::release` now runs it through
`call_keeping_fp_environment` and returns the bits it changed; the destructor and the spec-mandated disposal of a
newer-major capsule run it the same way.

## The audio boundary (crd-audio)

`engine/media/audio/include/crd/audio/audio_boundary.hpp`:
- `GraphSourceBinding::sample_rate` declares the rate a binding's samples were made at; 0 is undeclared.
  `check_source_binding(binding, graph_rate)` returns Bound, RateUndeclared, Unbound, PartialFrame or RateMismatch
  (shape before rate). `render_graph` checks against `AudioGraphResource::sample_rate`, `execute_audio_graph_ceir`
  against the func's `sample_rate` attribute (48 kHz when absent, the existing default). A binding that does not
  render is refused; an undeclared rate renders byte for byte as before. Nothing is resampled.
- Defects the shape check found: 129 samples bound as stereo rendered with the trailing half frame silently dropped,
  and a looped source holding one sample for two channels has zero whole frames, so `apply_source`'s `f %= bframes`
  divided by zero (the process died with 0xC0000094 on win-debug in the tooth below).
- `audio_sample_domain()`: finite, no bound (headroom above 1 is legal before the master stage), subnormals and -0
  allowed.
- `render_audio_period(fn, user, device_out, frames, device_channels, rate, stereo_scratch, scratch_floats)`: the
  device's per-period work, testable without hardware. It silences the period, runs the callback inside a
  `ScopedFpEnvironment` of the declared mode, reports `entry_fp_difference` (the device thread's mode) and
  `exit_fp_difference` (the mode the callback returned in), puts the thread's own mode back and probes the stereo the
  callback wrote. Nothing allocates.
- Defect found in the WASAPI device: the callback contract is interleaved stereo, but the device passed the endpoint
  buffer interleaved at its own channel count, so a 5.1 or 7.1 endpoint received stereo at the wrong stride. A
  non-stereo endpoint now renders through a stereo scratch that `start()` sizes from the buffer, and the stereo lands
  in its first two channels.
- `AudioDevice::fp_mode_divergence_count()` and `nonfinite_period_count()` count periods; the existing soak case now
  requires both to be 0 where an endpoint exists.

## Tests

crd-core-tests, `[diag10c]` (`tests/foundation/core/test_diag_layout_probe.cpp`, six new cases; sixteen with the
earlier ten):

| Case | Asserts |
| --- | --- |
| a layout probe reads offsets and proves overlap only where it can | contiguous, flipped, transposed offsets; a broadcast overlaps and is a violation only when written; a stride-0 dim of extent 1 does not overlap; equal, divisible and opposite-sign strides overlap; nesting strides are disjoint; `3i + 5j` is NotProven and no violation; rank 0 |
| a layout probe refuses counts and offsets past the integer ranges | the `2^65` count, an empty view with `2^80` other extents, `2^62` elements of 4 bytes (and of 2, which fits), offsets `2^63` and exactly `-2^63` (fits in elements, not in 4-byte units), `INT64_MIN` as a stride, two reaches that overflow only summed, rank past the bound |
| storage is checked only when it is named | inside, past either end, flipped storage, unnamed storage unchecked, an empty view inside any storage, overflowing offsets not reported inside |
| a strided walk names the first offender by its coordinates and lane | column-major storage, a flipped view, interleaved complex lanes, a broadcast walked once per index, `any` reports nothing |
| a strided walk refuses what it cannot walk and ignores the numerical mode | invalid domain, overflowing layout, empty view, zero lanes; identical reports under FTZ, DAZ and round-down |
| a foreign call's mode change is reported and undone | a quiet call gives 0; a call that sets FTZ gives FlushToZero and the mode is back |

crd-hesap-tensor-io-tests, `[diag10c]` (`tests/numerics/hesap-tensor/test_diag_tensor_probe.cpp`, five cases): the
five refusals above, the expanded and empty capsules still importing with the probe's report (overlap, storage), the
missing shape array, the tensor value probe over a column-major view and its permutation (same element, new
coordinates), c32 lanes against `finite` and `distance`, an f64 view; and a producer deleter that sets FTZ, reported
by `release()` and undone on the release, destructor and newer-major paths.

crd-audio-tests, `[diag10c]` (`tests/media/audio/test_diag_audio_boundary.cpp`, seven cases): every binding verdict;
`render_graph` with a declared, undeclared (byte-identical), mismatched and ragged binding; the looped sub-frame
source; the CEIR executor at an explicit 44.1 kHz, at the implicit 48 kHz, undeclared and ragged; a NaN source sample
named on the bus at stereo index 11; a render period in the declared mode, from a round-down thread (the callback sees
the declared mode, the entry is reported, the thread's mode comes back), with a callback that sets FTZ (reported, gone
by the next period) and one that writes a NaN; a six-channel endpoint through the scratch, and silence without
scratch, on a mono endpoint or with no callback.

## Teeth

Sixteen deliberate breaks on win-debug (seventeen observations: the partial-frame break was observed on two cases),
each built, observed failing, restored byte for byte, the target rebuilt and its `[diag10c]` cases green again (core
16, tensor 5, audio 7):

| Break | Result |
| --- | --- |
| element count by plain multiply | count case failed (2 assertions) |
| an empty view stops counting its other extents | count case failed |
| offsets by unchecked multiply | the count and storage cases failed (3 assertions) |
| no divisibility overlap rule | overlap case failed |
| storage always reported checked | the offset and storage cases failed (3 assertions) |
| the walk reports the physical offset as the index | walk case failed |
| `call_keeping_fp_environment` does not put the mode back | foreign-call case failed |
| DLPack import ignores the probe's verdict | count/offset refusal case failed (3 assertions) |
| DLPack import without the shape-array check | the missing-shape case died on an access violation (0xC0000005), reported by Catch2 |
| DLPack import without the low address check | refusal case failed |
| the deleter called without the witness | deleter case failed (4 assertions) |
| the binding rate ignored | the render_graph and CEIR cases failed |
| the partial-frame check ignored | the render_graph case failed; the looped sub-frame case died dividing by zero (0xC0000094), reported by Catch2 |
| the CEIR executor keeps its old binding check | CEIR case failed |
| the period runs the callback without the declared-mode scope | period mode case failed (4 assertions) |
| a wide endpoint written directly | layout case failed |

The NULL-strides overflow guard has no Windows tooth of its own: the probe's count rule refuses the same capsule, and
the guard only keeps the signed overflow from happening first (a UBSan lane would report it).

## Verification

Whole programs, final source (crd-core-tests, crd-hesap-tensor-io-tests, crd-audio-tests):

| Lane | core | tensor io | audio |
| --- | --- | --- | --- |
| win-debug (whole tree built) | 51 cases | 19 | 29 |
| win-shipping | 43 | 19 | 29 |
| win-clang-cl-shipping (linked first time) | 43 | 19 | 29 |
| win-asan (inside vcvars) | 51 | 19 | 29 |
| linux-gcc-debug | 43 | 19 | 29 |
| linux-gcc-asan, `[diag10c]` repeated 3 times | 43 | 19 | 29 |
| linux-clang-tsan (`setarch -R`), `[diag10c]` repeated 10 times | 43 | 19 | 29 |

- The only sanitizer reports are the crash-capture specimen's deliberate faults (12 on linux-gcc-asan, all from
  `crash_capture_specimen.cpp`); none in any `[diag10c]` run, and no TSan report.
- All 36 `[diag10c]` CTests on win-debug pass through CTest (core 16, jobs 4, ceir-cook 4, tensor io 5, audio 7).
- The full win-debug CTest was not run; the touched programs ran whole on every lane above, and the guard, coverage,
  seeded-fault, build-profile, master-plan and repository-hygiene CTests ran.
- The live soak on this host (a WASAPI endpoint exists here): 10 s at 48 kHz, 486,240 frames, 0 xruns, 0 divergent
  and 0 non-finite periods through `render_audio_period`.
- Coverage (linux-clang-coverage on WSL, clang 18): the registry gains the scope group "layout probes and the tensor
  and audio boundaries" (6 files), the programs crd-hesap-tensor-io-tests and crd-audio-tests, and the outcome sets
  LayoutViolation 5 of 5, LayoutOverlap 3 of 3 and SourceBindingVerdict 5 of 5. All 74 scope files instrumented,
  every CTest of the run passed, verdict PASS; regions 9,883/11,083, branches 6,472/8,220, functions 1,114/1,166,
  lines 18,324/21,028 (the new group: regions 344/400, branches 233/310). The 1.6 GB of profiles are deleted.
- Strict tidy (LLVM 20.1.8) is clean on all 16 changed C++ files. clang-format differs only in the repository's hand
  alignment, case-label indentation, include grouping and the neighbouring one-line function style of
  `audio_device_wasapi.cpp`.
- The Allman check, the 8 CTest guards, `crd-build-profiles`, `crd-diag-coverage`, `crd-diag-seeded-faults`,
  `crd-repository-tools`, `diag-coverage.py --validate`, `test-diag-coverage.py` (29), check-master-plan and
  check-repository pass. No script or workflow changed.
- The soak case's early return when no endpoint exists is unchanged (a WARN and return): converting it to a skip is
  the queued WARN-to-SKIP item, not this batch's.

## Follow-ups

- The mode witness around GPU driver calls (the gpu-context backends) belongs with the driver and device-reset
  evidence clause.
- `probe_layout`'s overlap proof is sufficient, not complete: a layout like offsets `3i + 5j` stays NotProven. An
  exact check (a small integer search bounded by the extents) could replace NotProven where rank and extents are small.
- `TensorView::contiguous` computes its strides with an unchecked signed product, like the DLPack path did; a
  caller-built view of absurd extents overflows there. The owned `Tensor` constructor already checks its count.
- The units library (`crd-units`) is compile-time dimensional; the only runtime unit at these boundaries is the
  sample rate. A runtime rate or time-base check for timeline and video boundaries would reuse `check_source_binding`'s
  undeclared-is-unchecked rule.
- The audio device does not resample a mismatch between the endpoint's rate and a graph's rate; `render_audio_period`
  passes the endpoint rate to the callback, and checking it against an offline graph's rate is the host's job.
