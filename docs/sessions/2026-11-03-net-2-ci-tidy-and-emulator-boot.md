# 2026-11-03: NET.2, win-tidy findings and the emulator's SDK root (Track C batch 77)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). This batch fixed two CI failures this track caused.

## What landed

- **win-tidy** (0222cfed). Dispatch 41568608043 at 29844ddc refused 15 findings:
  - function-local `kCamelCase` constants in the crypto policy, TLS policy and key store tests;
  - nested conditional operators in the key store's entry decoder and its fuzz model.
  The pinned LLVM 20.1.8 gate (`scripts/tidy-files.py`, a fresh `win-tidy` tree) then checked every C++ source
  changed since that run. It found three more: two constants in the ECH tests, one in the ceridc policy tests, and a
  narrowing iterator offset in `TlsEchKey`'s duplicate-AEAD check. All are fixed. The held-length checks are now
  explicit minimum and maximum lengths.
- **Emulator boot** (e608e642). Dispatch 41574979037 was the first hosted run of the emulator lane. Its android job
  built, then failed at boot with "Broken AVD system path": the emulator reads `ANDROID_HOME` before
  `ANDROID_SDK_ROOT`, and the runner image points `ANDROID_HOME` at its own SDK, which has no API 31 image. Fixes:
  - the installer now names the kit as `ANDROID_HOME` too;
  - the launcher gives the emulator the kit's root under both names;
  - a boot that ends early prints the last 40 lines of the emulator log.

## Evidence

- win-tidy (local, LLVM 20.1.8): all 20 changed sources analyse clean.
- win-debug: `crd-crypto-tests` all passed (1,449,061 assertions). The key-store fuzz corpus replays (11 inputs).
  `crd-ceridc-tests "*policy*"` passed (7 cases, 425 assertions).
- The android tests in `test-repository-tools.py` pass on Windows and in WSL. The new case covers a runner whose
  `ANDROID_HOME` points elsewhere.
- In WSL with a foreign `ANDROID_HOME`, the boot passes the SDK and AVD checks. It stops only at `/dev/kvm`, which
  this user cannot open.
- Validators pass.

## Next

Audit the android job of the complete-tier dispatch at e608e642 or later: boot time, test discovery and the crypto
CTests on API 31. Then the Keystore host. The launcher runs bare executables over `adb shell`, so no JavaVM exists:
- the Android crypto tests become a JNI library run by `app_process` with a small dex (pinned JDK and build-tools d8);
- the protector finds the VM through `JNI_GetCreatedJavaVMs`;
- check on the lane whether the shell uid reaches keystore2.

Apple Keychain comes after Android. crd-crypto's closure has no Darwin port yet (crash, CPU features, host
pressure, resource probe, futex semaphore, worker affinity, executable path), and the own CI has no macOS device.
