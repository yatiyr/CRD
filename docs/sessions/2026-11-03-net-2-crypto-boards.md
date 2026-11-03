# 2026-11-03: NET.2, the crypto boards against AWS-LC, BoringSSL and OpenSSL (Track C batch 81)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Contract: [P9](../design/network-foundation.md#provider).
Board: [2026-11-03-net-2-crypto-boards](../bench/2026-11-03-net-2-crypto-boards.md).

CI first: dispatch 41574979037 (26c32908) failed two lanes on this track's key store tests, both fixed here:
- linux-clang-tsan: a data race in Catch2. The test device's `sign_digest` ran `REQUIRE` on the signers' threads. It now
  counts contract breaches in an atomic, and its destructor checks the count on the test's own thread.
- linux-gcc-public-checks: `crd-crypto-tpm-swtpm` aborted on Catch2's "redirect is already active". Same cause.
- Also found locally: the Secret Service test failed about once in 30 runs. gnome-keyring 46 sometimes answers
  `SearchItems` with `UnknownMethod` while other clients' calls run. The protector now resends a call once (a copy)
  after `UnknownMethod`; a second such answer still means Unavailable. 240 repeated runs passed.
- Its android boot fault was fixed by e608e642.

## What landed

- **BoringSSL pin**: release 0.20261020.0 as its commit archive (d3bf4795; unpacks to the same files as the release's
  own archive, whose SHA-256 matches GitHub's digest), built by its own CMake from pregenerated sources, prefixed
  `crd_bssl` (`CRD_BUILD_CRYPTO_VS_PEERS`, now on in `linux-gcc-public-checks`).
- **crd-crypto-board** (`tests/foundation/crypto/board/`): crd-crypto, direct AWS-LC, BoringSSL and OpenSSL 4.0 in one
  program. `board_peer.c` is compiled once per library against its own headers under the engine's warning set; 33
  cases; cross-library checks before timing; `--quick` is the `crd-crypto-board-smoke` CTest; output for ab-board.py.
- **crd-crypto fixes the board found**: `ErrorScope` clears the provider's error queue only when it holds an error
  (80 ns a record); the pin check reads the leaf the provider already parsed (`SSL_get0_peer_certificate`) instead
  of parsing it again; `compress_certificate` builds no plain message for a context that compresses nothing or for a
  certificate above `kTlsMaxCertificateBytes` (GCC 13's link-time `-Wnull-dereference` refused the old form in the
  board's release link).

## Evidence

- Release board, 9 interleaved repetitions on one P-core: crd-crypto beats BoringSSL on every handshake (1.33x to
  1.43x) and every public-key primitive but ECDSA verification (level), and OpenSSL 4.0 nearly everywhere. Gaps named
  on the board: AES-GCM against BoringSSL (the provider's: no VAES-AVX2 path in AWS-LC 5.12.0), resumption and 256-byte
  records 1.10x against direct AWS-LC (crd-crypto's own).
- linux-clang-tsan: the device-key case three times, no report. linux-gcc-public-checks: 9 crypto CTests pass (the
  smoke test, both interop tests, coexistence, MLS, TPM, PKCS #11, Secret Service); `crd-crypto-tests` 287 passed,
  3 device cases skipped. linux-gcc-release (LTO): `crd-crypto-tests` all pass. win-debug: `crd-crypto-tests` 289
  passed, 1 skipped (1,449,214 assertions); `[key-store]` again after the device fix.

## Next

- Close crd-crypto's two gaps to direct AWS-LC (resumption's ticket bytes, the per-record calls), then the Keychain
  lane, the Android Keystore host and the Tamarin or ProVerif model.
