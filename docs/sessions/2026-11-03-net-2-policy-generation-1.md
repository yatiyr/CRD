# 2026-11-03: NET.2, crypto policy generation 1 and its issuing tool (Track C batch 75)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Contract: [S22](../design/network-foundation.md#policy-issue).

User direction recorded (2026-11-01): "approve the downloads we need to do everything properly, I will contect a
lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY
DONE AND GOLD STANDARD AND ELITE".

Order: the 2026-11-03 order lists the Android Keystore and the Apple Keychain first. The Android lane's dispatch
(41574979037) is still queued and the Keychain cannot compile on this PC, so the locally provable policy generation
went first; the Keychain and its macOS lane are next.

## What landed

- `scripts/gen-register-ids.py` generates `algorithm_name` and `protocol_name`: the register's own name of each
  Cerid identifier, by which reviewed text names it (Python and C++ tests).
- `assets/crypto-policy/generation-1.policy.toml`: 23 rules, one Everywhere rule for each of the 20 algorithms plus
  Cerid services rules. Classical-only key exchange is refused for Cerid services from 2026-11-01 and everywhere from
  2030-01-01. Traditional signatures are refused from 2036-01-01. Hybrids, ML-KEM, ML-DSA and the 256-bit AEADs are
  preferred.
- `ceridc policy body|assemble|verify` (tools/ceridc/src/policy.cpp) over crd-crypto's writer, decoder, store and
  provider verifier; strict TOML for the source and the trust set (unknown keys, names, scopes, days refused with
  key and line).
- ceridc now links crd-crypto, so its cryptography inventory changed: its five classifications were restated from
  crd-crypto's (the same answers, the EAR one proposed as a (b)(1) mass-market item since ceridc offers no
  cryptographic interface), reviewed 2026-11-03, counsel still open in the legal register's one review entry.

## Evidence

- win-debug: `test_ceridc_policy.cpp` (5 cases: generation 1 rendered, signed by fresh ML-DSA-65 and ECDSA P-256
  keys, joined and decoded with its exact dates; verify current, grace, expired, not yet issued, rollback, missing
  and forged signature, one-key trust set, source mismatch; every refusal; the real binary's bytes) and
  `test_register_ids.cpp` pass. Guards pass (crd-diag-coverage segfaulted once under -j4 and passed alone, the
  host's known misexecution); registers, inventory and drift checks pass.

## Next

- The issue ceremony is the user's (S22): create the ML-DSA-65 key on an offline machine and the ECDSA P-256 key in a
  TPM or token, sign the rendered body, commit the trust set and signed generation. The tool's key-store signing
  verb (`ceridc policy sign` through `StoredSigner`) and the bootstrap embedding with persisted-then-bootstrap
  startup come next, then the Keychain lane.
