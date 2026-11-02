# NET.2: server-authorized message schemas and early data bound to them

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 49; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [message schemas, S18](../design/network-foundation.md#message-schemas).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch takes NET.2's first Remaining clause: server-authorized message schemas, which
  bind early data to the operations a schema declares idempotent.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC: the track's
  WSL lane (`~/cerid-src-track-c`, `~/cerid-track-c-build`) and the win-debug profile in the worktree. No cloud machine
  ran (0 hours, 0 USD).

## CI first

No completed hosted run has a failure this track has not fixed. Push run 41358455047 (33ac38d4), still running, has
eight failed jobs, each with a cause already fixed on main: win-tidy's `quic.cpp`, `test_quic.cpp`,
`test_quic_handshake.cpp` and `fuzz_quic.cpp` findings (964487d5) and `test_fft_kernels.cpp` (d178d59b);
`-Wnull-dereference` at `quic.cpp:271` on linux-gcc-shipping, -release and -relwithdebinfo (b5160de6); the
ISA-isolation and compile-command checks of win-shipping-profile, win-relwithdebinfo and win-clang-cl-shipping
(b37a4177); and linux-clang-tsan's `keccak_log_dispatch` race (3a59a528, read off the job's TSan reports). Its
win-relwithdebinfo `crd-ci-orchestrator-e2e` failure is the CI platform's own test (a 30-second HTTP read on the
cancel endpoint), which no commit of this track touches. Push run 41379324256 (d1aba0e6) is pending; no hosted run
contains 3a59a528..461223d6 yet.

## What landed

Decision S18 ([contract](../design/network-foundation.md#message-schemas)), in `capability.hpp` beside the tokens it
gives meaning to:

- **The schema.** `MessageSchema` (magic `CRDSCHEM`): session type, the schema's identifier and revision, audience,
  issue time, policy generation, the declared operations (what the bits of `Grant::operations` mean) and the
  idempotent subset, signed by the audience's hybrid issuer set. 78 bytes before the signatures; `issue_schema`,
  `decode_schema` (exact, bounded by `kMaxSchemaBytes`, allocation-free) and `CapabilityAuthority::verify_schema`
  (audience and kind, issue skew, revocation generation, issuer set, policy, signatures). The verified
  `AuthorizedSchema` carries the SHA-256 of the signed bytes.
- **The verdict.** `authorize_operation(schema, capability, operations, object, delivery)`: `OtherSchema`,
  `Undeclared`, `Replayable` (early data, or any delivery other than `Established`, asking for a non-idempotent
  operation), `NotGranted`, `Admitted`. NET.3's admission stage S7 calls it; the hand-off table says so.
- **Early data bound to the schema.** `TlsSession::init` takes the server's early data context as
  `QuicHandshake::init` does (required where early data is accepted, refused elsewhere, at most
  `kTlsMaxEarlyDataContextBytes`, the renamed QUIC bound); `TlsContext::early_data()` tells a caller which contexts need
  one. A records server seals the context's SHA-256 into each ticket beside the issue time; a ticket of another
  context resumes with early data switched off for that handshake (`SSL_set_early_data_enabled`, restored by
  `settle_early()` once the 0-RTT decision is made, so the connection's own tickets allow it again) and spends no
  filter entry. Over QUIC the provider already compares the context, so the sealed digest is zero there.
  `SSL_SESSION_copy_without_early_data` was tried first and refused: its copy drops the ticket's age_add, so the
  resumption fails (the three resumption checks failed until the switch replaced it).
- **Early authority.** `CapabilityAuthority::verify_early` presents the peer key a resumed ticket proved, so a Peer
  token authorizes early data; a Proof token is `Unbound` there (AWS-LC has no early exporter).
- **Ticket format.** A server's sealed ticket gained the 32-byte context digest between the issue time and the session
  state. A ticket sealed before this change resumes nothing; nothing was deployed.
- **Ownership.** `cmake/capability-owners.json` gains `crypto.authorization` (capability.hpp, replay.hpp; consumers
  NET.3 and 4.2a), so admission and the session layer call these instead of writing their own.

## Evidence (this PC)

- linux-gcc-debug and win-debug (MSVC): `crd-crypto-tests` 161 cases, 790,141 to 790,142 assertions (some cases loop
  until a random handshake ends), all passing; the schema and early data cases (`[schema],[early]`) 21 cases, 5,657
  assertions. linux-gcc-shipping (GCC 13, LTO, the lane whose link-time `-Wnull-dereference` failed before) links
  `crd-crypto-tests` clean and passes its schema, early data and capability cases (35 cases). New: five schema cases in
  `test_capability.cpp` (one over real resumed TLS sessions) and two records context cases in `test_tls_early.cpp`.
- `crd-fuzz-crypto-capability`: two new modes (schema decode against a restated layout; a generated schema life
  against a model of the verification order, the restated verdict rule and a flipped byte); `--seed` wrote 39 seeds
  (18 new), replayed clean on both lanes, and 4,000 seeded random mutants of them replayed clean.
  `crd-fuzz-crypto-tls`'s 53 seeds replay clean with the early servers naming their context.
- `tls_interop.py` against OpenSSL 4.0.3: 79 of 79; the new case is a Cerid server under another schema's digest
  refusing the OpenSSL client's early data over records while the resumption stands, the bytes sent again as 1-RTT.
- `tidy-files.py` (LLVM 20) clean on every changed C++ file after one nested conditional and one local constant's
  name in the fuzz target were fixed.
- The CTest guards (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, ISA isolation excluded) on
  the whole linux-gcc-debug tree, built after the batch: 19 of 19 pass.
- Thirteen source mutations, each caught by the schema and early data cases (eight by the capability corpus too): early
  data judged as established, `early()` reading the declared set, undeclared operations or another audience passing,
  an idempotent operation outside the declared set accepted, schemas never revoked, the issue time unchecked, the
  digest taken over the signatures too, a peer presented without accepted early data, the records context never
  compared or never sealed, early data left off for the connection's own tickets, and a refused context spending a
  filter entry.

## Next

NET.2's next Remaining clause: MLS (RFC 9420) with OpenMLS and MLSpp interop.
