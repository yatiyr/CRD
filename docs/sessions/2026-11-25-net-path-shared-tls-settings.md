# 2026-11-25: NET-PATH, shared TLS settings and no stored parameter copies

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: the memory contract under
[the connection](../design/network-foundation.md). Follows [the send key](2026-11-24-net-path-send-key.md).
Ran on this PC (WSL2, and MSVC win-debug with LLVM 22 clang-tidy on Windows).

**CI first:** nothing new to fix. CI 45466383581's reds are the GCC LTO null-dereference (fixed in c0dfe5a8) and the
`min_u32` (fixed in 80411130); its Windows jobs are still running. Fuzz host-check 45471477574 (4ae23931) passed.

## What landed

- `TlsSettings` (pins, server name, ALPN list, ticket keys; 1,336 bytes) lives once in the provider context's
  ex_data, which frees it and zeroes its ticket keys. Each session's connection holds that context, so a context may
  still be cleared or initialized again while sessions live. A session keeps a `TlsSessionSettings` of 84 bytes:
  its policy's peer signatures, its ticket binding and a records server's early data context. Groups and AEADs are
  narrowed on the stack. `QuicHandshake` no longer keeps its context's binding.
- `QuicConnection`: the stored configuration's parameters are filled in place as this side's own (no `m_local`).
  A client's 0-RTT parameters are read again from the handshake's ticket bytes (no `m_remembered`); only their
  `max_udp_payload_size` stays, for the send path.
- Sizes: `QuicConnection` 15,200 to 13,152 bytes; `QuicHandshake` 640.

## Evidence

- New case: a server context re-initialized under another ticket key, then both contexts cleared mid-handshake. The
  live session still seals its ticket under the first key, and a server under that key resumes it.
- linux-gcc-debug: `crd-crypto-tests` 314 cases and `crd-net-tests` 1,202 cases pass; 19 guard CTests pass
  (`crd-check-environment-reads` segfaulted once and passed on rerun, this host's known misexecution).
- ASan: all `crd-crypto-tests` and the connection, handshake, memory and 0-RTT net cases pass with no report.
- LTO with GCC 13.3 (release, relwithdebinfo, shipping): tests, fuzz targets, peers and boards link clean. Release
  `crd-crypto-tests` and `crd-net-tests` pass.
- Interop (public-checks, 15 of 15): OpenSSL TLS, MLSpp, OpenMLS, ngtcp2, nghttp3, picoquic, msquic, quiche,
  quinn, webtransport-go, the qmux crate, QIFs, HTTP/2 and HPACK.
- libFuzzer, 120 s each, no artifact: crypto-tls 100,529 runs, crypto-quic 4,073,024, net-quic-connection 5,304.
  The three corpora replay.
- MSVC win-debug: both suites build; `[tls],[quic]` crypto and connection, memory and handshake net cases pass.
  clang-tidy (LLVM 22.1.8) is clean on the three sources touched.
- Boards at this engine against af8cbcc9's, 5 interleaved rounds, medians:
  - held KiB per connection: server 25.9 against 27.9, client 26.5 against 28.3 (msquic 35.0 and 58.0);
  - handshakes per CPU-second: 4,455 against 4,339;
  - bulk at 1 MiB windows: 567 against 567 MiB per CPU-second.

## Next

1. CI: the first finished run containing 89e8290c and 5ce9ad3e on the MSVC, clang-cl, ARM64 and TSan lanes, and
   45466383581's Windows jobs.
2. A held server end below msquic's best rounds (16.8 to 24.5 KiB): the three receive keys (2.0 KiB) are the
   largest connection part left. Otherwise the Remaining's bulk cost against msquic.
