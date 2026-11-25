# 2026-11-25: NET.2, win-asan's threaded ML-DSA signing refusal reproduced and split

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET-PATH](../ROADMAP.md#slice-net-path) batch; the entry is [NET.2](../ROADMAP.md#slice-net.2)'s provider.
Follows [the register entry](2026-11-11-crypto-threaded-signing-register.md). Operator order: step (1), a hosted
win-asan leg that runs the same threaded signing through AWS-LC alone.

**CI first:** CI 45496952342 (a3928095) failed only win-asan shard 1 on the threaded signing case. That is the fourth
sighting, on a Xeon 6973P-C: one thread's first signing was refused after 11,266 ms, and a second at once succeeded.

## What landed

- `tests/foundation/crypto/control/` has two EXCLUDE_FROM_ALL programs. Neither is a CTest.
  - `crd-mldsa-threads-control` links AWS-LC only. It makes the case's provider calls (`raw_signing.hpp`). Modes:
    `gate`, `storage`.
  - `crd-mldsa-threads-cerid` runs the case through `SigningKey` without Catch2. Modes: `gate`, `warm`,
    `std-buffer`, and `raw`, which makes the control's calls in this program.
- `scripts/provider-host-check.py` runs each arm in fresh processes, the case also through CTest; it has a unit test.
- `host-check.yml` experiment `provider-threads`: six win-asan legs. Each leg reads its CPU first and stops unless
  the CPU is Intel's (`provider_cpu`).

## Evidence (failures out of runs)

- 45521504985, case against plain control:
  - Xeon 8573C: case 89/150 alone, 202/300 four at once.
  - Xeon 6973P-C: case 91/150 alone, 212/300 four at once.
  - Control: 0/900.
  - All 828 faults were the thread's first signing (iteration 0): 827 refusals after 9.5-22.4 s, each followed at once
    by a signing that succeeded, and one signature that did not verify.
- 45529742859 and 45531196275: eight AMD EPYC legs (7763, 9V45, 9V74). Every arm clean. The lane agrees: its EPYC
  win-asan shards pass the case.
- 45542990858, Xeon 6973P-C:
  - Controls 0/160 (plain, gate, storage, both).
  - Case 29/40; through CTest 25/40.
  - `cerid` 27/40, gated 30/40, warmed 27/40, std-buffer 26/40.
- 45547501396:
  - Xeon 6973P-C: crd-crypto arms 22-32/40. `cerid-raw` 0/40 and controls 0/160.
  - Xeon 8370C (Ice Lake): every arm clean.
- **Cause located:** crd-crypto's own runtime path, on Emerald and Granite Rapids Xeons.
  - It is not AWS-LC alone or the test program, not the key's first use, and not the engine's allocator.
  - It is not the process either: the same binary's raw calls pass.
  - What only crd-crypto does is three things: `random_bytes` and `secure_zero` for the seed, `ErrorScope`'s
    `ERR_peek_error` after a successful call, and its verify through `signature_public_key`.
- No TP entry. Nothing is gated, so win-asan stays red whenever its shard draws one of those Xeons.
- Locally (i9-14900K; also SDE `-gnr`) every arm is clean. That proves the tool only.

## Next

Split those three as raw sub-modes of `crd-mldsa-threads-cerid` on a CPU-gated dispatch, then fix the cause in
crd-crypto, with teeth on the failing Xeons. Then NET-PATH's `min_mtu` trace
([LEO case](2026-11-25-net-path-leo-case-trace.md), Next 1).
