# NET.2: the policy generation in sessions (2026-11-03)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Contract: [S20](../design/network-foundation.md#tls-policy) and
[policy wire decision 9](../design/network-foundation.md#policy-wire).
User direction carried: "I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD STANDARD AND ELITE" (2026-11-01).

## CI first

Dispatch 41542040132 (97fe4a37) is still running: its finished jobs passed, none failed. Nothing to fix.

## Order

The 2026-11-03 order lists the Android Keystore and Apple Keychain before "the rest of NET.2". This batch took the
policy generation first because a generation nobody enforces refuses nothing: no session read the policy store.
The Android Keystore is the next slice.

## What landed

- `policy.hpp`: format 2, each rule with a scope (`Everywhere`, `CeridServices`); a service session follows its
  scope's rule first; unknown scopes decode, stay signed and govern nothing; format 1 read as Everywhere rules.
- `tls.hpp`/`tls.cpp`: `TlsConfig::policy` and `policy_scope`; each TlsSession and QuicHandshake takes a snapshot at
  start and narrows its groups, AEADs and peer signature algorithms on its own provider connection, preferred first;
  `PolicyRefused` for a refused credential, an emptied list or an empty store; `check_policy()` at every key update
  (own, the one `send()` makes before any record, the peer's), closing with close_notify; QUIC's transport calls it
  before each packet key update. The suite list builder is shared by contexts and sessions.
- Tests: `test_tls_policy.cpp` (6 cases, 7 sections), `test_policy.cpp` (scopes, the N-1 reader) and
  `crd-fuzz-crypto-policy` (restated format 2 and format 1, a scope-aware model; seeds regenerated, the format 1
  seed equal byte for byte to the previous honest seed).

## Evidence

- win-debug (MSVC): the whole of crd-crypto-tests, 275 cases (274 passed, 1 skipped, 1 447 674 assertions); the
  policy corpus replayed (20 inputs).
- linux-gcc-debug: `[tls],[net-std],[quic]` 100 cases passed, the corpus replayed; linux-gcc-release (LTO link
  clean): the policy and TLS policy cases, 22 passed. GCC's `-Wunused-function` found an unused helper, now used by
  the preferred-AEAD case.
- Mutations, each caught: the credential check, the peer key update check, preferred-first ordering, the `send()`
  check (TLS tests); the scope's own rule (test_policy, the TLS scope test and the fuzz seed `sequence-scopes`).

## Next

The Android Keystore protector (pinned NDK, emulator); then the first generation itself (offline keys, dates,
bootstrap), and the policy over ECH's HPKE suite and MLS's cipher suites.
