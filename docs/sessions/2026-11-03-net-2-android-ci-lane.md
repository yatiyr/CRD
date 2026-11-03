# NET.2: the android job on the own CI

Date: 2026-11-03. Row: [NET.2](../ROADMAP.md#slice-net.2). Commit: 26c32908 (fix(ci)).

## CI first

Dispatch 41568608043 (29844ddc) failed linux-gcc-arm64-debug and linux-clang-arm64-debug on four Python CTests,
all caused by 29844ddc, which added the `android` job and the `android-x86_64-debug` preset to the hosted workflow
only:

- `crd-repository-tools`: the setup-python count (13, now 14); already fixed by 98b3a9b0.
- `crd-build-profiles`: the visible configure preset count (28, now 29).
- `crd-ci-platform` and `crd-ci-orchestrator`: no own-CI device hosted the `android` job (planning raised
  `KeyError: 'android'`), and none of its 15 steps had an orchestrator step in the parity table.

## What landed

- **Device** (`ci/devices.json`): `gce-linux-x64-android-host`, an active n2 VM with nested virtualization on the
  `cerid-ci-ubuntu-2404-x64-android` image family (its provisioning adds the agent user to `kvm`), hosting `android`.
  The planned `android-emulator-phone-x86_64` device stays 6.1's app-launch and Vulkan cell.
- **Lane** (`ci/pipeline.py`): phases `android_ndk`, `kvm` (fails when `/dev/kvm` is not open to the agent, before a
  boot timeout could hide it), `android_emulator`, `emulator_boot` (ANDROID_SERIAL merged for CTest) and
  `device_log` (logcat and emulator logs, then `android-device.py stop`), which runs always, like the hosted
  `if: always()` step. The evidence bundle reads `--adapters` and `--packages` from the phases a job runs.
- The parity table in [ci-platform](../design/ci-platform.md#parity-with-github-actions-ci-4) is regenerated.

## Evidence

- `test-ci-platform.py` 102 tests pass (two new: the android lane's phase order, its always-run collection after a
  failed test, a bundle without packages or adapters, and the `/dev/kvm` refusal); `test-ci-orchestrator.py`,
  `test-native-build-profiles.py` and `test-repository-tools.py` (331) pass.
- check-ci-tiers, check-pins, check-allman-braces, check-generated, check-master-plan and check-repository pass.
- The queued dispatch 41570618552 (98b3a9b0, nothing started) was cancelled and dispatch 41574979037 started at
  26c32908: the first hosted run of the emulator tests and of this fix.

## Next

Audit dispatch 41574979037's android job (KVM, boot time, discovery through the launcher, every crypto CTest and
fuzz corpus on API 31, logcat). Then the Android Keystore protector over JNI in an app process.
