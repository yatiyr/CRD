# 2026-11-24: NET-PATH, ring refusals named

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Operator item: CI 45238977369, linux-gcc-asan shard 1, failed
`test_udp_ring.cpp:230`. On that run a 32768-buffer io_uring ring fell back to `recvmmsg` on the hosted machine, while
rings of 4, 8 and 1024 buffers opened in the same case. The same case had passed on that lane before.

## What landed

- **Refusals reported.** A socket that asks for a ring and gets none reports why.
  - `SocketFeatures::ring_refusal` names the step: `Count`, `Off`, `Memory`, `Setup`, `Register` or `Arm`.
  - `ring_error` holds the host's errno at that step.
  - `ring_open` and `rio_open` each read the error before their release. A Registered I/O socket that falls back
    carries its refusal into the plain socket that replaces it.
- **The test asserts the contract, not a host's memory.**
  - An invalid count reports `Count`, or `Off` where the environment or the build has no ring.
  - A ring host lends for 4, 8 and 1024 buffers.
  - The 32768-buffer ring lends, or reports a resource refusal (ENOMEM, EMFILE, ENFILE or EAGAIN at `Memory`,
    `Setup` or `Register`).
  - Every open prints its step and errno through INFO.
- **New case with teeth:** "a ring the host refuses names the step and its errno, and the socket receives through
  recvmmsg".
  - A seccomp filter on one thread answers `io_uring_setup` with EPERM, as Docker's default profile does. The socket
    reports `Setup` and EPERM.
  - `RLIMIT_AS` set 256 MiB above the process's size refuses the 2 GiB ring. The socket reports `Memory` and ENOMEM.
  - Each socket still receives through `recvmmsg`, and once the limits are restored the same open lends again.
  - An earlier version used `RLIMIT_NOFILE` to make setup fail with EMFILE. UBSan's vptr check probes memory through
    a pipe, so on the ASan lane it reported an invalid vptr. Seccomp replaced it.
- **New `udp-ring` host-check leg** ([host-check.yml](../../.github/workflows/host-check.yml)). It builds
  `crd-net-tests` on linux-gcc-asan and records the machine's limits: kernel, ulimits, capabilities, overcommit and
  io_uring sysctls, cgroup memory, meminfo and buddyinfo. It then runs the ring case 20 times alone and then beside
  a whole `crd-net-tests` run on every CPU, under `-s`, and tallies every step and errno. A short local dry run did
  119 loaded rounds beside 32 runs and found no refusal (WSL, kernel 6.6).

## Evidence

- WSL, as user 1000 on kernel 6.6: linux-gcc-debug passes 223 of 223 CTests and linux-gcc-asan passes 58 of 58. Both
  runs matched `io_uring|ring|recvmmsg`, which includes `crd-net-udp-recvmmsg`.
- Teeth: with `Setup` reported as `Register` and the Memory errno forced to 0, the new case failed 3 assertions. With
  the source restored it passes.
- linux-clang-tsan: every ring CTest passes, and the new case passes all 36 of its assertions.
- win-debug: the `[#test_udp_rio]` and `[#test_udp_ring]` cases pass (14 cases, 370 assertions), including the
  Registered I/O `Count` reports.
- LLVM 22 tidy is clean on Windows and on Linux. All the repository validators pass.

## Next

- Read host-check `udp-ring` runs 45289266249 (load fixed) and 45288424193 (its alone arm only). Find which step and
  errno that host refuses, and its limits.
- Fix the ring's sizing at that cause. The hypotheses are the CQ ring of `2 * buffers` entries (1 MiB of kernel
  memory, possibly accounted to `RLIMIT_MEMLOCK` on newer kernels), the pinned 512 KiB buffer ring, and the 2.3 GiB
  buffer region made writable by `mprotect`.
- Nothing is attributed to any of these hypotheses until the host's errno is known.
- Not mine: fuzz host-check 45229585649's failed ckir-binary leg was a 403 on the Ninja download.
