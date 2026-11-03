# 2026-11-03: NET.2, the unbounded key update chain and the Android Keystore (Track C batch 88)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Design: [protocol model](../design/network-foundation.md#protocol-model),
[K15](../design/network-foundation.md#key-store).

## CI first

Nothing to fix: no completed run contains 4bf36dc1 or later; 41646840079's two failures were already fixed or
attributed (1d038940; one bad Vulkan archive download).

## What landed

- **The key update chain, unbounded** (5a77913a): `tls_key_update.spthy` indexes keys by epoch, a private
  `chain(root, e)`; the adversary's one computation over it is HKDF-Expand-Label's step to the next key (`Advance`).
  The induction runs over natural numbers and the eight-key bound is gone. Sessions are named by their fresh root.
- **The Android Keystore** (K15): `PlatformKeyProtector` and the new `PlatformKeyDevice` are "android-keystore" on
  Android, through the application's Java VM (JNI, libnativehelper loaded at run time); a per-master-key AES-256-GCM
  Keystore key, sign-only P-256 and P-384 keys, StrongBox first. Elsewhere the device answers Unavailable.
- **Shared encodings:** `ecdsa_raw_of` (DER to r || s) and `ecdsa_public_of_spki` in provider_internal.hpp; one
  `hex_into` in byte_spans.hpp replaces the three copies in the Secret Service, PKCS #11 and TPM files.

## Evidence

- `check-protocol-model.py`: all four models pass in WSL (18 s, 78 s, 34 s, 41 s); `key_secrecy` 10 steps,
  `record_authentic` and `update_authentic` 9. Teeth: a KeyUpdate sent in the clear falsifies `update_authentic`
  (3 steps); an adversary that can step the chain backwards falsifies `key_secrecy` (16 steps).
- android-x86_64-debug (WSL, pinned NDK r30): crd-crypto and crd-crypto-tests build without a warning;
  android_keystore.cpp, provider.cpp and test_key_store.cpp analyse clean (LLVM 20). The Keystore itself is not run
  yet: a plain executable has no Java VM, so on the emulator its cases answer Unavailable or skip.
- linux-gcc-debug: `[key-store]` 21 passed, 4 skipped; the two device encoding cases pass (40 assertions);
  crd-crypto-pkcs11-softhsm, -tpm-swtpm and both Secret Service CTests pass; the guard CTests pass.
- win-debug: `[key-store]` 23 passed (this PC's TPM), 2 skipped; the encoding cases pass. win-tidy (LLVM 20.1.8): the
  six changed Windows sources analyse clean.
- Host fault, rerun once: GCC segfaulted (internal compiler error) on two hesap-fft AVX-512 codelet units while
  building ceridc's closure; the rerun at 4 jobs built clean.

## Next

The Android Keystore's proof in an application process: a pinned JDK, build-tools and platform, a test APK
(instrumentation, no Gradle) that loads crd-crypto-tests as a library, and the emulator lane running it. Then state
learning, PIV and the Keychain.
