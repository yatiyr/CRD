# NET-STD: the standards register's first refresh

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01 (UTC evening of 2026-10-31). Row: [NET-STD](../ROADMAP.md#slice-net-std) (track C, batch 12; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[snapshot and review](../design/registers.md#snapshot-and-review). Report:
[2026-10-31-standards-register-refresh.md](2026-10-31-standards-register-refresh.md).

## The user's decision

The user approved the three downloads this track had asked for (2026-11-01, about 01:25 local time), in these words:
"approve the downloads we need to do everything properly, I will contect a lawyer later! DO NOT BLOCK ANYTHING
CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD STANDARD AND ELITE".

The approval covers NET-STD's first refresh (this batch), COMPLY.0's first read of the official legal texts and the
gazette reading, and COMPLY.1's official sanctions lists. Counsel review never holds a row: each item that needs
counsel is a dated, named entry in the legal register with its question, and no row claims legal sign-off.

## CI first

- **Fixed by track A.** The operator's complete run 41247045494 (67977fda) failed to build `crd-ceridc-tests` on
  win-debug, win-debug-scalar, win-asan and win-public-checks. MSVC 19.51 refuses a defaulted
  `std::initializer_list` member (C2797) in `test_ceridc_screen.cpp`, this track's COMPLY.1 screen test. Track A
  had the fix in hand when this batch read the logs and pushed it as d9be0cfd (repeated flags fill the leading slots
  of a null-ended array). This batch took main's fix instead of writing a second one.
- **Run 41231558128 (d5cc2ec0).** Its other failures from this track were all fixed in batch 8: the GREASE
  comparison C2220 (2782ab9e), the wire writer's `stringop-overflow` (f635efb1) and the quoting of the real-binary
  comply command (ee040d0f). Its `cpu_features.cpp` shadowing, `durable_file.cpp` `ftruncate` result and CPU and
  hardware identity failures belong to other tracks and are fixed on main.
- **Still unread.** Run 41247045494's remaining lanes were still running. Batches 10 and 11 (UTF-16 transcoding,
  the wide environment, `GetCommandLineW`) have no hosted Windows compile yet, because every push run since was
  cancelled before its jobs were scheduled. This batch dispatches a complete run on its own push for them.

## The refresh

`scripts/register-review.py` planned the 63 sources (the RFC Editor records of 43 RFCs, the datatracker records of
16 drafts and four IANA registry files), fetched all 63 over HTTPS into
`~/.cerid/downloads/register-review/standards-2026-10-31/` and compared them with the register. The report pins every
record by URL, size and SHA-256. The bodies stay out of the repository, and the same plan fetches them again.

**The readers met the real records.** The record shapes had been written from knowledge, and two IANA shapes were
wrong:

- **QUIC Versions records carry no name.** A record has a value, a status, the RFC it cites and empty notes, so
  every QUIC version read as "IANA names 0x00000001 []". A record without a name now confirms its value only when it
  cites the entry's own document: RFC 9000 for version 1, RFC 9369 for version 2. A draft citation is matched without
  its revision. A named record still confirms by its name alone.
- **ALPN keys records by name.** `<value>HTTP/3</value>` holds the protocol's name, and the description holds the
  identification sequence `0x68 0x33 ("h3")`. The reader had looked for `protocol` and `id` fields that the file does
  not have.
- **Ranges.** A range such as `3-255` no longer reads as a name.
- **The report.** A changed draft now names its earlier revision and posted day. The report gains "Records read",
  the pin table.

The RFC Editor and datatracker records had the expected shapes.

**What the refresh found** (34 register changes applied, 3 amendment proposals):

- **Every IANA codepoint is confirmed.** That is 20 values across QUIC Versions, QUIC Transport Parameters, TLS
  Supported Groups, TLS SignatureScheme, TLS ExtensionType Values, ALPN and AEAD Algorithms. With the three ML-DSA
  object identifiers read by hand (below), the generated headers lost their `iana::unverified` namespaces
  (`crd/net/register_ids.hpp`, `crd/crypto/register_ids.hpp`; the generated-sources manifest is refreshed), and
  their tests now check all 23 constants.
- **Drafts.**
  - Merkle Tree Certificates moved from -00 to -07 (2026-10-07).
  - Media over QUIC transport moved from -20 to -22 (2026-10-01).
  - Exact posted days replace month-only ones: WebTransport over HTTP/3 -16 (2026-07-06), QUIC NAT traversal -00,
    qlog -14.
  - First recorded revisions: WebTransport over HTTP/2 -15, receive timestamps -03, pcapng -06, reliable reset -11
    and ACK frequency -14.
  - The guard now counts the pinned WebTransport draft's expiry from 2026-07-06.
- **Status changes.**
  - `draft-ietf-masque-connect-udp-listen` is in the RFC Editor queue. It is watched for NET-REACH.
  - `draft-ietf-avtcore-rtp-over-quic` expired (its last revision, -14, was posted on 2025-03-21). It is watched for
    NET-LIVE.
  - RFC 8446 is obsoleted, below.

<a id="tls-13-re-pinned"></a>
## TLS 1.3 re-pinned

