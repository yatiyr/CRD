# NET.2: MLS post-quantum suites awaiting IANA (2026-11-03)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Register contract: [registers](../design/registers.md).

## CI first

Dispatch 41542040132 (97fe4a37) and push run 41521847645 (57c51e55) have no failed job (8 and 28 passed, the rest
running). Nothing to fix.

## Decision

<a id="decision"></a>User order (2026-11-03): implement and prove the MLS post-quantum suites with the draft's
codepoints, "record a dated register entry that switches to the assigned codepoints when IANA publishes them, and do
not hold NET.2's Done on it". `mls.pq-ciphersuites` is now `implemented` with `awaiting_iana` (recorded 2026-11-03):
its eleven codepoints are draft-ietf-mls-pq-ciphersuites-07's (0x0010 to 0x0017, 0x0020 to 0x0022), still
`verified: false` and still generated into `iana::unverified`, so every user of one says so where it is used.

## What landed

- `check-registers.py`: an implemented entry with an unverified codepoint passes only under `awaiting_iana`
  (`recorded`, `decision`): refused on an RFC, without an unverified IANA codepoint, dated after today or citing a
  missing document or anchor.
- `register-review.py`: under `awaiting_iana` an absent codepoint is expected (no proposal); another value IANA
  assigns proposes the switch and triggers a full re-research (a wire change); a new draft revision asks for the
  codepoints to be re-read; once IANA confirms the last one the record is dropped, the constants leave
  `iana::unverified` and `mls.cpp` no longer compiles until it names the assigned ones. `due` lists the entry.
- Tests: three cases in `test-repository-tools.py` (`Registers`, `RegisterReview`); two mutations caught (the
  guard's exception, the review's expected absence).

## Evidence

- `python scripts/test-repository-tools.py Registers RegisterReview`: 52 tests passed; the whole file, 327 tests, passed (3 skipped).
- The suites' own proof is unchanged: every MLS behavioural test runs in all eleven (batch 59, M3 layer 10d).

## Next

The Android Keystore protector (pinned NDK; the emulator proof on a GitHub Linux runner with KVM, since this PC's
WSL `/dev/kvm` needs the user in the `kvm` group), then the Keychain lane on macOS runners.
