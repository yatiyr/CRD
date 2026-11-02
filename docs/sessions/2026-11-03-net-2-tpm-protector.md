# NET.2: the key store's TPM 2.0 protector on Windows (2026-11-03)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Contract: [key store K11](../design/network-foundation.md#key-store).

## CI first

Dispatch 41508851661 (13c815d3, complete tier) now has 15 failed jobs, every one read: each is one of the two
batch-62 defects b5a2d1b9 fixed (the key-store fuzz target missing from `scripts/fuzz.py`, failing
`crd-repository-tools` on every lane that runs it; GCC LTO `-Wnull-dereference` in `test_key_store.cpp:236/252` on
linux-gcc-release, -relwithdebinfo and -shipping). 14 jobs were still running; push runs 41521847645 and 41529036811
had not started, so no completed hosted run holds 4fea5806 or later yet.

## What landed

- `TpmKeyProtector` ("tpm-pcp", `Hardware` protection): one persisted, decrypt-only RSA-2048 TPM key per master key
  through the Platform Crypto Provider, RSA-OAEP-SHA-256 with the store in the label, deleted to forget (K5 now holds
  on Windows). Probed first: the provider has no sealing, AES or HMAC keys, so OAEP under a TPM key is the route.
- Two provider facts found on the hardware and written down: the TPM refuses an OAEP label longer than its largest
  digest (`TPM_RC_SIZE`, 0x80280095, on a 58-byte label; now 31 bytes), and `NCryptDeleteKey` refuses
  `NCRYPT_SILENT_FLAG` (`NTE_BAD_FLAGS`).
- `KeyStore::erase()`: forgets the newest save's master keys (a pending one too) under the lock, then removes the
  file and closes; a stale store refuses (K6), a protector that will not forget stops it before anything is removed.
- Standards register `tcg.tpm2` (new spec type `tcg`, checker, review planner, docs and a tool test); crypto
  inventory regenerated, cerid-package's findings restated and reviewed.

## Evidence

- win-debug (MSVC): `[key-store]` 17 cases, 9,524 assertions, the hardware case on this PC's Intel PTT TPM; the whole
  `crd-crypto-tests` (261 cases, 1,446,780 assertions). No TPM key is left after a run (`certutil -user -csp
  "Microsoft Platform Crypto Provider" -key`); the keys failed runs leaked were deleted.
- Teeth: a `forget` that deletes nothing fails four checks of the hardware case.
- linux-gcc-debug: the 15 `Key store` CTests (the hardware case skips there, the contract case runs the
  `Unavailable` branch) and both `crd-crypto-secret-service-*` CTests.

## Next

TPM-held ECDSA signers (K8 `Hardware` placement) through the provider with the TLS private-key method; then the tss2
protector and signers on Linux against a TPM simulator; PIV over PKCS#11 against SoftHSM2.