The RFC Editor records RFC 8446 as obsoleted by RFC 9846. RFC 9846 is "The Transport Layer Security (TLS) Protocol
Version 1.3", from July 2026, published from draft-ietf-tls-rfc8446bis-14. It also obsoletes RFC 5077, 5246, 6961,
7627 and 8422.

- **The guard.** The guard refuses a pinned entry on an obsoleted RFC, so `tls.v13` (NET.2) is re-pinned to RFC 9846,
  current. Its source is the RFC Editor records, so it is now verified.
- **The trigger.** The run records the trigger, so `review.next` moves to 2026-11-25. By then the full re-research
  this status change owes must be done: what the revision changes for NET.2's TLS 1.3 profile.
- **The check.** A second fetch of the re-pinned plan, the same day, found 0 changes, 0 proposals and 0 unread
  sources. It read RFC 9846's record: 1288 bytes, SHA-256
  `32a9ef2bf0c3fc620746aa2cd2474efd0df77c1073c7fd4e2f5071de0f926561`.

<a id="read-by-hand"></a>
## Read by hand

- **ML-DSA object identifiers.** The NIST CSOR algorithm registration page defines `id-ml-dsa-44`, `-65` and `-87`
  as `{ sigAlgs 17 }`, `{ sigAlgs 18 }` and `{ sigAlgs 19 }`, where `sigAlgs` is `{ nistAlgorithms 3 }` under
  2.16.840.1.101.3.4. The register's 2.16.840.1.101.3.4.3.17 to .19 are confirmed, and `pki.ml-dsa` loses
  `verified: false`.
- **FIPS 204 and FIPS 186-5.** Both are final (2024-08-13 and 2023-02-03), neither withdrawn nor superseded. NIST
  notes errata for a future revision of each. `crypto.ml-dsa` and `crypto.ecdsa` record the confirmed source.
- **NIST IR 8547 is final** (2026-10-27). The CSRC page marks the 2024 initial public draft "Obsoleted on October 27,
  2026 by IR 8547".
  - **Source.** The final PDF was read from `https://nvlpubs.nist.gov/nistpubs/ir/2026/NIST.IR.8547.pdf`: 742743
    bytes, SHA-256 `cf1c5b3eeb3bdd8bf1c30e16bb6f1a77b46f75eb619619e14d05f0821f4272c4`.
  - **Schedule.** ECDSA, EdDSA, RSA, finite-field and elliptic-curve DH and MQV are deprecated after 2030 and
    disallowed after 2035, whatever their classical security strength. The draft had distinguished 112-bit
    parameters.
  - **Federal deadlines.** OMB M-26-15 asks the most sensitive federal systems to move key establishment to
    post-quantum by 2030-12-31 and signatures by 2031-12-31, and agencies to support TLS 1.3 by 2030-01-02.
  - **Register.** The watched `nist.ir-8547` entry is re-pinned to the final text.
  - **Design.** The network foundation's dated refusal of classical-only key exchange
    ([crypto](../design/network-foundation.md#crypto)) now cites the final schedule. NET.2 sets Cerid's dates.
- **W3C Trace Context.** It is a W3C Recommendation (Level 1, 2021-11-23), as the register holds.
  `ops.trace-context` records the confirmed source.
- **IETF meetings.** The IETF meeting page confirms IETF 127 (San Francisco, 2026-11-14 to 2026-11-20). The calendar
  marks it verified and adds IETF 128 (Kuala Lumpur, ending 2027-03-12), 129 (Berlin, ending 2027-07-23) and 130
  (Vancouver, ending 2027-11-12).

## Proposed ROADMAP amendments (for the user)

The review edits no row. It proposes:

1. **tls.v13 (NET.2).** NET.2's TLS 1.3 profile cites RFC 9846. The full re-research due on 2026-11-25 reads what
   the revision changed. This track holds NET.2 and takes it there.
2. **masque.connect-udp-listen (NET-REACH).** Bound UDP proxying is in the RFC Editor queue. NET-REACH decides
   whether its relay path adopts it once it is an RFC. This track holds NET-REACH.
3. **media.rtp-over-quic (NET-LIVE).** RTP over QUIC expired. NET-LIVE (not this track's) decides whether it stays
   watched or Media over QUIC replaces it.

## Verification

- **Python.** The `RegisterReview` and `RegisterIds` suites pass: 14 cases, among them a new reader case for the real
  shapes.
- **Mutations, eight, each caught:**
  - in the codepoint judge: an unnamed record verified without its document, and a named record verified by its
    document;
  - in the reader: ranges read as names, the identification sequence not read as a value, a draft revision kept in
    a cited document, and xref documents ignored;
  - in the report: the records not pinned, and the earlier revision not reported.
- **Guards.** `scripts/check-registers.py` passes: the standards register is current and its next review is due on
  2026-11-25. `scripts/check-generated.py` passes.
- **C++, on linux-gcc-debug on the track machine:** `crd-net-tests` and `crd-crypto-tests`, with every constant of
  both generated headers checked, plus the scoped CTest selection (net, crypto, the registers and the `crd-no-*` and
  `crd-check*` guards).

## Remaining in NET-STD

- **The full re-research the TLS 1.3 trigger owes,** due 2026-11-25 (`review.next`).
- **Done once [NET.1](../ROADMAP.md#slice-net.1) is Done.** NET.1 waits on [OWN](../ROADMAP.md#slice-own), track B.
