# NET-STD: the full re-research the TLS 1.3 re-pin owed

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [NET-STD](../ROADMAP.md#slice-net-std) (track C, batch 13; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[snapshot and review](../design/registers.md#snapshot-and-review). Research:
[TLS 1.3 after RFC 9846](../research/2026-11-01-tls-13-rfc-9846-re-research.md). Report:
[2026-11-01-standards-register-full-review.md](2026-11-01-standards-register-full-review.md).

## The user's decision

The user approved the downloads on 2026-11-01, about 01:25 local time, in these words: "approve the downloads we need
to do everything properly, I will contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I
WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD STANDARD AND ELITE". The fetches of this batch were made
under that approval: the 66 sources of the full plan and the text of RFC 9847, read by hand.

## CI first

- **No new failure from this track.** In the operator's complete run 41247045494 (67977fda), win-release and
  win-relwithdebinfo have now failed too. They fail on the same MSVC C2797 in `test_ceridc_screen.cpp` as win-debug,
  win-debug-scalar, win-asan, win-public-checks and win-tidy. That test is COMPLY.1's, and main fixed it in d9be0cfd.
- **Still running in 41247045494:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (the first hosted run of the
  concurrent comply tests), linux-gcc-shipping, win-shipping and win-shipping-profile.
- **Dispatched run 41254049879 (31404ec4, complete tier):** about half its jobs were running when this batch ended and
  none had failed. It includes d9be0cfd and is the first hosted Windows, clang-cl and ARM64 compile of batches 10
  and 11.

## What landed

**The full scan reads what the trigger pins.** `scripts/register-review.py`:

- `plan --scan full` adds the full text of every document the owed full re-research reads, and the plan records its
  scan. The owed documents are those that the triggering entries of the light runs since the latest full run pin:
  `rfc-editor.org/rfc/rfcNNNN.txt`, a draft's `ietf.org/archive/id/<draft>-<revision>.txt`, or a W3C or NIST document
  read by hand. The plan refuses a triggered draft without a recorded revision and a trigger that names no entry.
- `compare --scan full` compares only a full plan.
- Applied, a full scan records `--ref`, the re-research document, which must exist and must cite the scan's report.
  `--ref` is refused on a light or unapplied run.
- The report gains a "Read in full" section.
- `check-registers.owed_runs` is the one definition of an owed full run, used by `review_due` and by the plan.

**IANA's Recommended column is register data (RFC 9847).**

- **Reader.** The IANA reader returns `IanaRecord(values, names, documents, recommended)`. The column is `<rec>` in the
  Supported Groups and ExtensionType files and `<recommended>` in the SignatureScheme file.
- **Review.** A confirmed codepoint in a registry with the column records IANA's value. A record without a single
  value is reported as not read. A pinned entry whose codepoint becomes "D" triggers a full re-research and a
  proposal.
- **Guard.** `check-registers.py` requires the value on every verified codepoint of those three registries. It
  refuses the value on any other codepoint, refuses a value other than Y, N or D, and refuses a pinned entry with a
  discouraged codepoint.

**The run (2026-11-01):** all 66 sources were read, with 18 register changes applied, 0 proposals from the comparison
and 0 sources unread. RFC 9846 was read in full (339413 bytes, SHA-256
`b1bee06a814f92ca4677c85b97519be53869b2bf8edc79ade955fb51c0402b17`).

- **TLS 1.3 profile.** The [research](../research/2026-11-01-tls-13-rfc-9846-re-research.md#delta) maps each of
  RFC 9846's technical changes onto NET.2: fresh key shares, key updates before the usage limits on every Cerid
  record layer, the 2^48-1 epoch bound, ticket hygiene, `general_error` toward unauthenticated peers, the misbinding
  defence for raw public keys and TLS 1.3 only. The network foundation's crypto section gains
  [the TLS 1.3 profile](../design/network-foundation.md#tls-13-profile) and T03 points to it.
- **Recommended values.** They are recorded for the 14 TLS codepoints. X25519MLKEM768 is "Y". SecP256r1MLKEM768,
  SecP384r1MLKEM1024 and the ML-DSA schemes are "N", and those are deliberate choices. No pinned codepoint is
  discouraged.
- **A gap closed.** The policy pins ML-DSA certificates but the register had no TLS signature scheme for them. The
  new entry `tls.ml-dsa` (draft-ietf-tls-mldsa-06, in the RFC Editor queue) holds `mldsa44`, `mldsa65` and `mldsa87`
  (0x0904-0x0906), confirmed against IANA. `crd/crypto/register_ids.hpp` gains them as verified constants.
- **New entry.** `tls.registry-recommended` (RFC 9847) is implemented by the guard and the review.
- **tls.v13.** Its source, note and review date (2027-11-01) now point to the research.
- **The run.** It is the register's second full run, citing the research's `#delta`. `next` moves to 2026-12-15,
  the light scan after IETF 127.

**Amendments for the user** (the research's list):

1. NET.2 cites RFC 9846, RFC 9847 and draft-ietf-tls-mldsa, with the conformance points above. This track holds
   NET.2.
2. connect-udp-listen (NET-REACH) is unchanged.
3. RTP over QUIC (NET-LIVE) is unchanged.
4. NET.2 decides ML-DSA's classical companion in TLS, and whether a high-assurance profile offers pure ML-KEM-1024.

## Verification

- **Python.** The `RegisterReview`, `Registers`, `RegisterIds` and `CryptoInventory` suites pass. They include
  three new cases:
  - the guard's Recommended rules;
  - the column recorded, with a discouraged pinned codepoint triggering;
  - the full scan from plan to recorded re-research, with each of its refusals.
- **Mutations: twelve, each caught.**
  - In the guard: the "D" refusal, the missing-value refusal, the value check, and the owed-run day bound.
  - In the reader: the `<recommended>` spelling.
  - In the review: the "D" trigger and the single-value check.
  - In the plan: the full-text sources and the revision refusal.
  - In compare: the full-plan check, the citation check and the recorded `--ref`.
- **Guards.** `check-registers.py` (standards: 75 entries, 59 pinned, next review 2026-12-15), `check-generated.py`
  (the crypto header's manifest refreshed), `check-allman-braces.py`, `check-master-plan.py` and
  `check-repository.py` pass.
- **C++, on linux-gcc-debug on the track machine** (0.13 h, stopped):
  - `crd-crypto-tests` passes: 18 cases, 929 assertions, among them the new ML-DSA scheme case.
  - `crd-net-tests` passes: 17 cases.
  - The scoped CTest selection passes, 73 of 73: crypto, net, the registers, the register-ids drift, the
    `crd-no-*` and `crd-check*` guards and the repository tools.
  - Teeth: 0x0905 changed to 0x0915 in the generated header fails the crypto case and the drift check, and the
    restore passes.

## Remaining in NET-STD

- **Done once [NET.1](../ROADMAP.md#slice-net.1) is Done.** NET.1 waits on [OWN](../ROADMAP.md#slice-own), track B.
  Every engineering clause of NET-STD is landed.
