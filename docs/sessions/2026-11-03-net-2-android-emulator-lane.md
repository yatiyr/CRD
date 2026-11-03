# 2026-11-03: NET.2, the Android emulator lane (Track C batch 72)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Order: the user's 2026-11-03 order (the Android Keystore protector
proven in the Android emulator, on GitHub's Linux runners with KVM).

## CI first

Dispatch 41567456066 (fc970c7c) stopped in preflight: `android-x86_64-debug` had no tier owner (check-ci-tiers.py),
so every hosted run since dc332572 failed before any lane. Fixed in 29844ddc: a complete-tier `android` job owns the
preset (`.github/ci-tiers.json`, the required aggregate, docs/design/ci-tiers.md). Its first hosted run (dispatch
41568608043) configured and built the preset with the pinned NDK: the job passed.

## What landed (98b3a9b0)

- **Pins**: platform-tools 37.0.1, emulator 37.2.13 (build 16740729) and the API 31 default x86_64 system image
  (index revision 5), each size and SHA-1 matched against Google's repository indexes on retrieval, SHA-256 pinned.
- **scripts/install-android-emulator.py**: the three into one SDK root through `pins.install_tree` (platform-tools and
  the emulator must report their revision; the image its API level, ABI and tag), then the AVD.
- **scripts/android-device.py**: `boot` (headless, `-accel on`, waits for `sys.boot_completed`), `stop`, and `exec`,
  the Android preset's `CMAKE_CROSSCOMPILING_EMULATOR`: host path P is mirrored at `/data/local/tmp/cerid/host` + P;
  the program, its `crd_test_data` paths and host-path arguments (fuzz corpora) are pushed when their stamp differs,
  under a host lock; the program runs in a device directory per host working directory with TMPDIR and the CRD_*
  variables; its exit code comes back through adb's shell protocol.
- **CMake**: `crd_test_device_path` and `crd_test_data` (cmake/CrdTestDiscovery.cmake); crd-crypto-tests reads its
  four vector sources from their device mirrors on Android. The `android` job boots the emulator after the build and
  runs `ctest --preset android-x86_64-debug` with the inventory, register gate, logcat and evidence bundle.

## Evidence

- test-repository-tools.py: 331 tests pass on Windows; in WSL the three Android tests pass, among them `exec`
  end to end against a fake device (exit code 7 returned, arguments rewritten, CRD_ANDROID_* withheld, an unchanged
  tree not pushed again, a changed corpus pushed alone).
- WSL: the real kit installs (all three SHA-256 verified, revisions reported), adb 37.0.1 runs, the emulator lists
  the AVD. The android-x86_64-debug tree configures with the launcher in Catch2's TEST_EXECUTOR and every fuzz
  corpus CTest, and the vector paths compiled into crd-crypto-tests are the device mirrors.
- check-ci-tiers, check-pins, pinned actionlint, check-repository, check-generated, check-master-plan pass.
- The hosted emulator run is dispatch 41570618552 (98b3a9b0); it was still queued behind dispatch 41568608043 when
  this note was written. This PC cannot boot it: WSL's user is not in the `kvm` group.

## Next

Audit dispatch 41570618552's android job (boot, discovery through the launcher, every crypto CTest on API 31). Then
the Keystore protector: from AOSP's sources and SELinux policy, decide how a test process reaches AndroidKeyStore
(a test APK with the library loaded in an app process, or `app_process` with a dex), write it over JNI and prove it in
this lane.
