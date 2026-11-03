# 2026-11-03: NET.2, the key update model and four CI fixes (Track C batch 84)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Contract: [the protocol model](../design/network-foundation.md#protocol-model).

CI first (dispatch 41589181174 at e5ba1b49, push run 41619527221 at 8d2485b5):
- win-clang-cl-shipping: `poisoning existing macro` in tls.cpp. BORINGSSL_PREFIX makes the two key-log calls
  renaming macros; GCC fails the same way (a probe reproduced it), so linux-gcc-shipping would have too. f95535b0 and
  fa5e599d undefine both macros before every compiler's pragma; the shipping lane's crd-crypto and ceridc build in
  WSL, and MSVC compiles tls.cpp with `CRD_TLS_KEY_LOG=0`.
- linux-gcc-relwithdebinfo: `-Wnull-dereference` in ceridc's LTO link at profile.cpp's `*node->as_array()`.
  8d7b6a2b iterates the already checked nodes; ceridc links on linux-gcc-relwithdebinfo and linux-gcc-shipping.
- win-clang-cl-shipping, `crd-ci-orchestrator-e2e`: KeyError 'state' when the build poll got another answer (CI-1's
  test, not this track's code). 319d7f2a fails the poll with the status, the body and the coordinator's log (the
  test kept it in os.devnull), so a recurrence names its cause; the e2e passes locally.
- linux-gcc-release's `std::system` failure was already fixed by fd75495e; android's ISA failure by 42058d23.
- This host misexecuted twice in GCC (a segfault, then `double free` heap corruption in fft_kernels.cpp); each step
  was rerun once at fewer jobs and passed, recorded per the host-misexecution lesson.

## What landed

- `tests/foundation/crypto/model/tls_key_update.spthy` (S7): one direction's key chain from a traffic secret the
  handshake model proves secret, records at their sequence number (natural numbers), KeyUpdates under the old key,
  either end's current key revealed at any time (one reveal rule per key shape, so no deconstruction stays open).
- The checker lists its 8 lemmas; the design's protocol-model section records it.
- test_tls.cpp: a genuine record replayed, delivered out of order, or replayed from before a KeyUpdate fails with
  bad_record_mac (the property the model leaves to the code).

## Evidence

- Proven for chains of up to eight keys: forward security of the updates (`epoch_secrecy`, 3216 steps),
  `record_authentic` and `update_authentic` (accepted only as sent unless that key or an earlier one was revealed),
  `update_does_not_heal` (no post-compromise security), executability with two updates; 63 s for the model.
- `crd-crypto-protocol-model` passes both models in linux-gcc-public-checks (WSL, 142 s; timeout 1800 s).
- Teeth: a KeyUpdate sent in the clear falsifies `update_authentic` (4 steps); a reveal that also exposes the old
  key falsifies `epoch_secrecy` (26 steps).
- Not converged: the unbounded chain's induction (six formulations, typed reveals and a knowledge-first tactic
  included) and a record-once lemma over the counters; the replay test above holds the latter.

## Next

The hosted audit of da54cf1a (Tamarin on the runner image); the unbounded epoch chain; resumption and early data
with the ticket filter (S9, S17); address validation tokens (S13); the binding by state learning.
