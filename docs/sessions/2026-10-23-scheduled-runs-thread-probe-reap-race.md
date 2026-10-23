# Scheduled runs 39746853196 and 39923363836: the thread probe's reap race, 2026-10-23

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c) (the OS thread probe); the two open findings
> below are queued under [DIAG.11b](../ROADMAP.md#slice-diag.11b). Rules: [AGENTS](../../AGENTS.md).

## Goal

`1e0574f1` was green in the nightly runs of 2026-10-20 and 21 and red in
[39746853196](https://github.com/yatiyr/CRD/actions/runs/39746853196) (2026-10-22, `linux-gcc-asan`) and
[39923363836](https://github.com/yatiyr/CRD/actions/runs/39923363836) (2026-10-23, `linux-clang-tsan`, `win-asan`).
Same commit, so each failure is an intermittent defect to explain. This batch fixes the first; the other two are
measured below and taken by the next batches.

## linux-clang-tsan: a thread reaped between open() and read()

`diag10c: the OS sees a thread this process started until it has been joined` failed at its post-exit check with
`3 == 1`: `os_thread_alive` answered `Unavailable`, not `Absent`, for a thread that had just returned. No TSan report.

Cause: `read_task_stat` opens `/proc/self/task/<tid>/stat` and then reads it. When the kernel reaps the task in
between, the open succeeds and the read fails with `ESRCH` (procfs `proc_single_show` finds no task). The probe
treated that as an unreadable file. Repair: a read that fails with `ESRCH` marks the task missing, so the probe answers
`Absent`, which is what the thread is.

Test: `diag10c: a thread asked about while it exits reads as present or absent, never unavailable` starts 300
threads that return at once and probes each without pause until it is no longer present. Before the repair it failed
on all three runs on `linux-gcc-debug` (mirror tree), with 76, 52 and 45 of 300 threads read as unavailable; repaired
and rebuilt, 0 of 300 on three runs. The `diag10c` CTest set passes on `linux-gcc-debug` (119) and `win-debug` (131).

## Measured, not yet repaired

### win-asan: TP-1's registered set is not stable on WARP 10.0.26100.33648

The gate reported `UNEXPECTED PASSED: registered TP-1 did not fail: DX12 cached DXR pipelines mint one Program identity
each, retired with the context`; the other four TP-1 tests failed as registered, each the 8-byte `memmove` read in
`d3d10warp.dll+0x18012f3f5` 0 bytes past a `D3D12Core.dll` copy (2,924, 3,976, 2,924 and 3,788 bytes). On 2026-10-22
(image `20261018.275.1`) the same driver build, the same WARP and D3D12Core code offsets and an identical DX12 census
gave all five failures (the identity test past a 2,952-byte copy), gate PASS. The identity test passed on the 23rd
with all 20 assertions, ASan clean; `REN-38-F13` was read past a 3,976-byte copy instead of the 2,568-byte one of
2026-10-17, 18 and 22. So which library copy WARP over-reads varies between runs of the same binaries.

Workstation, WARP `10.0.26100.9711`: the SDK-only probe of 2026-09-12 (no Cerid code) still reproduces TP-1, 20 of 20
runs, always past the miss library's 2,496-byte copy. The defect is WARP's; the open question is what makes its
target vary on hosted runners, since the register needs a deterministic set.

### linux-gcc-asan: LeakSanitizer reports in the Vulkan driver on runner image 20261018.339.1

418 of the 419 failures are LeakSanitizer reports at exit of every lavapipe device test: a 128-byte direct leak
(`realloc` under `__pthread_once_slow`) and 56-byte indirect leaks in threads started from an unloaded module, reached
from `vkEnumeratePhysicalDevices` in `vulkan_context.cpp:721`. The remaining failure is the registered TP-5 oracle.
That job ran on image `20261018.339.1`; every passing `linux-gcc-asan` job ran on `20261012.334.1`. The apt step
installs the same packages and versions on both (Mesa `25.2.8-0ubuntu0.24.04.4`, LLVM 20.1.2); the only difference
is `libdebuginfod1t64`, preinstalled on the new image. That image's release notes are not published, so the
difference between them is not known yet.
