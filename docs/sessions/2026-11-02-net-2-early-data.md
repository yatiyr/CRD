# NET.2: early data (0-RTT) over TLS and QUIC

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 48; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [early data, S17](../design/network-foundation.md#early-data).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch takes NET.2's first Remaining clause, 0-RTT over TLS and QUIC.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC: the track's
  WSL lanes (`~/cerid-src-track-c`, `~/cerid-track-c-build`: linux-gcc-debug and linux-clang-tsan) and the win-debug
  profile in the worktree. No cloud machine ran (0 hours, 0 USD).

## CI first

Push run 41358455047 (33ac38d4) failed four threaded crypto cases on linux-clang-tsan: a data race on
`BORINGSSL_function_hit` in AWS-LC's `keccak_log_dispatch`. AWS-LC defines `BORINGSSL_DISPATCH_TEST` in every
configuration whose name lacks "rel": instrumentation for its own dispatch tests (not built here), by which its assembly
and its Keccak code mark a shared table with plain stores, so two threads hashing at once race. Cerid's build no longer
compiles it (3a59a528, pushed first): the definition leaves the directories of AWS-LC's libraries, and NASM, which also
reads it from AWS-LC's own NASM flags, undefines it after them. Reproduced on the track's new linux-clang-tsan tree:
before, exactly those four cases failed with four `keccak_log_dispatch` reports; after, no `BORINGSSL_DISPATCH_TEST` is
left in its `build.ninja`, and all 140 crypto cases pass under TSan with no report. On win-debug only NASM's commands
still name it, each followed by `-UBORINGSSL_DISPATCH_TEST`, and `crd-crypto-tests` links (an assembly object still
marking the table would leave it undefined) and passes its threaded cases. No suppression and no register entry: the
instrumentation was Cerid's configuration of the dependency.

The other failed jobs of that run and of complete-tier run 41342960677 were already fixed on main: GCC's link-time
`-Wnull-dereference` in `quic.cpp` (linux-gcc-shipping and linux-gcc-relwithdebinfo) by b5160de6, the ISA-isolation
and compile-command checks of win-shipping and win-shipping-profile by b37a4177, linux-gcc-debug's ECDSA certificate
compression case by 6e5dbb36 and win-tidy's `test_fft_kernels.cpp` finding by d178d59b. Push run 41371880561 (ac29068b)
is pending.

## What landed

Decision S17 ([contract](../design/network-foundation.md#early-data)): the provider's 0-RTT, off unless both ends set
`TlsConfig::early_data`.

- Tickets carry their sealed issue time; a server that accepts early data needs a `ReplayFilter` whose period covers
  the ticket lifetime and `kTlsMaxTicketAgeSkew`, and opens each ticket once (its salt's first 16 bytes, filed by the
  issue time): a ticket seen before resumes nothing, so a replayed first flight gets no early data from that server.
- A client keeps Cerid's ticket record (the allowance its NewSessionTicket states, the server's QUIC transport
  parameters, the session); `send_early()` sends whole messages within it. A refused client is `Rejected` and sends
  its bytes again after the handshake; the provider refuses a ticket age more than 60 seconds off (`Stale` on the
  server), a HelloRetryRequest, another protocol, a server without early data and another QUIC context.
- A server's early bytes come only from `read_early()`, after the handshake's checks; it peeks before it reads, so the
  read that runs into the client's Finished completes the handshake and leaves the 1-RTT bytes to `read()`.
- QUIC: `QuicLevel::Early` (keys only, the client's), the server's early data context at `init()`, the client's
  remembered transport parameters, the 0-RTT secret zeroed on a refusal.
- The fuzz target's ECH mode treated an empty ECHConfigList as one the context must refuse; a context takes it as a
  client without ECH. Found by this batch's mutation run and fixed in the oracle.

## Evidence (this PC)

- win-debug (MSVC) and linux-gcc-debug: `crd-crypto-tests` 154 cases (786,096 to 786,098 assertions: some cases
  loop until a random handshake ends), `test_tls_early.cpp` 14 of them; linux-clang-tsan: the early, TLS and QUIC
  cases, 84 cases, no report.
- `crd-fuzz-crypto-tls --seed`: 53 seeds (13 new: early data over records and QUIC both ways, ticket records of
  another version, with parameters past the end and without a session), replayed clean on both lanes; 4,000 seeded
  mutations of them (half of the early data seeds) clean after the two oracle fixes above.
- `tls_interop.py` against OpenSSL 4.0.3: 78 of 78, the 9 new ones early data over records both ways accepted (the
  early_data extension read off the resumed ClientHello, the bytes reported as early data by the server and not echoed
  as 1-RTT data), refused by a server without it and after a HelloRetryRequest both ways with the bytes sent again,
  over QUIC both ways with the 0-RTT secrets' probes equal, and a Cerid server under another early data context
  refusing it while the OpenSSL client had made its 0-RTT keys.
- `tidy-files.py` (LLVM 20) clean on every changed C++ file after three nested conditionals, two enum bases and two
  local constant names were fixed.
- Twelve source mutations of `tls.cpp`, each caught by the early data tests or the TLS corpus: the filter never
  consulted, the issue time not sealed, `read()` returning early bytes, a refused client keeping its 0-RTT secret, no
  QUIC early data context, the allowance not checked, a replay or a stale ticket reported as refused, 1-RTT bytes the
  peek ran into read as early ones, the ticket's allowance misread, the remembered parameters not kept, and a server's
  early data settled only at the handshake's end.

## Next

NET.2's Remaining continues with the server-authorized message schemas, which bind early data to the operations a
schema declares idempotent; then MLS (RFC 9420) with OpenMLS and MLSpp interop.
