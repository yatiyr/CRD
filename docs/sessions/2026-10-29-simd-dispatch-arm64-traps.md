# 2026-10-29 — SIMD-DISPATCH: ARM64 traps, compact switch tables and a lookup that may miss

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §6](../decisions/0134-runtime-cpu-dispatch.md);
follows [the pocketfft peer](2026-10-29-simd-dispatch-pocketfft-peer.md).

## CI first

Push run 40826432863 (dd860168) is still running. Its later push runs were replaced by the concurrency group before
any job started. Five of its jobs had failed, all read from their logs:

- linux-gcc-release: the MTTKRP oracle's `fids[5]`, fixed by f5be91c4. The pending run of baebebea is its first
  proof.
- linux-gcc-relwithdebinfo: `-Wnull-dereference` at the link of crd-resources-tests, `test_eviction.cpp:374`. The
  test read `rm.find_entry(id)->blob_size`, a lookup that may miss, at four sites. GCC named one site, because a link
  lists only its first failing partition. All four now go through `ev_blob_size`, which `REQUIRE`s the entry.
- linux-gcc-arm64-debug and linux-clang-arm64-debug: the guard's branch-target reads (fixed by baebebea) and three
  crash cases, below.
- win-arm64-debug: 42 guard failures. 23 were the branch-target reads. The other 19 are below.

DIAG.11c and DIAG.12 stay Needs CI: no completed run carries their hosted evidence yet.

## What landed

- **Trap signals on Linux.** On AArch64 `__builtin_trap` (and so `CRD_DEBUGBREAK`) is `brk`, which raises SIGTRAP
  (exit 133). The handler did not install for it, so no record was written; the build-id case reads that record.
  - The handler now records SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL and SIGTRAP from one table. Install, the mask,
    the chain and uninstall all walk the table, so no list can miss a signal again.
  - SIGTRAP is re-raised rather than re-executed, because x64's int3 leaves the instruction pointer past it.
  - A first install that fails part-way puts back the dispositions it had already replaced.
  - SIGBUS was missing too, though the crash runbook names it. New mode `bus` reads a shared mapping of an empty
    memfd (BUS_ADRERR). The TSan specimen sets `handle_sigbus=0`, as it does for SIGFPE.
- **FPUs that cannot trap.** ARMv8 makes the floating-point trap enables optional, and the hosted ARM64 cores
  implement none: `feenableexcept` returns -1 and the division never traps (exit 61). The `fpe` mode now exits 62
  when the trap cannot be armed. The case probes the same capability in-process: a host that can arm the trap must
  record SIGFPE, and one that cannot must exit 62 with no record.
- **Compact ARM64 switch tables.** MSVC's byte and halfword switch tables (`adr x9,T; ldrb w8,[x9,w8 uxtw #0]`) sit
  after the function's code with no `__swt` label, and dumpbin decodes them as SVE (`E4DED8D2  st1h ...`). An ADR
  whose register an indexed load reads within four instructions now starts data to the next function. The switch's
  second ADR, the address of its cases' code, is read by no load, so a case body stays judged. Both forms are decoded
  from the instruction word, not dumpbin's text.
- PRINCIPLES §3: a header-only reference runs through its peer library, never in the bench unit.

## Checks

- win-debug: crd-resources-tests, crd-core-tests and the crash specimen build; `[eviction]` passes (6 cases).
- linux-gcc-debug mirror tree: the four touched Linux objects compile with the tree's warnings as errors. The
  specimen runs once: `bus` exits 135 with a record of signal 7 code 2, `fastfail` 132 (signal 4), `fpe` 136.
- `test-repository-tools.py IsaIsolation` (16 cases, a compact-switch fixture with both ADRs and an SVE word in a
  case body). No ARM64 compiler on this host, so only the next win-arm64 run can judge the table rule, and the
  ARM64 Linux lanes the trap and FPU cases.
- tidy-files.py on the five changed C++ files, the ctest guards and the repository validators.

## Left

The Boost peers (a download, awaiting the user), the no-peer-header check after them, and the row's other Left
items.
