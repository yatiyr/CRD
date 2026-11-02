# NET.2: TPM-held signature keys and the TLS private-key method (2026-11-03)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Contract: [key store K8 and K12](../design/network-foundation.md#key-store).
User direction carried: "I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD STANDARD AND ELITE" (2026-11-01).

## CI first

Dispatch 41508851661 (13c815d3) now has 17 failed jobs; the two new ones (win-relwithdebinfo, win-asan 2/2) and win-vs
fail only `crd-repository-tools`' fuzz-driver test, the batch-62 defect b5a2d1b9 fixed. No completed hosted run holds
382b9d11 or later yet; a complete-tier dispatch at this batch's head follows the push.

## What landed

- `KeyDevice` (create, sign_digest, forget) and `DeviceSigner`: the device signs the digest, r || s becomes P4's DER
  form, and each signature is verified under the stored key before it leaves (a wrong-key or faulty device is caught).
- The store: `Hardware` placement for ECDSA P-256/P-384, the device's reference sealed in place of a private input;
  a curve the device lacks sends `HardwarePreferred` to the file, any other device failure fails the change; device
  keys are deleted before the save that drops them (revoke, destroy, erase) and a key made for an unsaved change is
  deleted again; a store opened without its device signs nothing and changes nothing of its device keys.
- `TpmKeyDevice` ("tpm-pcp"): one persisted sign-only ECDSA key per generation through the Platform Crypto Provider,
  bound to the store by name, a key handle per signature.
- TLS: a device credential's certificate is signed in the device over the re-encoded TBSCertificate and verified
  under its key; each context keeps its own signer copy behind `SSL_PRIVATE_KEY_METHOD`.
- Fuzz: the restated body accepts `Hardware` entries; the change model holds a device and checks its live keys; seed
  corpus regenerated (a device-key change sequence added).

## Evidence

- win-debug (MSVC), this PC's Intel PTT TPM: `[key-store]` 21 cases, 9,880 assertions; the TPM holds P-256 and P-384
  keys, signs a TLS handshake and three concurrent signers, refuses another store's identifier, and revoked and erased
  keys sign no more. No `Cerid` key is left in the provider after a run (`certutil -user -csp "Microsoft Platform
  Crypto Provider" -key`); the TPM cases now erase their store whatever the outcome, after mutated runs leaked keys
  (deleted).
- Teeth (each caught): no signature verification, no device forget on revoke, an unsaved key kept, r and s swapped,
  a certificate signed over a truncated TBSCertificate.
- win-debug: the whole `crd-crypto-tests` (265 cases, 1,447,149 assertions).
- linux-gcc-debug, linux-gcc-asan and linux-gcc-release (LTO links clean): `[key-store],[tls]` 82 cases, 20,025
  assertions (the TPM hardware cases skip, the simulated device's run), and the key-store corpus replay (11 inputs).

## Next

The tss2 protector and device on Linux against a TPM simulator; PIV over PKCS#11 against SoftHSM2.
