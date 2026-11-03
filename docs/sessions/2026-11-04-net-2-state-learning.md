# 2026-11-04: NET.2, protocol state learning and the Keystore app's emulator run (Track C batch 90)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Design: [state learning](../design/network-foundation.md#state-learning).

## CI first

Dispatch 41717350998 (1bb5ab7c): both Repository checks jobs and the android job's crd-license-manifest and
crd-repository-tools failed on one cause from batch 89: three new pins without a re-rendered
docs/generated/dependency-licenses.md. 8a66ef13 re-renders it. fddede9a makes check-repository.py render it again
(`license_manifest_errors`, a unit test), so a local validator run now catches a pin added without a re-render.

## Android Keystore in the app: proven

The same job's `crd-crypto-keystore-app` passed on the API 31 emulator: the `[android-keystore]` case ran in the APK's
own Java VM, 177 assertions passed (evidence bundle, LastTest.log).

## What landed

- **crd-crypto-tls-sul** (tests/foundation/crypto/state/tls_sul.cpp): the system under learning, with 11 inputs (a
  queue of the peer's flights: deliver, drop, replay; each end's data, update, update_req and close). The subject's
  update bound is 3.
- **scripts/check-state-machine.py**: L* for Mealy machines with Rivest and Schapire's counterexample analysis, a trie
  cache, a W-method oracle (one extra state) plus 2000 seeded random words, and a determinism recheck. It compares the
  result with the reviewed machine (shortest distinguishing word) and runs five property monitors from the lemmas
  (records_authentic, handshake_in_order, terminal, update_bound, requests_answered).
- **Reviewed machines:** tls_server.mealy (94 states) and tls_client.mealy (77 states). CTest
  `crd-crypto-state-learning` runs on every desktop lane.
- **A finding, a contract fix:** the first update_bound monitor flagged an answer to a request that reached the bound.
  Design S7 says this is the intended behaviour: the last update is kept for the peer's request, and the session closes
  after it. The tls.hpp header said "closes instead". The header now states both cases, and the monitor follows S7.

## Evidence

- WSL linux-gcc-debug: the SUL builds without warnings and clang-tidy (LLVM 20) is clean on it. Learning: tls-client
  145782 queries in 51 s, tls-server 177779 in 60 s; PASS (both machines equal the reviewed ones, every property
  holds, the determinism recheck is clean).
- Teeth: tls.cpp's read() closing only past the bound (`>` for `>=`). The learned server then has 99 states, fails
  update_bound ("open after the update that reached its bound") and differs from the reviewed machine on
  `deliver deliver p_update_req s_update s_update deliver` (ok.E.EkKu where the reviewed machine has ok.C.CkKu).
  After the restore and a lane rebuild, the run passes again.
- test-repository-tools.py: random machines learned exactly, the combination lock (hidden with one extra state, found
  with four), nondeterminism and malformed files refused, every property's violation with its witness; every
  validator passes.

## Next

The Keychain lane on GitHub's macOS runners; PIV over ykcs11 with a fake module for every refusal path; the hosted
complete-tier run with every NET.2 commit (win-arm64-debug's P3 assembly in it).
