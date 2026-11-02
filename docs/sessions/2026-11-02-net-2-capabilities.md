# NET.2: identities, capabilities, revocation generations and service links

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 46; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract:
[capabilities, S15](../design/network-foundation.md#capability-tokens).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch takes NET.2's first Remaining clause: identities, signed session-bound
  capability tokens with expiry, attenuation, rotation and revocation generations, and mutual authentication on
  service links.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC: the win-debug
  profile in the worktree and the track's WSL lane (`~/cerid-src-track-c`, `~/cerid-track-c-build`). No cloud machine
  ran (0 hours, 0 USD).

## CI first

Complete-tier run 41342960677 (59a3347e) still has 14 jobs running; three have failed:

- **win-clang-cl-shipping**: `crd-check-isa-isolation` and `crd-check-compile-commands`, both fixed by b37a4177
  (batch 45); the job's log names no third failure.
- **linux-gcc-debug**: the TLS case "a server sends its Certificate compressed when that is smaller" expected zlib for
  an ECDSA certificate and got zstd (a random signature decides which form is smaller); 6e5dbb36 already accepts
  either form for ECDSA, after this run's commit.
- **win-tidy**: `readability-use-anyofallof` in `tests/numerics/hesap-fft/test_fft_kernels.cpp:803`. Fixed here
  with `std::ranges::any_of` over the levels; the tidy helper was clean on the file and both affected cases passed on
  win-debug. The FFT track pushed the same change first (d178d59b), so the rebase dropped this track's copy. The tidy
  job stops at its first failing unit, so later units' findings appear only in the next completed run.

The tidy helper over every crypto source and test of this track also found findings older than this batch, all
fixed: local constants named as statics and a nested conditional in `quic.cpp`, statics named as locals and a loop
`all_of` replaces in `fuzz_quic.cpp`, local constants and a nested conditional in `test_quic_handshake.cpp`, and
arguments the suspicious-call check reads as swapped in `test_quic.cpp`. No complete-tier run was dispatched: the
running one would be cancelled by it.

## What landed

Commits: 964487d5 (the QUIC tidy fixes), d1e2b49f (capabilities) and this record.

- **Capabilities** (`capability.hpp`, decision S15): principals named by their keys; tokens issued by every issuer
  key whose window holds the issue time (a hybrid set, or `UntrustedIssuer`), delegated offline by blocks that only
  narrow and are signed by the previous holder over every byte before them, presented with a proof over the
  session's exporter (Proof) or by the session's mutual authentication (Peer), and verified against the audience,
  the lifetime bound, the chain, the time, peer-only service links, the revocation generation and the policy
  snapshot, cheapest first. Revocation generations are signed, monotonic and dated, adopted as policy generations
  are, and the authority fails closed without a current one.
- **The exporter** (`TlsHandshakeState::export_keying_material`, RFC 9846 section 7.5, over QUIC too) and
  `capability_binding`, each session type under its own private-use label (RFC 5705 section 4).
- **Shared code**: the signature list and the every-key check moved out of `policy.cpp` into `signed_artifact`,
  which policy generations, tokens and revocation generations use; `PolicySignature` is now `KeySignature`; signing
  goes through a `Signer` interface that `SigningKey` implements (the key store's hardware keys will too); and the
  provider exposes `sha256`, which `key_id_of` uses.
- **A design change, recorded in the contract**: the design bound a token to one session. S15 binds it to its
  holder's key and each presentation to its session, so a token survives reconnections and can be delegated, while a
  stolen token or a replayed proof is still useless.

## Evidence

- win-debug: `test_capability.cpp` 14 cases, 8,987 assertions; the crypto suite 136 cases, 591,253 assertions;
  `crd-fuzz-crypto-capability` (21 seeds, each verdict checked as written) and 4,000 random mutations of its corpus
  replayed clean; the QUIC and policy corpora clean.
- linux-gcc-debug: all 140 crypto CTests (the suite, the five fuzz corpora, the register drift check and the OpenSSL
  interop) after the tidy fixes.
- linux-gcc-public-checks: `crd-crypto-tls-interop-openssl` 69 of 69 against OpenSSL 4.0.3, every established
  connection now also comparing each library's exporter and Cerid's world binding, over records and QUIC.
- Two source mutations, each caught: a proof message changed before verification (every Proof verification fails)
  and the grant check dropped from the chain (a widened block verifies as a bad signature instead of `Widened`).
- Sizes: a token with two ranges and ML-DSA-65 and Ed25519 issuer signatures is 3,653 bytes; a delegation by an
  Ed25519 holder adds 201 bytes with one range; an Ed25519 proof is 100 bytes.

## Next

NET.2's Remaining continues with anti-replay windows and idempotent-only 0-RTT. CI first: audit the complete-tier run
41342960677 when it ends and the first hosted run containing this batch (the first MSVC, clang-cl and ARM64 compile of
`capability.cpp`, `signed_artifact.cpp` and the `Signer` interface).
