# 2026-11-25: NET.2, the threaded ML-DSA signing refusal crossed between crd-crypto and raw calls

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET-PATH](../ROADMAP.md#slice-net-path) batch; the entry is [NET.2](../ROADMAP.md#slice-net.2)'s provider.
Follows [the split](2026-11-25-crypto-threaded-signing-control.md).

**CI first:** CI 45529733321 (fdd5c20c) failed `crd-ci-platform` on every lane: the parity table, already regenerated
by 76ed9f1d. Its linux-gcc-asan red beside it is TP-5 (MikkTSpace), registered. Nothing else to fix.

The three suspects of the last note run before or after the faulting first signing, so they act only through other
threads; the arms therefore cross the key, the signing and the verification.

## What landed

- `raw_signing.hpp`, `copy`: `SigningKey::sign`'s and `ProviderVerifier::verify`'s exact calls, written in the
  program. A refusal names the provider error's source line.
- `crd-mldsa-threads-cerid` adds `seed-key`, `raw-verify`, `no-verify`, `raw copy` and `raw crd-verify`, and
  `touch` (both programs): each thread touches 512 KiB of its stack before its first signing.
- `provider-host-check.py --arms` (unit-tested); seven new arms.
- `host-check.yml`: `provider_arms`, and `provider_cpu=affected` (default: Xeons from Sapphire Rapids on); ten legs.

## Evidence (failures out of 40 runs)

- 45553773567 (Xeon Platinum 8573C):
  - Failed: case 29; `cerid` 27; `seed-key` 26; `raw-verify` 26; `no-verify` 27.
  - Clean (0): `control`, `control-copy`, `raw`, `raw-copy`, `raw-crd-verify`.
- 45557190754 and 45557203999 (Xeon 6973P-C each): `cerid` 27 and 25 (10 fast exits, 0xC0000409), `cerid-touch` 0
  and 0, `cerid-raw` 0 and 0. 45554828003 drew no affected CPU.
- It fails only on the `SigningKey` path, and touching the stack first removes it. The two paths still differ in where
  the code is built and placed, and in stack layout: a raw session holds a 3,309-byte array in the object.
- The refusals' 9.5 to 22.4 s fit mldsa-native's rejection loop run to its bound: (65535 - L) / L = 13,106 attempts.
  No provider error has been captured.
- This corrects the last note: no crd-crypto call differs from the raw copy, so "the cause is crd-crypto's code" is not
  shown.

## Next

One dispatch:
1. `control copy` without the in-object array. If it fails, that is the Cerid-free reproduction.
2. A crd arm padded with a 3,309-byte zeroed member.
3. A stack-only touch from a function ASan does not instrument, to separate stack pages from shadow.

No gate or TP entry until a Cerid-free arm fails.
