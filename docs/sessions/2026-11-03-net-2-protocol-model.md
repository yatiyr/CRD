# 2026-11-03: NET.2, the Tamarin model of the full handshake (Track C batch 83)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Contract: [the protocol model](../design/network-foundation.md#protocol-model).

CI first (dispatch 41589181174, e5ba1b49), two failures this track caused, fixed in 42058d23 and fd75495e:
- android: `crd-check-isa-isolation` failed 60 SSE4.1 instructions. Android's x86_64 ABI requires SSE4.2, POPCNT and
  CMPXCHG16B and the NDK's clang turns them on for every unit, so the guard now judges code against each operating
  system's floor (`OS_FLOOR`, `--os android` from CrdSimd.cmake). Proof: the WSL android tree passes with
  `--os android` and reproduces the 60 failures with `--os linux`; the regenerated CTest carries `--os android`.
  The job's other failure (Catch2's "redirect is already active" in the key store's device test) predates 0e206133.
- linux-gcc-shipping: `(void)std::system(...)` in the ceridc policy test (glibc's warn_unused_result); the test now
  checks that the shell ran.

## What landed

- **Pin**: `tools/tamarin-linux-x64`, Tamarin 1.12.0 and Maude 3.5.1, two release archives without a top directory
  (`pins.tree_members` takes an empty root); each SHA-256 matched GitHub's published asset digest.
  `scripts/install-tamarin.py` requires `tamarin-prover --version` to report Tamarin 1.12.0 and accept Maude 3.5.1.
- **Model**: `tests/foundation/crypto/model/tls_handshake.spthy`: the hybrid group and its classical companion,
  pinned keys with mutual authentication, ephemeral breaks split into classical (X25519) and post-quantum (ML-KEM).
- **Check**: `scripts/check-protocol-model.py` proves every model once and fails on a wellformedness warning, an
  incomplete or falsified lemma, a lemma added or removed without its list, or a cited design anchor that is gone.
  The `crd-crypto-protocol-model` CTest (CRD_BUILD_PROTOCOL_MODEL, linux-gcc-public-checks; the GitHub step and the
  own CI's `Lane.tamarin`).

## Evidence

- WSL, the pinned install: 7 of 7 lemmas verified in 79 s: `handshake_completes` (16 steps), `no_downgrade` (11),
  `client_authenticates_server` (90, injective), `server_authenticates_client` (71, injective), `handshake_secret`
  (165), `client_traffic_secret` (197), `server_traffic_secret` (1,764).
- Teeth: a client that skips the server's CertificateVerify falsifies `client_authenticates_server` (30 steps); both
  ends leaving the ML-KEM half out of the handshake secret falsifies `handshake_secret` (18 steps, a classical break
  alone). A CertificateVerify without the ClientHello did not converge to its downgrade trace within 400 s.
- Tamarin's full proofs need a heap bound: a diverging search reached 14 GiB and once took the WSL service down
  (E_UNEXPECTED, `wsl --shutdown` restored it); the checker runs with `+RTS -N4 -M8g`.
- `crd-crypto-protocol-model` passes in a configured linux-gcc-public-checks tree (WSL, 77.9 s).
- Unit tests: the installer (two rootless archives in one prefix, both versions checked) and the checker's verdicts;
  test-repository-tools (349), test-ci-platform (102) and test-ci-orchestrator (161) pass; every validator passes.

## Not converged, next

The traffic epochs after the handshake (key updates, S7) did not converge: with linear or persistent epoch facts,
with or without a two-update bound, the secrecy and executability lemmas exhausted 4 to 8 GiB of heap. They stay out
of the committed model. Next: the epoch chain (helper lemmas or an oracle), resumption and early data with the
single-use ticket filter (S9, S17), address validation tokens (S13), then the binding by state learning.
