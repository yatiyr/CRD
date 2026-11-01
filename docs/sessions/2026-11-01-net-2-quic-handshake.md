# NET.2: the TLS 1.3 handshake over QUIC, and application protocols

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 42; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract:
[the TLS handshake over QUIC](../design/network-foundation.md#quic-handshake), decisions S11 and S12.

## The user's decisions this batch acts on

- 2026-11-01, about 01:25: "approve the downloads we need to do everything properly, I will contect a lawyer later!
  DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD
  STANDARD AND ELITE". Peer libraries and standards texts are testing and standards downloads: the pinned OpenSSL
  3.5.9 archive was fetched again from its official source for this machine's lane (SHA-256 and size as pinned), and
  RFC 7301 and IANA's TLS ExtensionType Values were read (below).
- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch takes NET.2's first Remaining clause: the QUIC handshake integration.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lane, `~/cerid-src-track-c` and `~/cerid-track-c-build`); no cloud machine ran (0 hours, 0 USD).

## CI first

Nothing from this track to fix. Push run 41333397427 (59f64219) had five failed jobs, each already fixed on main: the
repository checks and win-vs failed `crd-module-selection` (fixed by 47474b0b), win-arm64-debug also
`crd-ci-platform` (fixed by df0d359f), and both clang ARM64 lanes `-Wdouble-promotion` in `test_ckir_device_check.cpp`
(fixed by 3cf07ddc). Its win-arm64-debug job ran 2,617 of 2,619 tests clean, the TLS session cases through tickets
and resumption among them (the run is at 59f64219; certificate compression and QUIC packet protection came later).
Push run 41343108236 (fa02943c) and complete-tier run 41342960677 were
still pending.

## What landed

- **`QuicHandshake`** (tls.hpp, tls.cpp; decision S11): the same contexts' TLS 1.3 handshake through the provider's
  QUIC method. CRYPTO data in per level (`provide`) and out per level (`pending`, `take`); each Handshake and
  Application secret turned into `QuicPacketKey` and `QuicHeaderKey` once by `derive_keys` and zeroed; transport
  parameters both ways; alerts with their level for CRYPTO_ERROR (`quic_crypto_error`); no TLS KeyUpdate; tickets
  under a QUIC binding; `receive_limit` (RFC 9000 section 7.5).
- **One handshake state** (`detail::TlsHandshakeState`): the stream session and the QUIC handshake share the provider
  connection, the settings and every callback (pins, names, tickets, certificate compression, the observer), and the
  end of a handshake (`TlsCallbacks::complete`), so nothing is written twice.
- **Application protocols** (decision S12, RFC 7301): `TlsConfig::application_protocols`, server preference,
  `no_application_protocol` both ways, required over QUIC, reported by `application_protocol()`.
- **Registers**: the new `tls.alpn` entry (implemented, RFC 7301, codepoint 16) and `quic.tls`'s codepoint 57 and
  evidence; `gen-register-ids.py` wrote `kTlsExtensionApplicationLayerProtocolNegotiation` (crypto) and
  `kTlsExtensionQuicTransportParameters` (net), and the generated-sources manifest was refreshed.

<a id="rfc-7301"></a>
## Read for this batch

Read 2026-11-01 from the RFC Editor: RFC 7301 (`rfc7301.txt`, 17,439 bytes, SHA-256
ba122aee58871c604c9e98e2c9f81d5042e4056bd497cd41e23ad80fe3123222): section 3.1 (the extension, 16, a non-empty list in
descending preference), section 3.2 (the server selects its most preferred protocol the client advertises, or answers
`no_application_protocol`, 120) and the note that the extension is a property of the connection, not the session.
From IANA, 2026-11-01: TLS ExtensionType Values (`tls-extensiontype-values-1.csv`, 4,853 bytes, SHA-256
653c1162ee6bb22ed89422178a97877ad601fe2e94fda65ced56ce64f9c6658b): application_layer_protocol_negotiation 16 and
quic_transport_parameters 57, both "CH, EE" and recommended. RFC 9001 was read in batch 41
([QUIC protection](2026-11-01-net-2-quic-packet-protection.md)); OpenSSL 3.5's interface is its
`doc/man3/SSL_set_quic_tls_cbs.pod` and `include/openssl/core_dispatch.h` from the pinned archive.

## Evidence (this PC, linux-gcc-debug)

- `crd-crypto-tests "[quic-handshake]"`: 15 cases, 3,905 assertions (the whole suite: 97 cases, 117,224 assertions):
  every group and AEAD and every signature algorithm with mutual authentication, each level's keys opening the other
  end's packets in versions 1 and 2; which level each message and secret arrives at (a server's half-RTT key before
  the client's Finished); a HelloRetryRequest at the Initial level; unpinned keys refused with `bad_certificate` at
  the Handshake level and `general_error` at the Application level (the server already writes there); application
  protocols over QUIC and over a stream; a ClientHello without transport parameters refused with
  `missing_extension`; the flights; tickets under the QUIC binding that never cross transports; a TLS KeyUpdate
  refused with `unexpected_message`; data at levels not read; garbage; the receive limit; refusals; four threads.
- Measured flights (CRYPTO bytes, X25519MLKEM768): ClientHello 1,450 (two Initial datagrams, the transport's to
  send); server flights Ed25519 1,580, ECDSA P-256 1,648 and P-384 1,746, ML-DSA-44 7,580, ML-DSA-65 10,010 and
  ML-DSA-87 13,267; resumed 1,259 after a 1,704-byte ClientHello. Against the amplification limit of two datagrams
  (7,200 bytes, 8,832 at a 1,500-byte path): classical flights and resumptions fit, ML-DSA ones do not, which is what
  the address-validation tokens (the next slice) are for.
- `crd-fuzz-crypto-tls`: a QUIC mode (CRYPTO data per level as a server and a client) with eight new seeds: a QUIC
  ClientHello (the server reaches its Handshake secrets and half-RTT key), a stream ClientHello (`missing_extension`),
  one with a forged ticket, a ServerHello and a ServerHello with EncryptedExtensions (the client reaches its Handshake
  secrets), Handshake data first (refused without an alert), a KeyUpdate (`unexpected_message` at the Initial level)
and an overlong piece (cut to the input); each of the eight seeds' paths was confirmed under gdb. The
  corpus of 28 replays clean; the 20 earlier seeds are byte-identical.
- `crd-crypto-tls-interop-openssl`: 57 of 57, 23 of them new QUIC cases against OpenSSL 3.5's QUIC TLS interface
  (`SSL_set_quic_tls_cbs`, a new `--quic` mode of `crd-openssl-tls-peer`): the three hybrid groups both ways in
  versions 1 and 2, AES-256-GCM (OpenSSL's first), AES-128-GCM and ChaCha20-Poly1305 both ways, a HelloRetryRequest
  and mutual authentication both ways, protocol selection in each server's order and refusal at the Initial level
  both ways, an unpinned key at the Handshake level, resumption both ways across processes and compressed
  certificates both ways. Each ClientHello is read off the wire (transport parameters, protocols, an empty
  legacy_session_id, the key shares and GREASE); each end reports the other's transport parameters; and for each of
  the four secrets each end seals a fixed packet and masks a fixed sample under the keys it derives (crd-crypto on one
  end, OpenSSL's HKDF and ciphers restating RFC 9001 section 5 on the other), compared byte for byte. Teeth: a changed
  nonce in OpenSSL's probe fails the 20 cases that establish.
- 15 source mutations of tls.cpp, each caught by the `[quic-handshake]` cases: the two sides' secrets swapped, the
  client's first protocol selected, a client offering none accepted, a server selecting none accepted, QUIC tickets
  under the stream binding, a secret handed out twice, CRYPTO data at the Initial level only, alerts at the Initial
  level, the receive limit as a failure, post-handshake messages not processed, empty transport parameters accepted,
  no protocol required, the peer's parameters not reported, an unknown version not refused, and the levels swapped
  in `has_keys`.
- LLVM 20.1.8 strict analysis of `tls.cpp`, `tls.hpp`, `test_quic_handshake.cpp`, `fuzz_tls.cpp` and
  `cerid_tls_peer.cpp` is clean (its first run found four deleted members to make public and eight local constant
  names, fixed).
- The scoped CTests (crypto, registers, source routes, module selection) pass once this note exists.

## Next

NET.2's address-validation tokens: Retry and NEW_TOKEN tokens bound to the client's address under a rotating fleet key,
with Retry measured in round trips within the amplification limit (the ML-DSA flights above), then ECH.
