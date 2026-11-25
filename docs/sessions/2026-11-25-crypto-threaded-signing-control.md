# 2026-11-25: NET.2, a Cerid-free control for win-asan's threaded ML-DSA signing refusal

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET-PATH](../ROADMAP.md#slice-net-path) batch; the entry is [NET.2](../ROADMAP.md#slice-net.2)'s provider.
Follows [the register entry](2026-11-11-crypto-threaded-signing-register.md). Operator order: step (1), a hosted
win-asan leg that runs the same threaded signing through AWS-LC alone.

**CI first:** CI 45496952342 (a3928095) failed only win-asan shard 1, on
`Provider: one verifier and one key serve several threads`. It is the fourth sighting: one thread's first signing was
refused after 11,266 ms and a second signing at once succeeded (case 11.6 s). No other job failed; win-release,
win-shipping and win-asan shard 2 were still running.

## What landed

- `tests/foundation/crypto/control/mldsa_threads_control.cpp`, target `crd-mldsa-threads-control` (EXCLUDE_FROM_ALL,
  links only `crypto` and the warning set). It makes the case's provider calls in the case's order: `RAND_bytes` seed,
  `EVP_PKEY_pqdsa_new_raw_private_key`, four threads × 8 × (`EVP_DigestSignInit` with no digest, `EVP_DigestSign`, verify
  through a fresh public key). It prints each thread's first-signing time, the refused call, the error queue and the
  second signing's answer. crd-crypto installs nothing into AWS-LC (grep: no allocator, RNG or thread hooks, no init).
- `scripts/provider-host-check.py`: per round, both arms in fresh processes (every sighting was a first signing),
  once alone and then `--parallel` at once with the arms alternating. Each run is timed to its own exit, and runs
  over 5 s count as stalls. It exits 1 on any event. Unit test in `test-repository-tools.py`.
- `host-check.yml`: experiment `provider-threads`, two `windows-2025` legs on `win-asan`, input `provider_rounds`
  (150). `test-instruments.md` says how to read the arms. The register entry records the fourth sighting.
- Local check (win-asan, MSVC, this PC; it proves the tool only): the control built warning-clean and ran clean
  (first signings 4-24 ms).

## Next

Dispatch `provider-threads` on yatiyr/CRD once GitHub's main holds this commit, then audit it. Control event: admit a
TP entry and make the gate express an intermittent registered failure that retires when the control stops
reproducing. Events in the case only, with the control clean: crd-crypto's cause, fixed with teeth. Both clean: more
exposure, no verdict. Then NET-PATH's `min_mtu` trace ([LEO case](2026-11-25-net-path-leo-case-trace.md), Next 1).
