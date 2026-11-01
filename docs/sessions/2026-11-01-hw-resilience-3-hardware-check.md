# 2026-11-01 — HW-RESILIENCE.3: the hardware check (`ceridc doctor --hardware`)

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the tensor self-check](2026-11-01-hw-resilience-3-tensor-self-check.md).

## CI read

- Completed runs: push runs up to 41291103424 were cancelled by concurrency and have no failed jobs.
- Running complete-tier run 41274380605 (`de5e10b0`): win-shipping-profile, win-public-checks, win-debug-scalar and
  win-asan shard 2 failed only `filesystem: the last write time converts exactly ...`, fixed on main by cc5ee9e9.
  Its other failures and those of 41269105413 (`b0b1b245`) were fixed on main before.
- Needs CI audit: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2) is reclassified below. DIAG.11c and DIAG.12
  still need a green complete-tier run; none was dispatched (one is running, one waits on its Required result).

## HW-RESILIENCE.2: the ARM64 clause moves to JOBS-PORT.2

The clause asked the ARM64 lanes to pass the diagnostic host's wild-write case. Those lanes configure
`CRD_MODULES_WITHOUT=jobs`, and crd-perf links crd-jobs, so crd-perf-tests is not built there: in 41274380605 the
linux-gcc-arm64-debug (job 124764040324), linux-clang-arm64-debug (124764040332) and win-arm64-debug (124764040284)
jobs pass all 2,628, 2,628 and 2,632 tests they run, and none is a crd-perf case. The
[third recording's audit](2026-11-01-diag-11b-third-recording.md#needs-ci-audit) counted those lanes' whole suites as
passing the case; it never ran there. No run can meet the clause before
[JOBS-PORT.2](../ROADMAP.md#slice-jobs-port.2) gives ARM64 a fiber context, so the clause moves there verbatim as a
platform acceptance and HW-RESILIENCE.2 is Done: the decoder's 2,097,152-word sweep against LLVM 20 stands, and the
real capture's decoding on ARM64 hardware is JOBS-PORT.2's.

## The hardware check

`crd/perf/hardware_check.hpp` (`run_hardware_check`) runs one thread pinned to every logical CPU, all at once, each arm
for the same time. The arms' work is Cerid-free, each result checked against an answer evaluated at compile time or
derived from the work's position:

- `reload`: the 2026-10-26 control's getter-chain shape. 4,096 nodes form one cycle (Hull-Dobell: multiplier 1029,
  increment 17). Every link, owner and index is reloaded through volatile out-of-line getters, and each link is
  compared with the node its index names (an integer division). A wrong link is reported and never followed.
- `compute-<level>`: `crd/perf/hardware_check_kernels.hpp`, a new kernel family (`hwcheck`: portable, sse2, avx2,
  avx512, neon) with one generic loop over each level's lanes. It runs integer multiply-shift-xor, logistic-map and
  divide recurrences on 16 lanes, every step folded into a checksum lane. No multiply feeds an add, so every level
  gives the compile-time bits. Each 64-step block is compared bit for bit, at every level the selection includes.
- `memory`: moving inversions over each CPU's buffer (`memory_mib`, default 8). The pattern is
  `crd::splitmix64_mix` of position and pass. The buffers are pages of one committed `crd::vm` range, refused past half
  the machine's memory.
- `shared`: one sequence-locked cache line per CPU, written by its CPU and read by the next pinned one. A read whose
  sequence held but whose payload is not that sequence's is an event. Each CPU runs on until it has checked one
  consistent read, so a neighbour preempted inside a write cannot leave the arm unchecked.

The report gives, per CPU (group, number, CPUID 0x1A core type) and arm, the blocks, time and events, and for the
first event its block, place, value read and value expected. A CPU the OS refuses is listed unpinned. Pinning is
crd-core's (`logical_cpu_count`, `logical_cpu_at`, `pin_current_thread`); the CPU selection's walk now uses the same
calls.

`hardware.check` is a built-in diag command under the Execute grant, so a read grant can never start the load.
`seconds=` and `memory_mib=` stay within the host's bounds (`DiagServiceConfig::max_hardware_check_ns` and
`max_hardware_check_mib`); the defaults are lowered to them. The verdict is `no-event-seen` or `events-seen`: no event
proves only that none was seen in that time. `ceridc doctor` answers `diag.capabilities`; `--hardware` (with
`--seconds`, `--memory-mib`) runs the check through a read and execute service and prints every page, one per line. It
exits 1 when a page is not ok or an arm saw an event. A fourth fault seam, `install_hardware_check_fault_hook`
(compiled out of shipping), corrupts the first value an arm reads on a named CPU. The four seams now share one
`HookSlot`.

This host: 32 CPUs pinned (16 performance, 16 efficiency), reload, compute-portable, -sse2, -avx2, memory and shared,
0.5 s each, no event; 3.3 s in all. At 10 s an arm (8 MiB a CPU), win-debug, still no event in 60 s.

## Tests and evidence

- `hardware check:` cases (crd-perf-tests; the two `[kernels]` cases also at every forced level):
  - Every arm runs on every pinned CPU with no event, and a refused CPU runs nothing.
  - The seam's value is found once by each arm, on that CPU only, at place 0, one bit off.
  - Every level gives the portable bits over 1,000 steps.
  - Out-of-bounds configurations are refused, and a cancelled run stops after one block.
  - `hardware.check` needs Execute, refuses nine bad arguments, lowers the defaults to the host's bounds and reports
    `events-seen` with the injected item.
- `doctor:` case (crd-ceridc-tests): the real binary's `doctor`, `--hardware` and a refused `--seconds 0`, and the
  verb failing in process under the seam.
- The fuzz target lists `hardware.check` among the commands with argument checks and bounds it to 1 ns and 1 MiB; a new
  seed covers the command (57 seeds).
- Teeth:
  - A wrong compile-time answer (3.98) and an avx2 shift by 30 each failed the no-event case.
  - Disabling the reload, compute, memory or shared comparison each failed the seam case for that arm alone.
  - Restored, rebuilt, passing.
- win-debug: crd-perf-tests (all 356 cases), crd-ceridc-tests and the fuzz corpus pass; forced portable, sse2, sse4.2
  and avx2 pass. SDE 10.13.1 `-skx` runs compute-avx512 with no event (SDE lets one CPU be pinned).
  linux-gcc-debug: the cases, forced arms and CPU selection cases pass. Its first build hit a GCC internal compiler
  error on `test_diag_commands.cpp`, which did not recur on the rerun: the host's known misexecution.
- Guards 21 of 21. `crd-check-capability-owners` found a copy of the SplitMix64 finalizer in the first draft, now
  core's `seed_generators.hpp`. In two parallel guard runs that check stopped on CPython's "internal error in
  regular expression engine" at a search over unchanged inputs; alone it passes: the host's known validator fault. Strict tidy and clang-format are clean on the changed lines.

## Left on the row

The device path and the state-hash divergence check.
