# NET.2: certificate compression on the TLS sessions

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 40; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [TLS sessions](../design/network-foundation.md#tls-sessions),
decision [S10](../design/network-foundation.md#certificate-compression).

## The user's decisions this batch acts on

- 2026-11-01, about 01:25: "approve the downloads we need to do everything properly, I will contect a lawyer later! DO
  NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD
  STANDARD AND ELITE". The only downloads were the RFC 8879 text and IANA's TLS extension registry file (standards
  register reading, below).
- 2026-11-01, about 14:50: "I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!" This batch takes the
  first clause of NET.2's Remaining: certificate compression (RFC 8879).
- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!"

## CI first

At the start, dispatch run 41319531955 (3cb696ef) had ended "failure" with every job cancelled before it ran, push run
41333397427 (59f64219) was pending and complete-tier run 41323535550 (54f062f2) was queued. That run's first jobs
finished during the batch, and two of its failures came from this track's earlier commits; both were already fixed on
main when this batch rebased:

- win-public-checks, win-asan and win-debug-scalar: MSVC C4389 (signed/unsigned `==`, an error under `/WX`) in
  `test_tls.cpp`'s `is_grease`, where `u16` promotes to `int`; fixed by 9ef40e7e. This batch's new code had one more
  such comparison (a `u8` against `0xA5U` in the decoder test), now compared as `u8`.
- win-vs, win-arm64-debug and the Windows repository checks: `crd-module-selection` expected the full configuration's
  packages without aws-lc and wycheproof; fixed by 47474b0b. This batch's new crypto dependencies (compression, and
  zstd for the OpenSSL peer) add no package to the full configuration.
- win-arm64-debug's other failure, `crd-ci-platform`'s generated table, is the CI platform's, not this track's.

<a id="rfc-8879"></a>
## Reading: RFC 8879 and IANA's registry

Read 2026-11-01 from the official sources:

| File | Source | Bytes | SHA-256 |
|---|---|---|---|
| `rfc8879.txt` | https://www.rfc-editor.org/rfc/rfc8879.txt | 15,350 | `8e30b047e20762ea3de13fb3487d93ee416712b62cab2993900368438c8d20db` |
| `rfc8879.json` | https://www.rfc-editor.org/rfc/rfc8879.json | 895 | `f30eb5b0a337363e467d711323c2468f360fe234d470474d9c83eb7579c9eea7` |
| `tls-extensiontype-values.xml` | https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xml | 50,444 | `90df531dd7dc03dbff3490badde4b863f4bd927990dbb6062301e837df7a443d` |

- RFC 8879 is a Proposed Standard of December 2020, neither obsoleted nor updated.
- `compress_certificate` is extension 27, sent in the ClientHello and the CertificateRequest, with "Recommended" Y.
- IANA's "TLS Certificate Compression Algorithm IDs" registry has no Recommended column. It lists zlib 1, brotli 2
  and zstd 3, and reserves 0.
- What the code follows:
  - the algorithm is one the peer listed;
  - the uncompressed length is checked after decoding and before the Certificate is processed;
  - the decoded Certificate is processed as if it had arrived plain;
  - the same size limit applies with and without compression (section 5);
  - compression changes record lengths, so a padding scheme must disable it (section 5): an empty
    `certificate_compression` list does that.
- Section 4 names `bad_certificate` for a form that does not decode. AWS-LC sends `decode_error`. The handshake fails
  either way; the design records the difference.

The standards register's `tls.certificate-compression` now cites this reading, records zlib and zstd as verified
codepoints of the new registry, and is implemented. `register-review.py` reads the registry from the same IANA file in
later reviews, and `gen-register-ids.py` writes `iana::kCertCompressionZlib` and `kCertCompressionZstd`.

## What the provider does

AWS-LC 5.12.0's libssl links no compression library. Each `SSL_CTX` registers algorithms with a compressor, a
decompressor or both:

- A client offers the algorithms it can decode.
- A server compresses with its first registered algorithm that the client offered. Once that is negotiated it always
  sends the compressed form, with no way to fall back to the plain message.
