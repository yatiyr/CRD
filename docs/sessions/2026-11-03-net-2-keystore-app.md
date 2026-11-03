# 2026-11-03: NET.2, the Android Keystore's instrumentation APK (Track C batch 89)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Order: the user's 2026-11-03 order (the Keystore protector proven
in the Android emulator inside NET.2).

## CI first

Nothing to fix: no completed hosted run contains 4bf36dc1 or later (the push runs through 720d8fe3 were superseded
and cancelled; 41713995401 is pending). 41646840079's two failed jobs are 1d038940's fix and one bad Vulkan download.

## What landed

- **Why an APK:** only an installed app runs the native code as an app uid in the `untrusted_app` SELinux domain, which
  is the Keystore as products see it; `app_process` from a shell runs as shell or root (decision K15 now says so).
- **Pins** (testing SDKs): build-tools 37.0.0 and the API 31 platform (size and SHA-1 matched against Google's
  repository index), Temurin JDK 21.0.13+9 (SHA-256 matched against Adoptium's published `.sha256.txt`).
  `install-android-emulator.py` installs them into the kit's SDK root; each must report its pinned version
  (`source.properties`, the platform's level and revision, the JDK's `release`).
- **scripts/android-apk.py:** `check` and `build`: javac `--release 11 -Werror` against android.jar, d8, aapt2 link,
  the library stripped of debug information, fixed-time zip entries, zipalign, apksigner (a debug EC key made once in
  the work directory) and `apksigner verify`.
- **The APK** (`tests/foundation/crypto/android/`): a self-instrumenting `KeyStoreRunner` (TMPDIR set to the app's cache
  directory, Catch2's arguments as extras `argc`/`arg<i>`) and `keystore_app_main.cpp`, the JNI entry that runs
  Catch2 with its output in `files/catch.txt`. The library is `test_key_store.cpp` plus that entry.
- **`android-device.py instrument`:** uninstall, install, `am instrument -w -r`, the report through `run-as`, uninstall
  (which also deletes the uid's Keystore keys); the exit is trusted only when `INSTRUMENTATION_CODE` agrees with it.
- **CMake:** `CRD_BUILD_ANDROID_KEYSTORE_APP` (ON in `android-x86_64-debug`; configure fails without a complete kit),
  the `crd-crypto-keystore-apk` target and the `crd-crypto-keystore-app` CTest running `[android-keystore]`. The
  hosted and own-CI android lanes install the kit before the configure.

## Evidence

- WSL, android-x86_64-debug: configure, then the library, `crd-crypto-tests` and the APK build with no warning.
  `aapt2 dump badging`: package cerid.test.keystore, min/target SDK 31, debuggable; `apksigner verify`: v3 scheme;
  `llvm-readelf --dyn-syms` lists `Java_cerid_test_keystore_KeyStoreRunner_run`; NEEDED is libc, libm, libdl, liblog.
- clang-tidy (LLVM 20, the android tree's database): `keystore_app_main.cpp` and `test_key_store.cpp` clean.
- test-repository-tools.py `-k android`: 8 pass in WSL (the fake-adb instrument run: order of calls, a failed exit, a
  crash, a missing APK); test-ci-platform.py passes (parity table regenerated); every repository validator passes.
- The real kit installs in WSL with all three new archives verified.

## Next

Audit the hosted android job of the first complete-tier run with this commit: the kit before the configure, the APK
build, and `crd-crypto-keystore-app` on the API 31 emulator. Then state learning, PIV and the Keychain.
