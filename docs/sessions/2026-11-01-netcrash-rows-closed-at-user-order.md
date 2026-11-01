# NET.1, NET-STD, COMPLY.0 and COMPLY.1 closed at the user's order; GREASE in every TLS ClientHello

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Rows: [NET.1](../ROADMAP.md#slice-net.1), [NET-STD](../ROADMAP.md#slice-net-std),
[COMPLY.0](../ROADMAP.md#slice-comply.0) and [COMPLY.1](../ROADMAP.md#slice-comply.1) closed;
[NET.2](../ROADMAP.md#slice-net.2) and [4.2a](../ROADMAP.md#slice-4.2a) amended (track C, batch 36; the
[parallel tracks](2026-10-31-parallel-tracks.md)).

## The order

The user ordered the rows that wait only on table order closed: "let's close the parts waiting for CI"
([record](2026-11-01-user-order-close-waiting-rows.md#user-order)). Each of the four rows had every clause proven and
waited only on table order behind the open-ended SIMD-PERF track; each now carries the record's `user-order` marker
and closes Done. [OWN](../ROADMAP.md#slice-own), the prerequisite of NET.1 and COMPLY.0, closed earlier on 2026-11-01.

## The flip duties

- **NET-STD's three entries.** The register's guard refuses a planned entry whose rows are all Done.
  - `ietf.grease` (RFC 8701) is now implemented, owned by `crypto` with rows NET-STD and NET.2: the TLS work below.
  - `quic.grease-bit` (RFC 9287, transport parameter 0x2ab2) and `quic.version-negotiation` (RFC 9368, 0x11) are
    QUIC transport parameters. No QUIC code exists until 4.2a's transport, so marking them implemented would be
    false. Their rows are now NET-STD and 4.2a, they stay planned, and 4.2a's row names both as its own clauses
    under NET-STD's contract.
- **COMPLY.0 and COMPLY.1.** The legal register's guard passes with both rows Done (219 entries, 205 pinned). The
  counsel review stays one open register entry (82 questions), never a clause, by the user's decision of 2026-11-01.

## GREASE in every TLS ClientHello (RFC 8701)

- `tls.cpp` turns on the provider's GREASE for every context, in every build. AWS-LC 5.12.0 (read in its
  `ssl/extensions.cc`, `handshake_client.cc` and `handshake.cc`) then sends a reserved cipher suite, supported group
  and supported version, a one-byte key share and two reserved extensions (the first empty, the second one byte). The
  values come from a per-handshake random seed. A ClientHello sent again after a HelloRetryRequest keeps them and
  drops the GREASE share. A server's GREASE goes only into NewSessionTicket, which this profile does not issue yet.
- `test_tls.cpp` reads the hellos off the wire with its own RFC 8701 predicate (independent of the provider and of
  crd-net, which crd-crypto may not depend on, decision D2). The new case checks every field over eight connections,
  that the values vary between connections, and that a retried hello keeps them and shares only the group asked for.
  `read_shares` skips the GREASE share, so the earlier cases keep their exact share counts.
- `tls_interop.py` checks the same fields in Cerid's ClientHellos, the retried one included, and strips the GREASE
  share before it compares the offered groups. All 15 cases pass, so OpenSSL 3.5 servers ignore the values.

## Evidence (cloud machine `cerid-track-c`, linux-gcc-debug unless named)

- `crd-crypto-tests "[tls]"`: 2015 assertions in 15 cases pass (debug and linux-gcc-release, no warnings). The
  whole crypto suite passes: 94128 assertions in 57 cases.
- `tls_interop.py` against the pinned OpenSSL 3.5.9: 15 of 15 cases pass.
- The scoped CTests (`crypto`, `crd-net`, `crd-no-*` and the register, capability, source-route, include-budget and
  header checks): 19 of 19 pass. The `crd-fuzz-crypto-tls` corpus replays clean.
- clang-tidy 20.1.0 is clean on `tls.cpp` and `test_tls.cpp`. Three findings in the first draft were fixed: a
  forwarding reference never forwarded, and two names of local constants.
- **Teeth.** Removing the GREASE call fails 4 assertions of the new case and 6 of the 15 interop cases (every
  case that reads a Cerid ClientHello). Restoring it passes both again.
- Validators: master-plan, repository, Allman braces, generated sources and registers (77 standards entries, 61
  pinned).