- Only a client accepts a CompressedCertificate. The server never asks for one in its CertificateRequest.
- A claimed length above `max_cert_list` is refused with `illegal_parameter`. A decoder failure or a wrong length is
  refused with `decode_error`.

## What landed (decision S10)

- `TlsConfig::certificate_compression` (zstd and zlib by default, in a client's order; empty for none) and
  `kTlsCertificateCompression`. Any other value, or a value listed twice, is `BadConfig`.
- A server makes its Certificate message once at `init()` and compresses it with each algorithm it may use (zstd at
  level 22, the engine's own zlib). It keeps a form only when the CompressedCertificate is smaller than the plain
  message, and registers the kept forms smallest first. The forms live in the provider context's ex_data and are copied
  into each handshake, so no handshake compresses anything. The copy checks that the provider asks for exactly the
  context's Certificate.
- A client registers its decoders. `kTlsMaxCertificateBytes` (64 KiB) is `max_cert_list` for every context. A zlib
  claim that DEFLATE's 1032:1 cannot reach is refused before a buffer is reserved, and the buffer has exactly the
  claimed length. A first draft also bounded zstd claims by zstd's 128 KiB blocks; a mutation survived, because one
  3-byte block header already reaches the 64 KiB limit, so that dead check was removed.
- `TlsSession::certificate_compression()` reports the algorithm sent or received. `detail::tls_decompress_certificate`
  is the decoder, exposed for the tests and the fuzz target.
- crd-crypto links crd-compression privately, the module graph declares it, and the codecs stay the only compression
  owner (OWN.1).
- **Defect fixed on the way:** `TlsCredential::init` failed with `ProviderFailure` for any DNS name over 64 characters,
  because the provider refuses a common name above RFC 5280's ub-common-name. A credential now carries the name in the
  subjectAltName. The common name repeats it only when it fits. A longer name leaves the subject empty and marks the
  subjectAltName critical (RFC 5280 section 4.1.2.6). Names of 63, 65 and 253 bytes are tested, and a 122-byte one
  handshakes in-process and with OpenSSL.

## Measurements

The server's first flight (ServerHello to Finished), from one server context, with the client offering both
algorithms against offering none. Three runs each; ML-DSA signatures are hedged and ECDSA signatures vary in length,
so their compressed flights vary by a few bytes:

| Server key | Plain flight | Compressed flight | Algorithm |
|---|---|---|---|
| Ed25519 | 1,651 | 1,612 | zlib |
| ECDSA P-256 | 1,722 to 1,723 | 1,678 to 1,680 | zlib |
| ECDSA P-384 | 1,816 to 1,817 | 1,781 | zlib |
| ML-DSA-44 | 7,668 | 7,599 to 7,615 | zstd |
| ML-DSA-65 | 10,086 | 10,027 to 10,044 | zstd |
| ML-DSA-87 | 13,362 | 13,297 to 13,315 | zstd |

A pinned carrier saves 35 to 69 bytes. Its key and signature are random, so only the names and the DER structure
compress. The engine's fixed-Huffman zlib beats zstd at level 22 on the small classical certificates, where zstd's frame
and table headers cost more than they save. Compression therefore does not bring a post-quantum server flight under
QUIC's amplification limit (three times the client's bytes). Address-validation tokens and resumption do that, and are
measured in round trips with the QUIC integration, the next clause.

## Evidence (cloud machine cerid-track-c, linux-gcc-debug)

- `crd-crypto-tests`: 72 cases, 98,204 assertions, all passing. The `[tls]` cases are 30, four of them new, plus the
  configuration refusals and the credential names:
  - **Forms and sizes:** the table above, for every signature algorithm, with the offer read off the ClientHello.
  - **Choice:** the smallest form both ends allow; zlib alone, zstd alone, none either way and no common algorithm; a
    resumption sends nothing compressed.
  - **Decoder:** exact length only; a claim one byte off either way, a cut frame, over the bound and under four bytes
    refused; a zlib Adler-32 change refused; an unreachable zlib claim refused with the output untouched; two bytes
    are no zstd frame; brotli, 0 and 4 decode nothing.
  - **A hostile server of the provider's own stack:** a valid stored-block zlib form larger than the message is
    accepted. Forms of another length, bytes that are no zstd frame and a wrong Adler-32 fail the client with
    `decode_error` and no peer key. A client that decodes only zstd is sent the plain Certificate, and one that
    decodes only zlib gets an ML-DSA-65 server's Certificate plain (the engine's zlib does not shrink it).
