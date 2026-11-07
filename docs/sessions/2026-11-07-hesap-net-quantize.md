# 2026-11-07: quantisation of replicated state, bit-exact with serialize and networked-physics

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 104.

## CI first

- Dispatch 42298265296 (7b1b1b32): 14 lanes' one failure was `crd-comply-rules-drift`. Marking eu.dual-use implemented changed what
  EU's generation renders, and only that CTest could see it. EU reviewed to generation 11; `crd-check-registers` now holds
  the rendered rules to the reviewed digest too, so the validators catch it before a push. The register review tests now review
  a read's changed rules. The first push of that guard left `test-repository-tools.py` failing for 8 tests; 72674452 fixed it.
- Push run 42282917539 (474a37f8): win-debug finished in 3 h 27 under the cap and linux-clang-arm64-asan passed, so both of batch 99's
  fixes are confirmed. Its one failure is B14-d (CKIR NRC on Vulkan) timing out under linux-clang-tsan: not this track's, recorded.
- No completed run contains ac88bfed or later: the NEON arm and the libwebrtc and ntpd installs stay open.

## What landed, `quantize.hpp` in crd-hesap-net (capability `numerics.quantization`), no allocation

- `BoundedFloat`: fewest bits for a resolution, at most 24, as serialize's compressed_float: two roundings, its integer clamp.
- `SmallestThree`: 2 to 16 bits per component in one 64-bit word, as compressed_quaternion: first-of-the-largest ties, the
  source's `1 / 1.414214f` bound, kept because both are wire format.
- Hostile words: a bounded code above the top is refused and decodes to min. A quaternion word with a stray bit, or past
  `3 * (1/2 + half step)^2`, the most an encoder can reach (derived in the header), is refused; every word decodes to a unit
  quaternion. The peer returns NaN past length one, which it also reaches itself at two bits.
- Kernel family `hesap.quantize`: one body over each level's lanes (`crd/math/simd`), portable, SSE2, AVX2, AVX-512, NEON, a padded
  block for the tail instead of a scalar path, no fused operation; known-answer self-check.

## Evidence

- `crd-hesap-quantize-peer-tests` (linux-gcc-public-checks, LTO linux-gcc-release; x64 levels under SDE icx too): 1.2 million
  assertions, bit for bit: every wire constant for 200,000 declarations, codes and decoded values through serialize's own streams
  at every level, the top of the 24-bit range and refused codes, every word and quaternion at 2 to 16 bits. The peers are pinned
  files of the upstream authors: serialize v2.1.3 and networked-physics-gdc-2015's Snapshot.h, with the struct cut at configure time.
- `crd-hesap-net-tests` `[quantize]`, 1.07 million assertions in 17 cases, on win-debug and both Linux lanes: bounds, clamps, NaN,
  half-step error, every code of small formats, every word of 2 to 5 bits, each refusal restated, every length 0 to 70 at every level.
  Forced-level arms pass on all five CTest levels available here (avx512 under SDE); the NEON body passes an aarch64 clang syntax check.
- `crd-fuzz-hesap-quantize`, 13 seeds: 888,975 libFuzzer executions clean under ASan and UBSan; replay clean on win-debug.
- 21 teeth, each caught and restored; two only bit after strengthening: serialize's constants now see resolutions that leave a part
  step, and the 24-bit peer case now runs the batch too. clang-tidy clean: 20.1.8 on win-debug's files, 20.1.0 on the peer lane's.
- [Board](../bench/2026-11-07-hesap-net-quantize-vs-serialize.md): on SSE2 3.5x and 1.9x serialize with its stream, 3.6x and 1.1x
  networked-physics; on AVX2 4.7x and 2.5x, 6.2x and 1.5x. The portable level, a scalar build's, is 1.7x behind the peer's quaternions.

## Next

Interpolation, extrapolation, IEEE 1278.1 dead reckoning and rewind sampling; then NET-DEBUG's Philox distributions and RaptorQ.
