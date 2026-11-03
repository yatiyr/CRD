# 2026-11-03: NET.2, the Android build of crd-crypto's closure (Track C batch 71)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Order: the user's 2026-11-03 order (the Android Keystore protector
done inside NET.2, proven in the Android emulator). This slice makes the protector buildable: the NDK is pinned and the
whole crd-crypto closure compiles and links for Android.

## CI first

Dispatch 41542040132 (97fe4a37) had one failed job when read: win-release, `diag 8c: program.inspect bounds its
stops and stops when cancelled`. Not this track's code: a release build spends the plan's default 2^24-step budget in
under 100 ms, so the 50 ms outlast case ended `fuel-exhausted` and answered Ok. Fixed under defer-nothing in face8c47
and 2d20f903: `ProgramInspectCommand::max_steps` reaches `plan::run` through `InspectScript`, `InspectHost::start`
and `Session::run`; the outlast and cancel cases run at 2^48 steps and a new case proves a small budget ends the run.
Evidence: win-release `diag 8c` passed 20 repeats; all 120 ceir-cook tests passed on win-debug.

## What landed

- **NDK pin** (dc332572): `tools/android-ndk-linux-x64`, NDK r30 (30.0.16248370), size and SHA-1 matched against
  Google's SDK repository index on retrieval, SHA-256 pinned. `pins.install_tree` reads a zip as a tar (`ZipTree`:
  Unix modes kept, Info-ZIP symbolic links kept, the same data filter); `scripts/install-android-ndk.py` checks
  `source.properties`. Installed in WSL: 9494 files, 39 links, its clang runs.
- **Preset** `android-x86_64-debug`: the NDK toolchain, API 31, `CRD_MODULES=crypto` (the closure and its tests).
- **Portability**, each behind its guard: `CRD_OS_ANDROID` in core/platform.hpp (Linux kernel, bionic); the Secret
  Service and tss2 TPM paths answer Unavailable on Android; worker affinity through `sched_setaffinity` and
  `pthread_gettid_np`; the diagnostic allocator's stack capture through `_Unwind_Backtrace` (bionic's `backtrace`
  needs API 33); `file_clock::to_sys` under libc++ (no `clock_cast`); a units parameter renamed from bionic's
  `si_value` macro.

## Evidence

- `android-x86_64-debug`: configure and `crd-crypto-tests` build and link (169 steps, 0 warnings, an x86-64 PIE for
  `/system/bin/linker64`).
- win-debug: crypto 276 tests passed. linux-gcc-debug: crypto 278, jobs 218, platform 67, units 177 passed.
- `test-repository-tools.py`: the NDK installer test passes on Windows (links become hard links) and in WSL.

## Next

The Keystore protector: decide from AOSP sources how a test process reaches `AndroidKeyStore` (an `app_process` dex
that loads the test library, or an app context), write it over JNI (an AES-GCM key that never leaves the Keystore
wraps the master key), then the emulator lane (pinned system image, a GitHub Linux runner with KVM). Local emulator
runs in WSL need the user in the `kvm` group: `sudo usermod -aG kvm $USER`, then restart WSL.
