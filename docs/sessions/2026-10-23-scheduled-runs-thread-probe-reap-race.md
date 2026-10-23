# Scheduled runs 39746853196 and 39923363836: the thread probe's reap race, 2026-10-23

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10c](../ROADMAP.md#slice-diag.10c) (the OS thread probe); the WARP and the
> LeakSanitizer findings are repaired under [DIAG.11b](../ROADMAP.md#slice-diag.11b) pending their hosted evidence.
> Rules: [AGENTS](../../AGENTS.md).

## Goal

`1e0574f1` was green in the nightly runs of 2026-10-20 and 21 and red in
[39746853196](https://github.com/yatiyr/CRD/actions/runs/39746853196) (2026-10-22, `linux-gcc-asan`) and
[39923363836](https://github.com/yatiyr/CRD/actions/runs/39923363836) (2026-10-23, `linux-clang-tsan`, `win-asan`).
Same commit, so each failure is an intermittent defect to explain. All three are repaired below.

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

## Measured

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
is `libdebuginfod1t64`, preinstalled on the new image. Repaired below, 2026-10-23.

## linux-gcc-asan: the Vulkan loader unloads drivers before LeakSanitizer runs

Every report's driver frames are `<unknown module>`: by exit the loader had `dlclose`d the module. LeakSanitizer scans
only mapped modules, so a block a driver keeps for the process (here one allocated under `pthread_once` in
`vkEnumeratePhysicalDevices`, holding the blocks of the threads the driver started) is reported as leaked once its
module is gone, and nothing in the report names the driver. On the WSL host, `LD_DEBUG=files` on
`crd-gpu-context-vulkan-tests "v17-i-a*"` shows the loader destroying seven Mesa link maps (six probed drivers and the
`device_select` layer) before exit. Image `20261018.339.1` was never released: the newest ubuntu24 release,
`20261023.334.3`, lists `20261012.334.1` as its previous image and differs from it only in Bazel, and the run of
2026-10-23 was green on `20261012.334.1`. Which of its differences made a driver keep that block is not knowable from
here (the WSL host has Mesa `25.2.8-0ubuntu0.24.04.2`, the hosted lanes `.4`; no report reproduces locally).

Repair, the whole class: the `linux-gcc-asan` test preset sets the loader's own switch,
`VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1` (loader 1.3.275 on both hosts), so every driver and layer stays mapped
until exit; its globals are scanned and its frames symbolized, and a block it really loses is still reported with a
named driver frame. No suppression. With it the same run destroys no Vulkan link map. Teeth: with the preset entry
removed and the mirror synced, `VK_LOADER_DEBUG=warn wsl-lane.sh test linux-gcc-asan -V` on `v17-i-a`, `raf2b` and
`DIAG.7a(f)` prints no "library unloading is disabled" line (0); restored, 3 of 3. With it, `diag 3f: a memory leak is
caught by LeakSanitizer` still reports the specimen's leak and passes, and `raf2b`, `raf7` and the rebuilt
`crd-gpu-context-vulkan-tests` under CTest (309 of its 312 tests finished inside the 19-minute bound: 308 passed, the
shading-rate gate skipped on lavapipe) print no LeakSanitizer report.

## win-asan: the library tail keeps WARP's read inside the runtime's copy

Every TP-1 report is an 8-byte `memmove` read starting exactly at the end of a `D3D12Core.dll` block the size of one
DXIL library: the runtime copies `BytecodeLength` bytes and WARP reads 8 more. Which library it reads past varies on
hosted runs, so no stable registered set exists; the repair removes the out-of-bounds read instead of describing it.

SDK-only probe (the 2026-09-12 program with each library copied into a buffer `pad` bytes longer, zero-filled, and
that length passed as `BytecodeLength`), workstation WARP `10.0.26100.9711`, ASan, debug layer and unfiltered
InfoQueue callback on: `pad=0` three runs, each the read 0 bytes after the 2,272-byte region; `pad=8`, `16` and `64`
`CreateStateObject returned 0x00000000 errors=0 warnings=0`, no report. So WARP reads at the container's end, not at
`BytecodeLength`, and the runtime accepts the longer bytecode.

Repair: `dxr_pipeline` copies the libraries of a pipeline it builds into one array, each followed by 8 zero bytes, and
passes container + 8. Exactly 8: a provider that reads further still reaches the sanitizer. The content keys stay on
the bare DXIL. The work-graph state object takes no tail; `CEIR-20c-1 ... runs as a D3D12 WORK GRAPH` passed on
`win-asan` in both runs. The five `win-asan` registrations are removed from the register; TP-1 records the handling
and retires when the probe without the tail is clean.

Local evidence (hardware adapter; this box cannot run the engine on WARP without changing a system setting):
`win-debug`, with `crd-gpu-context-dx12-tests` and `crd-scene-render-tests` rebuilt, the 23 CTests matching `CEIR-19b`,
`REN-38-F6`, `DXR`, `REN-38 RT` or `REN-38-F13` pass, the five former TP-1 tests among them; `win-asan`, the 8 DXR CTests of `crd-gpu-context-dx12-tests` pass with no
report. Hosted evidence still required: the next `win-asan` run on WARP with the gate line
`gate: no registered failures for this lane`, the five tests `Passed` and no AddressSanitizer report.
