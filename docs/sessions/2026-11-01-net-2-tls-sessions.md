# NET.2: TLS 1.3 sessions and the hybrid groups

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 35; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [TLS sessions](../design/network-foundation.md#tls-sessions),
decisions S1 to S6, and the provider's decision P6.

## The user's decisions this batch acts on

- 2026-11-01, about 01:25: "approve the downloads we need to do everything properly, I will contect a lawyer later! DO
  NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD
  STANDARD AND ELITE". The independent peer, OpenSSL 3.5.9, is a testing download under it (below).
- 2026-11-01, about 14:50: "I wanted you to finish all net rows and you stalled on very currently unnecessary things. Of
  course we think about the laws but it seems you are over complicating everything! I WANT NET DONE PROPERLY AND GOLD
  STANDARD AND WITHOUT ANY DEFERS!" The batch takes NET.2's next clause in the row's order: the hybrid groups proven
  against an independent peer, which needs the TLS stack and so starts the handshake integration.

## CI first

Nothing to fix from this track. Complete-tier runs 41311293265 (dac5d27e), 41298463221 and 41302856153 had no job
started; push run 41315612057 (6094b8e9) was pending; every other push run since was cancelled before it started.

## Download

OpenSSL 3.5.9 (released 2026-09-29, the 3.5 long-term support series), pinned as `openssl` in `cmake/pins.json`:
`https://www.openssl.org/source/openssl-3.5.9.tar.gz`, 53,279,637 bytes, SHA-256 `603f5602...eb859a`, the same digest
as the `.sha256` files published on openssl.org and on the GitHub release; LICENSE.txt read from the archive:
Apache-2.0. Gated by `CRD_BUILD_TLS_VS_OPENSSL` (Linux), built by its own Perl Configure as a separate project.

## What landed

- **The TLS stack.** AWS-LC's libssl is built (`BUILD_LIBSSL ON`) and linked privately into `crd-crypto`, exported
  link-only as Cerid::aws-lc-ssl. AWS-LC's own install rules are skipped (`CMAKE_SKIP_INSTALL_RULES` around the
  package): its export of libssl apart from libcrypto cannot coexist with Cerid's package exporting both.
- **[tls.hpp](../../engine/foundation/crypto/include/crd/crypto/tls.hpp)**: `TlsCredential`, `TlsContext`,
  `TlsSession` (sans-I/O), `tls_aead_preference()` and the group and signature codepoint maps, under decisions S1 to S6
  of the contract. `provider_internal.hpp` now shares the provider's key casts, error scope and a key's signature
  algorithm and public bytes between `provider.cpp` and `tls.cpp`, so the peer key check uses the provider
  interface's own encoding.
- **Facts read from the provider's source.** AWS-LC's TLS 1.3 server picks the AEAD in the client's order, preferring
  ChaCha20-Poly1305 only when the server has no AES instructions; the contract now says so (S3) instead of claiming
  server order, and `tls_aead_preference()` orders a client's list by its CPU. Server group preference is honoured
  (`SSL_OP_CIPHER_SERVER_PREFERENCE`). AWS-LC has no public GREASE switch and no RFC 7250 raw public keys, so pinned
  keys travel in self-signed carriers (S4).
- **P6 met.** The three hybrid groups are in `provider_algorithms()`; `tls.hybrid-kex` is implemented with the unit
  test and the interop driver as evidence. The cerid-package inventory gained them, its five findings and the counsel
  question were restated, and `gen-crypto-inventory.py --review cerid-package` recorded the review (inventory
  `11a3fabe...`).

## Evidence (cloud machine, linux-gcc-debug, GCC 13.3)

- `crd-crypto-tests "[tls]"`: 14 cases, 1,305 assertions. Each group's ClientHello and ServerHello key share is read
  off the wire and checked against its layout through the provider's own primitives: X25519MLKEM768's first 1184
  bytes pass FIPS 203's modulus check and its last 32 are an X25519 key; the SecP groups start with a point on their
  curve, then an encapsulation key (client) or a ciphertext (server). Also: HelloRetryRequest three ways, a hybrid
  share kept beside a classical one, no common group (`handshake_failure`), every signature algorithm, AEAD choice,
  pins both ways, `certificate_required`, `general_error` for an unknown name or client key, case-insensitive names,
  40,000-byte messages, both bounds, `close_notify`, a forged record (`bad_record_mac`), garbage, every configuration
  refusal, a credential outliving its key and contexts cleared under live sessions, and one context on four threads.
- `crd-fuzz-crypto-tls`: 7 deterministic seeds (a real ClientHello whole and in 7-byte pieces, a ServerHello, an
  oversized record, an SSL 2.0 hello, an alert, nothing); oracles: never established, only documented answers,
  failure final, bounded buffers.
- `tls_interop.py` against OpenSSL 3.5.9: 15 of 15 cases. X25519MLKEM768, SecP256r1MLKEM768 and SecP384r1MLKEM1024
  with each library as client and as server; a HelloRetryRequest with each as server; a hybrid group kept beside a
  classical share with each as server; X25519 and P-384; an OpenSSL client with a pinned credential; a Cerid client
  refusing an unpinned OpenSSL key with `bad_certificate`; a Cerid server refusing a TLS 1.2 OpenSSL client with
  `protocol_version`. The driver reads the key share groups and lengths off the wire itself.
- Ten source mutations, each caught: the pin check dropped (by the unit tests and by the interop), the server name
  check dropped, the HelloRetryRequest unnoticed, TLS 1.2 allowed (by the interop), each bound dropped, the client's
  group order winning, two hybrid codepoints swapped, a refusal reported as a plain failure and a server ignoring the
  client credential.
- The scoped CTests (18: the crypto suites and corpora, `crd-crypto-tls-interop-openssl`, the register, capability,
  source-route and include-budget guards, the inventory drift) pass; the build prints no warning; LLVM 20.1.0 strict
  analysis of the changed C++ is clean after renaming four constants, a raw string and two nested conditionals.

## Remaining on NET.2

As the row states, starting with the TLS 1.3 profile's other conformance points and the QUIC handshake integration
over these sessions.
