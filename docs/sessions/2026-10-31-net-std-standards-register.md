# 2026-10-31 — NET-STD: the standards register and its guard

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Track C of the [parallel tracks](2026-10-31-parallel-tracks.md), second batch. Row:
[NET-STD](../ROADMAP.md#slice-net-std); previous batch: [NET.1 design](2026-10-31-net-1-network-foundation-design.md).

## CI first

The runs for this track's first three commits (3bb87c7f, e9909c93, c0a098e4) had not started when this batch began:
pushes to `main` cancel the running pipeline, and the run for e9909c93 was still pending. The repository-check
failure of the older run at 06f099ce ("Generated sources carry their provenance and bytes") predates this track and
passes at HEAD (`check-generated.py`: 176 sources). The follow-up the first batch named, the POSIX-only
`BenchMachine` tests of `scripts/test-ci-platform.py` (remote command round trip, detached exit status), passed under
Linux Python 3.12 on the track machine: 96 tests, the three skips being the two Windows Job Object tests and the
PowerShell alias test.

## The register

- **Format.** One `cerid-register/1` format for both registers, so COMPLY.0's legal register adds a domain to the same
  guard instead of writing a second one: common entry fields (identifier, kind, state, owner, rows, source with its
  read day and a verified flag, review day, evidence, deprecation) and a per-domain block (the standards domain adds
  the specification with revision, posted day and snapshot status, the codepoints and the RFC 7696 checklist). The
  [contract](../design/registers.md) records the format, the rules, the snapshot provenance and the review cadence.
- **Content.** [cmake/standards-register.json](../../cmake/standards-register.json), beside the module registry and
  the capability owners: 65 entries. 49 are pinned (planned): the eight Cerid wire protocols with their identifiers
  (`0x0001` session control to `0x0008` relay allocation) and the negotiation extension with the reserved GREASE
  pattern `0x?A?A`; QUIC v1 and v2, datagrams, the greased QUIC bit and compatible version negotiation; HTTP/3,
  QPACK, HTTP datagrams, WebTransport over HTTP/3 (draft -16) and WebSocket over HTTP/1.1, HTTP/2 and HTTP/3;
  MASQUE CONNECT-UDP and CONNECT-IP; DPLPMTUD and L4S; ICE, STUN, TURN and trickle ICE; TLS 1.3, the RFC 10024
  hybrid groups, ECH, certificate compression, SSLKEYLOGFILE (non-shipping only) and ML-DSA certificates; MLS;
  SFrame and WHIP; FlexFEC, RED and RaptorQ in `hesap-comms`; W3C Trace Context. 16 are watched: the row's watch
  list (multipath QUIC, reliable reset, address discovery, ACK frequency, receive timestamps, QUIC-LB, WebTransport
  over HTTP/2, connect-udp-listen, MoQ transport, RTP over QUIC, qlog, Opus DRED, Merkle Tree Certificates, NIST IR
  8547) with QUIC NAT traversal and pcapng.
- **Owners.** The `net` and `crypto` owners of the network foundation's D1 and D2 entered the OWN.1
  [capability registry](../../cmake/capability-owners.json) as planned capabilities (`network.transport`, created by
  NET-STD's negotiation codec; `crypto.providers`, created by NET.2), which completes NET.1's last open clause
  besides OWN itself. The register reads its planned owners from there and keeps no second list.
- **Provenance.** The snapshot was assembled from the research read on 2026-10-27 and the 2026-10-31 design; no page
  was fetched. Entries the research marks as knowledge or secondary, and every codepoint taken from knowledge (the
  TLS group, extension and QUIC transport parameter values, the ML-DSA object identifiers), carry `verified: false`;
  an entry cannot become implemented until they are checked against the IANA registries. Refreshing the snapshot from
  the RFC Editor, the datatracker and IANA is a download the user approves first.
- **Guard.** [scripts/check-registers.py](../../scripts/check-registers.py), the `crd-check-registers` CTest and the
  "Standards register is current" step of `ci/pipeline.py` and the GitHub workflow (with the CI platform table). It
  reuses `check-master-plan.py`'s row parser and module registry reader and the capability registry's owners. It fails an expired pinned draft (185 days
  after posting, counted from the first day of a month- or year-precise posting, unless the IESG or the RFC Editor
  holds it; an `expired` or `replaced` status), an obsoleted pinned RFC, overdue entry or register reviews, sources
  read after the snapshot or today, unknown owners, rows, files or anchors, a governed row without an entry, a planned
  entry whose rows are all Done, an implemented entry without existing evidence or with an unverified codepoint, and a
  checklist item whose rows are all Done without evidence.

## Verification

- `scripts/test-repository-tools.py Registers`: seven tests over fixture trees, run with `--today` to cross the
  expiry boundaries (2027-01-01 passes and 2027-01-02 fails for a July 2026 posting; 2027-01-20 passes and
  2027-01-21 fails for 2026-07-20), plus held, watched, replaced and unpinnable drafts, obsoleted RFCs, overdue
  reviews, future reads, unknown owners and rows, stale planned owners, uncovered governed rows, missing anchors,
  duplicates, Done rows, missing or unproven evidence, unverified codepoints and schema errors. Mutating the expiry
  comparison (`<=` to `<`) and disabling the obsoleted-RFC rule each fail one test.
- On the track machine (linux-gcc-debug tree reconfigured from the commit): `ctest -N` lists `crd-check-registers`
  and it passes; the guard prints 65 entries, 49 pinned, the next review 2026-12-15 and the first pinned draft to
  expire, `webtransport.h3` on 2027-01-02. `scripts/test-ci-platform.py` (96 tests) passes on Windows and Linux.
- Local validators: `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py`, `check-generated.py`,
  `check-ci-tiers.py`.
- The machine ran for about 0.06 h and was stopped.

## Calendar

Without a refresh the guard fails `main` on 2026-12-16 (the register's review after IETF 127) and, for the first
pinned draft, on 2027-01-02. The scheduled re-research tooling is NET-STD's remaining work; until it lands, track C
runs the review.

## Remaining in NET-STD

The negotiation codec (versions, extensions, ignore-unknown, GREASE in every build, transcript binding) with the
identifiers generated from the register, which creates `crd-net` and its OWN.1 entry; the algorithm identifiers and
the signed, monotonic policy generation mechanism behind the `crd-crypto` verification interface; the scheduled
re-research tooling; the approved snapshot refresh; and Done once NET.1 is Done (it waits on OWN, track B).
