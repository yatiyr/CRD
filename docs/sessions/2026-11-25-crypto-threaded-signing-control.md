# 2026-11-25: NET.2, win-asan's threaded ML-DSA signing refusal reproduced and split

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET-PATH](../ROADMAP.md#slice-net-path) batch; the entry is [NET.2](../ROADMAP.md#slice-net.2)'s provider.
Follows [the register entry](2026-11-11-crypto-threaded-signing-register.md). Operator order: step (1), a hosted
win-asan leg that runs the same threaded signing through AWS-LC alone.

**CI first:** CI 45496952342 (a3928095) failed only win-asan shard 1, on
`Provider: one verifier and one key serve several threads`. It is the fourth sighting, on a Xeon 6973P-C: one
thread's first signing was refused after 11,266 ms, and a second signing at once succeeded.

## What landed

- `tests/foundation/crypto/control/` holds three EXCLUDE_FROM_ALL programs, none of them a CTest:
  - `crd-mldsa-threads-control` links only `crypto`. It runs the case's provider calls in the case's order. Its modes
    are `gate` (the four first signings released together) and `storage` (the case's stack digest context and heap
    signature).
  - `crd-mldsa-threads-cerid` runs the case through `SigningKey` with no Catch2. Its modes are `gate`, `warm` (one
    signing on the main thread first) and `std-buffer` (signatures outside the engine's allocator).
  - `threads_harness.hpp` holds the shape both share.
  - crd-crypto installs nothing into AWS-LC: a grep finds no allocator, RNG or thread hook and no init call.
- `scripts/provider-host-check.py` runs every arm in fresh processes. The case runs directly and through CTest. An
  optional contended phase is available. It has a unit test.
- `host-check.yml`: experiment `provider-threads`, six `windows-2025` win-asan legs. Each leg reads its CPU first and
  stops at once on one that is not Intel's, unless `provider_cpu` is `any`.

## Evidence

- **Host check 45521504985** (150 rounds, alone and four at once): the case failed on both Intel machines.
  - Xeon Platinum 8573C: 89/150 alone, 202/300 contended.
  - Xeon 6973P-C: 91/150 alone, 212/300 contended.
  - The Cerid-free control was clean in all 900 runs; its slowest run took 1.23 s.
  - All 828 failed assertions are iteration 0. All 827 refused signings took 9.5-22.4 s, and each second signing at once
    succeeded. One thread signed, but its signature failed verification twice.
- **Host check 45529742859** (40 rounds of all ten arms): it drew AMD EPYC 7763 and 9V45. Every arm was clean, the case
  included (its slowest run 1.03 s).
- **Host check 45531196275** (six legs): every leg drew AMD EPYC (7763, 9V74 four times, 9V45), and every arm was
  clean.
- The fault follows Intel Xeon runners. The lane agrees: the case passed on the AMD EPYC win-asan shards of
  45466383581, 45358207939 and 45496952342 (7763, 9V74) and failed on that run's Xeon 6973P-C shard.
- **Host check 45542990858** (six CPU-gated legs; five drew AMD and stopped): leg 2 drew a Xeon 6973P-C.
  - Every control arm was clean (0/160: plain, gate, storage, gate+storage).
  - Every arm that links crd-crypto failed: case 29/40, case through CTest 25/40, `cerid` 27/40, gated 30/40, warmed
    27/40, std-buffer 26/40.
  - So the cause is not AWS-LC alone, not Catch2 or the test program, not the key's first use and not the engine's
    allocator. No TP entry. Nothing is gated, so win-asan stays red whenever its shard draws an Intel Xeon.
- Then a `raw` mode of `crd-mldsa-threads-cerid` (arm `cerid-raw`): the control's AWS-LC calls, now shared in
  `raw_signing.hpp`, made inside the crd-crypto-linked program.
- **Host check 45547501396** (CPU-gated): four legs drew AMD and stopped. Two drew Intel CPUs.
  - Leg 5, a Xeon 6973P-C: the case failed 27/40, through CTest 32/40. The crd-crypto arms failed 22-31/40.
    `cerid-raw` was clean 40/40, and so were all four control arms (0/160).
  - Leg 4, a Xeon Platinum 8370C (Ice Lake): every arm was clean. So the fault needs the newer Xeons (Emerald and
    Granite Rapids), not every Intel CPU.
- **Cause located:** crd-crypto's own runtime path. It is not that program's process, image or linked libraries,
  because the same binary's raw calls pass. Both paths make the same provider calls in the same order. What only
  crd-crypto does is:
  - generate the seed through `random_bytes` and zero it with `secure_zero`;
  - wrap each operation in `ErrorScope`, which calls `ERR_peek_error` after a successful call;
  - verify through `signature_public_key`.
  No TP entry. Nothing is gated, so win-asan stays red whenever its shard draws one of those Xeons.
- Locally (i9-14900K, MSVC 19.51, also under SDE `-gnr`) every arm is clean. That proves only the tool.

## Next

Split crd-crypto's path one behaviour at a time, as `crd-mldsa-threads-cerid` raw sub-modes run on a CPU-gated
dispatch: `ERR_peek_error` after each call, the key made by `SigningKey::from_seed`, and crd-crypto's verify. Then fix
the cause in crd-crypto, with teeth on the failing Xeons. Then NET-PATH's `min_mtu` trace
([LEO case](2026-11-25-net-path-leo-case-trace.md), Next 1).
