# NET.2: the PKCS #11 key device (2026-11-03)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Contract: [key store K14](../design/network-foundation.md#key-store).
User direction carried: "I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD STANDARD AND ELITE" (2026-11-01).

## CI first

Dispatch 41508851661 (13c815d3): its last three jobs (linux-clang-tsan, win-shipping-profile, win-public-checks) fail
only `crd-repository-tools`' fuzz-driver test, the batch-62 defect b5a2d1b9 fixed. Push run 41521847645 (57c51e55,
which holds that fix) has 20 jobs passed and none failed. Dispatch 41542040132 (97fe4a37) is still running.

## What landed

- `Pkcs11Token` and `Pkcs11KeyDevice` ("pkcs11", `pkcs11_device.cpp`), as K14 states; ykcs11's sources were read
  first (generation needs the SO login and a slot `CKA_ID`; `C_DestroyObject` deletes a slot's certificate only), so
  the PIV placement is the next slice and needs no change to this interface.
- Pins: `pkcs11` (the OASIS TC's repository at c55388c0; `published/3-02` byte for byte the OASIS Standard's
  `include/pkcs11-v3.2`) and `softhsm-linux-x64` (SoftHSM 2.6.1 and OpenSC's pkcs11-spy, four Launchpad packages,
  `scripts/install-softhsm.py`, a CI step on linux-gcc-public-checks, the own CI's `softhsm` step).
- `crd-crypto-pkcs11-softhsm` (`pkcs11_token_session.py`, `CRD_BUILD_PKCS11_VS_SOFTHSM`).
- Registers: `oasis.pkcs11`; `cerid.key-store` restated; the inventory regenerated and cerid-package's findings
  restated (the device interface carries signatures only). `check-registers.py` accepts `oasis` specifications and
  holds only `rfc` and `draft` entries to the snapshot's date, the only records the snapshot keeps (tested).

## Evidence

- linux-gcc-debug, -asan and -release (LTO link clean): `[key-store]` against the token, 23 cases (21 passed, the
  two TPM cases skipped), 9,927 assertions; the token left with no object; the wrong PIN refused by every call and
  recorded as one `C_Login` (`CKR_PIN_INCORRECT`).
- win-debug (MSVC): the contract case, 72 assertions (kernel32.dll as the library that is no module).
- Teeth, each caught: forget by handle alone, no `CKA_SENSITIVE`, the PIN tried again, a search without the curve,
  a signature search without the label.
- LLVM 20 tidy clean on the three changed C++ files; repository tool, CI platform and module-selection tests.
- One `[key-store]` run on linux-gcc-debug aborted in Catch2's output redirect after a skip and passed on the rerun
  (this host's known misexecution).

<a id="tp-12"></a>
## TP-12

Under linux-gcc-asan the spy step reports 16 bytes lost inside pkcs11-spy's `C_GetFunctionList`; a C program that
only loads the spy and calls that function reproduces it, SoftHSM alone does not. Registered as
[TP-12](../third-party-defects.md#tp-12); no hosted lane runs the session under a sanitizer.

## Next

The PIV placement over ykcs11; then the first policy generation and the dated classical-only refusal.
