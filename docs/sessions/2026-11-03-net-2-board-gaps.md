# 2026-11-03: NET.2, crd-crypto's gaps to direct AWS-LC (Track C batch 82)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Contract: [P9](../design/network-foundation.md#provider).
Board: [rerun](../bench/2026-11-03-net-2-crypto-boards.md#rerun-after-the-gap-fixes).

CI first: nothing new. Push run 41579304146 (937cb283) failed linux-gcc-debug's sse2 pass on "Key store: device keys
by placement" (Catch2's "redirect is not active"): the worker-thread REQUIRE that 0e206133 already removed.

## What landed

- **Buffers method**: the TLS contexts use `TLS_with_buffers_method()`. A credential holds its certificate as DER
  bytes (`SSL_CTX_set_chain_and_key`, the device signer as the private-key method), sessions and tickets keep
  certificates as bytes, and the pin check reads only the leaf's SubjectPublicKeyInfo (`certificate_key`). No
  handshake parses an X.509 object any more.
- **Error scope**: a record path whose provider calls all succeeded says so (`ErrorScope::succeeded()`) and skips the
  look at the provider's thread-local error queue; an asserting build checks there that the queue is empty.
- **Record path**: `take` is one `BIO_read`; `send` reads the write sequence once for a one-record message; the
  message observer returns before the session lookup for a record's header and inner content type.
- Credential comments corrected (the serial number comes from the key identifier, validity from the epoch).

## Evidence

- Board rerun (9 interleaved repetitions): full handshakes 0.97x and 0.98x direct AWS-LC (unresolved at the 2% band,
  both intervals below 1), resumption 1.10x to 1.03x, 256-byte records 1.10x to 1.06x; the rest measured and
  explained in the bench note.
- New test: records, key updates, a partial take and a failed session leave the provider's error queue empty. Teeth:
  a failure path marked succeeded fails it (one assertion counted).
- win-debug `crd-crypto-tests`: 290 passed, 1 skipped. linux-gcc-debug and linux-gcc-public-checks: 288 passed,
  3 skipped; public-checks' 22 crypto CTests pass (OpenSSL TLS interop, MLSpp and OpenMLS, TPM, PKCS #11, Secret
  Service, fuzz corpora, board smoke). LLVM 20.1.8 tidy clean on the changed sources. The win-debug guards pass;
  `crd-check-capability-owners` hit an impossible `re` RuntimeError once (this host's misexecution) and passed on
  rerun.

## Next

- The Keychain lane (macOS runner), the Android Keystore host, the Tamarin or ProVerif model, then NET.2's close.
