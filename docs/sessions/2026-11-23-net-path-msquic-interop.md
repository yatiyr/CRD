# 2026-11-23: NET-PATH, msquic interop, the first board peer

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S50](../design/network-foundation.md#quic-interop)'s msquic
paragraph.

## CI first

- Fuzz host-check 45194420211 (c252049d), net-quic-connection seed 8403, found `84567a0e`: the follow check's `heard`
  clause. On HEAD it fails with c252049d's escape (`can_send`) and passes with cbf5c258's (`can_send_data`, what a PING
  needs), with the engine at c252049d, e4dfd752 and HEAD alike: the `d57f5de5` class, the client's storage down to the
  ACK-only and probe entries. It joins `corpus-quic-connection` (5def12df); the corpus CTest passes.
- CI 45198716949 (c4fae3b7): its one red is win-asan's `crd-project-structure-sync`, a cmake configure of a
  `LANGUAGES NONE` project past its 60 s bound (0.7 s on this PC) while the runner was loaded; not this track's code.

## What landed

- `cmake/pins.json` `msquic` 2.6.4 (commit archive, SHA-256 pinned), option `CRD_BUILD_QUIC_VS_MSQUIC` on
  linux-gcc-public-checks: msquic's static library by its own CMake, `QUIC_TLS_LIB=openssl` over the peers' OpenSSL
  4.0.3, no submodule needed. Its OpenSSL 4 const-name warning stays a warning: [TP-16](../third-party-defects.md#tp-16).
- `msquic_quic_peer.c`: the echo of `quic_interop.py` over msquic's API; the server's credential is the seed's Ed25519
  key and `openssl_self_signed.h`'s certificate as PKCS #12, the client pins the key from the certificate msquic hands
  it (`USE_PORTABLE_CERTIFICATES`); one version offered and accepted.
- `msquic_interop.py` and `crd-net-quic-interop-msquic`: 10 cases, each way, v1, v2, 16 one-byte streams, 4 MiB on
  one stream, and a relay with 2% loss, 1% duplication and reordering jitter.
- `parse_seed` moved into `openssl_self_signed.h` from the ngtcp2 and picoquic peers (no third copy).

## Evidence

- `crd-net-quic-interop-msquic`: 5 of 5 runs pass on linux-gcc-public-checks. A mutant msquic server echoing another
  pattern fails all five Cerid-client cases ("client read a wrong byte").
- The ngtcp2 and picoquic peers rebuilt: `quic_interop.py --only v1` 122 of 122, the picoquic prague loopback cases 11
  of 11. `crd-pins`, `crd-license-manifest`, `crd-check-source-routes` and the repository validators pass.

## Next

- The budget boards: handshakes per CPU-second, bytes per CPU-second, marginal memory per connection and added p99
  latency under 1% loss and reordering (netem in `unshare -Urn`, which works unprivileged in WSL here), Cerid, msquic
  and quiche each against itself, release builds, equal certificate, suite, controller and offload settings.
