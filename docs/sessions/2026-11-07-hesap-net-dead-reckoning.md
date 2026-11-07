# 2026-11-07: IEEE 1278.1 dead reckoning and projective velocity blending, against KDIS

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 106.

## CI first

Runs 42319502866 (23e5b36b) and 42328203363 (9a058d80) predate their fixes: crd-ci-platform (67e30587), crd-comply-rules-drift
(72674452), the clang and UBSan failures of test_clock (b565a3b6). The libwebrtc, ntpd, ISA-L and OpenFEC peers all passed
on linux-gcc-public-checks at 23e5b36b, so the first install of libwebrtc and of the ntp tarball work from CI's network.

New, this track's: win-asan, `Capabilities: verification and revocation run on several threads at once`, 19 of 20
generations applied. With one applying thread and constant inputs the only non-deterministic return is the signature check,
so this is batch 99's `Provider: one verifier and one key` seen again: concurrent verification answered Invalid once, on
win-asan only. Cerid's path holds no shared mutable state (read); not reproduced and not fixed. Both tests now retry the same
input at once and report the first answer and the retry. Until a run says which, those tests are not proven clean on win-asan.

## What landed, capability `numerics.motion-sampling`, kernel family `hesap.motion`, no allocation

- `dead_reckoning.hpp`: the nine algorithms in f64 columns, each entity its own dt, quaternions body to world, a column an
  algorithm does not use may be null. Turning is q * (rotation by |rate| dt about rate), the standard's DR on the matrix.
- R1 and R2's six entire functions: alternating Taylor series in lanes up to a turn of 1, closed forms past it, each lane
  there its own `crd::math::sincos`. A rate whose square is not finite is no rotation.
- `orientation_from_dis_euler`, `dis_euler_from_orientation`: DIS's psi, theta, phi, the wire form.
- `projective_blend` (Murphy) in f32 and f64: position and its derivative with respect to elapsed, so the next blend starts
  at the real velocity.

## Evidence

- `crd-hesap-net-tests` `[dead-reckoning]`, 15 cases: every level's bits with portable's on every length and algorithm, nothing
  past a batch or in a column not moved, in place; each algorithm against the integrated true rotation for turns 0 to 400
  within 1e-12 and attitude within 1e-12; the standard's R1, R2 and DR restated; composition and reversal; Euler both ways;
  blend ends, derivative, zero span. win-debug, linux-gcc-debug, linux-gcc-asan; compiled by linux-clang-tsan.
- Forced-level arms on linux-gcc-debug, avx512 under SDE skx and icx. NEON: aarch64 clang syntax check only.
- `crd-hesap-dead-reckoning-peer-tests` (linux-gcc-public-checks, KDIS 97f8e86a, Open-DIS's closed forms in long double):
  [figures](../bench/2026-11-07-hesap-net-dead-reckoning-vs-kdis.md). Open-DIS's Java is read, not run: its expressions are
  restated.
- `crd-fuzz-hesap-motion` gains four ops, 16 new seeds: 863,982 libFuzzer executions clean. The first run's one finding was
  the target's own normalisation of a tiny quaternion.
- 20 teeth, all caught, without the level cases. One first survived (a lane in series taking a neighbour's closed form), so
  a case now holds each entity in a mixed batch to its answer alone.
- [Board](../bench/2026-11-07-hesap-net-dead-reckoning-vs-kdis.md): 2.1x to 15.2x KDIS on every algorithm and level.
- clang-tidy clean on win-debug's files, and on the peer test and board on the peer lane. Tidy did not judge
  `dead_reckoning_peers.cpp`, the KDIS wrapper: it reads KDIS's own `.clang-tidy` and stops on an unknown key there.

## Next

NET-DEBUG's loss and delay distributions on Philox; RaptorQ per RFC 6330; FlexFEC and RED against libwebrtc's reader, writer and
splitter.