- `crd-fuzz-crypto-tls`: a new input mode feeds compressed Certificates to the decoder. The oracles restate the claim
  bounds, a refused claim's untouched output, zlib's Adler-32 and determinism. Seven new seeds (zstd and zlib, each
  claiming its length, one byte more and far beyond the bound, and a brotli claim); the corpus of 20 replays clean.
- `crd-crypto-tls-interop-openssl`: see the interoperability section below.
- Nine source mutations, each caught by the `[tls]` cases (the cap left at the provider's default by the interop
  test):
  - the zlib ratio bound dropped;
  - claims under four bytes accepted;
  - forms larger than the message kept, or not ordered by size;
  - a decode buffer one byte long;
  - the algorithm not reported;
  - an algorithm listed twice accepted;
  - long names put in the common name;
  - `max_cert_list` left at the provider's 100 KiB.
- LLVM 20.1.0 strict analysis of `tls.cpp`, `test_tls.cpp`, `fuzz_tls.cpp` and `cerid_tls_peer.cpp` is clean; its
  first run found two narrowing iterator offsets and three constant names, fixed.
- The linux-gcc-release preset (link-time optimisation): `crd-crypto-tests` passes (72 cases, 98,259 assertions) and
  the TLS corpus with the new seeds replays clean, with no warning.
- The relocatable package of the crypto module (`CRD_MODULES=crypto`, Release) now installs `libcrd-compression.a`
  and `libzstd.a` beside `libcrd-crypto.a`. A downstream project configured against the moved prefix builds a TLS
  server context with an Ed25519 credential, which compresses its Certificate through the packaged codecs.
- Order of the runs: the 18 scoped CTests (crypto, TLS, interop and the guards) ran at the first complete draft and
  passed except `crd-check-registers`, which ran before this note existed (the guard passes with it), and the interop
  test, whose OpenSSL peer could not then make a certificate named past 64 characters. The interop test passed 34 of
  34 once the peer carried its name in a subjectAltName. The last two fixes (the dead zstd bound and the
  strict-analysis findings) were then proven by the debug `[tls]` cases, the mutations and the whole release suite.
- The cloud machine ran from about 17:57 to 18:43 UTC, and twice for a few minutes after to read the seven new seeds
  back; it is stopped.

## Interoperability (OpenSSL 3.5.9)

The OpenSSL peer is now built with the pinned zstd and the system's zlib (`enable-zstd zlib`). It reports the
CompressedCertificate it sent and received, installs a compressed certificate from a file, dumps the form it made, and
carries its name in a subjectAltName, with a common name only up to 64 characters, as a Cerid credential does.

`crd-crypto-tls-interop-openssl`: 34 of 34 cases, ten of them new:

- a Cerid client decoding an OpenSSL server's zstd and zlib certificates, and an OpenSSL client decoding a Cerid
  server's, each end reporting the same algorithm and each offer read off the ClientHello;
- OpenSSL's real zlib inflates the engine's own fixed-Huffman DEFLATE, and the engine's inflate decodes OpenSSL's
  dynamic-Huffman zlib;
- an OpenSSL client that decodes none, a Cerid client that decodes none, and a Cerid client decoding zstd alone
  against a zlib server, each sent the plain certificate;
- an OpenSSL client taking a Cerid credential named with 122 bytes (an empty subject and a critical subjectAltName);
- OpenSSL servers installing hostile forms: the honest dumped form as a control, a claim one byte off either way
  (`decode_error`), claims of 65,537 bytes and 1 MiB (`illegal_parameter`, the provider's bound) and a 9-byte zlib
  claiming 3,097 bytes (`decode_error` before decoding), with no peer key taken from a refused certificate;
- 24 seeded single-bit corruptions of real zstd and zlib forms: the client either fails with `decode_error`,
  `bad_certificate` or `illegal_parameter`, or establishes with the pinned key, and a zlib form it accepts still
  decodes under Python's zlib.
