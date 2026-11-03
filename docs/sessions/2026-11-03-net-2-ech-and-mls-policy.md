# 2026-11-03: NET.2, the policy generation over ECH's HPKE suites and MLS cipher suites (Track C batch 74)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-03. Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial).

## CI first

Dispatch 41568608043 (29844ddc): its failed jobs (win-vs, win-arm64-debug, both arm64 Linux lanes, Repository
checks) fail only the Python CTests whose counts and android device 98b3a9b0 and 26c32908 already fixed (setup-python
14, 29 visible presets, the `android` own-CI device and lane). Dispatch 41574979037 (26c32908, the first hosted
emulator run) was still in preflight, so its android audit carries forward. Order: the Android Keystore protector
waits for that lane's first hosted boot, since it is the protector's only proof path; this PC cannot boot the
emulator until WSL's user is in the `kvm` group (a user action).

## What landed

- **Decision S21** ([design](../design/network-foundation.md#tls-ech-policy)): `PolicySnapshot::status` over a suite;
  a client hands the provider only ECH configurations whose every pickable suite the generation allows (preferred
  first; none left is `PolicyRefused`, retry configurations included); a server whose ECH keys publish a refused
  algorithm is refused. `TlsEchKey::init` takes the AEADs it publishes and writes the ECHConfig itself;
  `hpke_codepoints`/`hpke_suite_of` map HPKE suites to IANA identifiers.
- **Decision M14** ([design](../design/network-foundation.md#mls-policy)): `GroupConfig::crypto_policy`/`policy_scope`,
  `check_suite_policy`, `Group::set_now`; create, Welcome and external joins, and every staged commit (own or received)
  refuse a refused suite; a ReInit to an allowed suite is the way out. `CryptoError::PolicyRefused` is new.
- The S20 "not yet under the policy" bullet is closed. Test policy helpers are shared (`TestInstallation`,
  `accepting_all`, `refuse_rule`, `prefer_rule` in policy_test_signer.hpp).

## Evidence

- win-debug: crd-crypto-tests `[ech]` (16 cases), `[tls]` (70), `[mls]` (69, the three new `MLS policy` cases among
  them), `[hpke]` (codepoints cross-checked against every RFC 9180 and draft-ietf-hpke-pq vector), `Policy*` and
  `Provider*` pass; all 13 crypto fuzz corpora and the `crd-no-*`/`crd-check*` guards pass (32 CTests).
- `crd-fuzz-crypto-tls`: a client under a generation refusing ChaCha20-Poly1305 is `PolicyRefused` exactly when the
  restated filter leaves no configuration, and otherwise never encrypts with it; two new seeds (allowed, and skipped
  then allowed).
- linux-gcc-debug (WSL): crd-crypto-tests and crd-fuzz-crypto-tls build; 94 focused CTests pass.

## Next

The first policy generation (offline keys, the dated classical-only refusals, the bootstrap generation); the Android
Keystore protector after dispatch 41574979037's android audit.
