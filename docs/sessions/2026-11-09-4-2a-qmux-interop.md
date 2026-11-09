# 2026-11-09 — 4.2a: QMux interoperability with a second implementation

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S52](../design/network-foundation.md#qmux), [S53](../design/network-foundation.md#stream-host).

CI first: run 42539020772 (524fb8bb) failed on win-arm64 only in the `WSAPoll` wait, the defect 0351c2d3 already fixed. No completed run contains 4327df3a yet (42556131353 and Host check
42556514351 are queued), so tidy ran here on the four files nothing had analysed: `tls.cpp`, `qmux.cpp`, `fuzz_qmux_early.cpp`, `qmux_fuzz_fixture.hpp`: clean.

## What landed

- `crd-qmux-peer`, QmuxHost over real TCP; `crd-qmux-crate-peer`, the `qmux` crate 0.6.6 (draft-02, MIT or Apache-2.0) over rustls 0.23.46 and aws-lc-rs, pinned by its committed `Cargo.lock` (tools/qmux-crates).
  `qmux_interop.py` runs the two both ways round (`CRD_BUILD_QMUX_VS_CRATE`, on `linux-gcc-public-checks`).
- **A defect the first run found in crd-net:** a peer's last stream bytes and its CONNECTION_CLOSE in one read reached a Closed connection, and QmuxHost did not serve it, so the application never read bytes that had arrived.
  A connection that ended on what it read or its timer is now served once more, Closed. New test in `test_qmux_host.cpp`, and the fuzz target's oracle holds it; removing the fix fails the test. QUIC is not affected: a peer's close leaves a connection Draining, still served.
- Refactoring so nothing is written twice: `interop_echo.hpp` holds the echo application and the option parsing both peer programs use (`cerid_quic_peer.cpp` is 620 lines shorter), `interop_common.py` the report reading,
  `cmake/CrdCargoPeer.cmake` the Cargo build the three Rust peers share (OpenMLS, RaptorQ and this one).
- Three tidy findings in `test_qmux_host.cpp`, two of them from earlier batches, fixed.

## Evidence

- `crd-net-qmux-interop-crate`: 52 cases on `linux-gcc-public-checks`, five runs of all 52, every case passing: each way, each end closing with a code and a reason, X25519MLKEM768, SecP256r1MLKEM768, X25519, the three AEADs,
  16 MiB on one stream and 32 MiB on eight (far over the windows), 64 streams through 8 credits, unidirectional streams, RESET_STREAM, STOP_SENDING, a mix of all with the server closing, datagrams at and one byte over the limit, resumption with
  0-RTT tickets, QX_PING past a 900 ms idle timeout with the RTT measured on both ends, a 65535-byte max_record_size, a relay cutting the bytes into 1 to 7 at three seeds, a silent path each end's idle timeout ends, and a refused ALPN.
- `crd-net-quic-interop-ngtcp2` after the refactoring: 102 of 102. `crd-net-tests` `[qmux]`, `[qmux-host]`, `[qmux-early]`: all pass.
- `crd-fuzz-net-qmux-host`: corpus of 5 seeds replayed; three libFuzzer runs on linux-clang-fuzz, 18,315 executions, clean (hosted: Host check `net-qmux-host`).
- Teeth: restoring the old serve condition fails the new test; a wrong tag on one direction's unidirectional streams fails the interop on the peer that reads them, both ways.
- 0-RTT is not in this matrix: the crate reads QMux only after the handshake, so a crd-net client has nothing to offer it. It stays proven by crd-net's own peers and fuzzing (S54). Tickets that allow 0-RTT are carried by the resumption case.

## Next

The raw UDP path of ADR-0035, then the browser endpoints.
