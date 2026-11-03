# NET.2: the TPM 2.0 on Linux through tss2 (2026-11-03)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Contract: [key store K13](../design/network-foundation.md#key-store).
User direction carried: "I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD STANDARD AND ELITE" (2026-11-01).

## CI first

Dispatch 41508851661 (13c815d3): the five jobs finished since batch 66 (win-clang-cl, win-shipping, win-release,
win-debug-scalar, win-debug) fail only `crd-repository-tools`' fuzz-driver test, the batch-62 defect b5a2d1b9 fixed.
Dispatch 41542040132 (97fe4a37, complete tier) is still queued.

## What landed

- `TpmConnection` (a tss2 TCTI configuration, at most 255 bytes; empty: tss2's default order) for `TpmKeyProtector` and
  `TpmKeyDevice`; Windows ignores it.
- `tpm_tss2.cpp` ("tpm-tss2"): ESYS, the TCTI loader and MU loaded at run time; per call its own context under one
  lock, a TPM whose owner hierarchy has an authorization refused before any owner command, the TCG ECC P-256 storage
  root made transiently and an HMAC session salted to it with AES-128-CFB parameter encryption. The master key, or a
  sign-only ECDSA P-256/P-384 key's TPM-wrapped blob, in a write-once, write-locked owner NV index authorized by
  HMAC-SHA256 of the store's identifier under the reference's secret (`NO_DA`, `NODA`); forget proves the
  authorization, then deletes the index. Reference: format, index handle, secret (37 bytes).
- Pins `tpm2-tss` 4.0.1 (headers) and `swtpm-linux-x64` (swtpm 0.7.3, libtpms, tss2 with the swtpm and pcap TCTIs;
  `scripts/install-swtpm.py`, sharing `scripts/ubuntu_packages.py` with the gnome-keyring installer).
- `crd-crypto-tpm-swtpm` (`tpm_simulator_session.py`, `CRD_BUILD_TPM_VS_SWTPM`, linux-gcc-public-checks): every
  `[key-store]` case against a fresh simulator, then `[.tpm-bus]` recorded by the pcap TCTI (the canary master key
  appears nowhere in the traffic, which holds its NV_Write and NV_Read), then the owner hierarchy given an
  authorization and `[.tpm-owner-auth]` (protector and device Unavailable).
- Registers: `tcg.tpm2` and `cerid.key-store` restated; inventory regenerated, cerid-package's review restated.

## Evidence

- `crd-crypto-tpm-swtpm` on linux-gcc-debug and linux-gcc-asan, and the session on linux-gcc-release (LTO link
  clean): `[key-store]` 21 cases, 9,880 assertions (the TPM's protector and device cases run: P-256 and P-384 in the
  TPM, a TLS handshake, three concurrent signers, revocation and erasure), `[.tpm-bus]` (20,444 bytes recorded, the
  master key in none) and `[.tpm-owner-auth]`. Without a TPM (the default run, every Linux lane): 19 passed, the two
  hardware cases skipped.
- win-debug (MSVC), this PC's Intel PTT TPM: `[key-store]` 21 cases, 9,892 assertions.
- Teeth, each caught: no encryption on NV_Read and on NV_Write (the canary seen on the bus), forget not deleting the
  index, an authorization that ignores the store, an index of other attributes accepted, r and s swapped.
- LLVM 20 tidy clean on the four changed sources; repository tool (322), CI platform and module-selection tests.

## Next

PIV over PKCS#11 against SoftHSM2; then the first policy generation and the dated classical-only refusal.
