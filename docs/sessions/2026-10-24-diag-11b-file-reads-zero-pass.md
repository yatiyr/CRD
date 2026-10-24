# DIAG.11b (7): the file readers' zero pass, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133 ID-5](../decisions/0133-runtime-diagnostics-and-instrumentation.md#implementation-decisions). Rules:
> [AGENTS](../../AGENTS.md). Previous batch: [mixed I/O workload](2026-10-24-diag-11b-mixed-io-workload.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. The eleven completed
runs since 40032224637 (green) were cancelled with no failed job. 40052612807 (head `90e9a299`, still running) has
17 failed jobs: every lane fails only `crd-diag-coverage` and both repository jobs only
`test_committed_registry_describes_the_tree`, which `9583f26b` repairs. 40063771851 (head `eb4daf6c`) was pending.

## Cause

The previous batch's follow-up: under cachegrind, 36% of `io`'s instructions were one line of
`crd/containers/array.hpp`, `resize(n)`'s value-initialization loop. Two defects:

- The loop indexed the `m_data` member. A store of a byte-sized `T` may alias that member, so every compiler reloaded
  it per element. HEAD on win-shipping (cdb): `mov rcx,[rdi+8]; xor edx,edx; mov [rax+rcx],dl; inc; cmp; jb`, six
  instructions a byte; GCC the same shape.
- `read_file_binary` and `read_file_range` zeroed the whole buffer and then read over it. Both fill every byte or
  clear `out`, which is the contract of `Array::resize_uninitialized`.

## Change

- `Array::resize(n)` and `resize(n, fill)` construct through local pointers (`slot` up to `last`). No new branch or
  include; correct for every `T`. An index loop over a local base was tried first: whole-tree linux-gcc-shipping
  (keep-going) failed one link, `crd-ceir-gpu-cuda-tests`, with `-Werror=aggressive-loop-optimizations` at
  `ckir_eval.hpp:194` (`rstk.resize(rstk.size() - 1)`): with the base invariant, GCC's link-time pass bounds the
  wrapped empty-array path. The pointer loop builds the whole tree with no `warning:` line.
- The two readers call `resize_uninitialized`. `crd-platform-tests`' binary round trip now also reads a range past the
  end and requires `false` and an empty `out` (teeth on win-debug: without the range reader's `out.clear()` it fails,
  `past_end.empty()`; restored, rebuilt, green).

Code after the change: GCC 13.3 (`-O3` probe TU) calls `memset` for `Array<u8>::resize(n)` and for
`Array<int>::resize(n, -1)`. MSVC (win-shipping-profile, cdb) runs a five-instruction store loop with no reload, not
a `memset` (it formed one for the index loop that GCC refused); reported, since `io` and the other file readers no
longer zero on the hot path.

## Counts

`diag-overhead.py instructions --workloads io --repeats 3`, linux-gcc-shipping with profiling (off) against
linux-gcc-shipping (compiled-out), cachegrind, per frame:

| Source | Compiled-out | Off | Off vs compiled-out (1%) |
| --- | ---: | ---: | --- |
| HEAD `eb4daf6c` (`39a30cd83d22`, `926d66da20a2`) | 1,313,551 .. 1,328,772 | 1,303,602 .. 1,304,872 | -1.89% .. -0.66%, within |
| `resize` through a local base, readers unchanged | 974,684 .. 976,284 | 964,066 .. 965,527 | -1.25% .. -0.94%, within |
| plus readers without the zero pass | 729,894 .. 731,279 | 718,801 .. 720,717 | -1.71% .. -1.26%, within |
| pushed source (`cdee82c2cfbe`, `0c0a380b4899`) | 728,941 .. 729,761 | 718,636 .. 719,821 | -1.52% .. -1.25%, within |

`cg_annotate` of an off run at 300 frames: at HEAD the `array.hpp` line is 186.8 M instructions (42.9%), first; with
the local-base loop it is gone and glibc's `memset` takes 79.0 M (24.2%); with the readers changed neither appears.

## Board

`diag-overhead.py run --workloads io --processes 96 --iterations 200 --warmup 20 --affinity 5555`, the pushed
source's pair, host idle (`build-bench.py host`):

| Arm | p50 / p99 µs | Frame cost (95%) | CPU cost (95%) | Verdict |
| --- | --- | --- | --- | --- |
| compiled-out | 172.0 / 281.3 | reference | reference | |
| off | 171.8 / 271.1 | -0.11% (-0.53 .. +0.09) | -0.53% (-1.03 .. -0.08) | within (1%) |
| basic | 172.0 / 283.1 | -0.08% (-0.13 .. +0.20) | +0.08% (-0.25 .. +0.33) | within (2%) |
| full | 177.6 / 288.7 | +3.24% (+3.07 .. +3.46) | +2.92% (+2.24 .. +3.66) | reported |

The frame fell from 202 µs to 172 µs. Full's fixed cost is now +3.2% of a shorter frame (it was +2.3%). A first
board at 48 processes (the readers' binaries) read disabled unresolved (CPU upper +1.15%); 96 resolves it. Windows
was not re-boarded: the pair would sit inside the two-binary layout floor, and its code change is read above.

## Verification

win-debug: crd-containers-tests, crd-platform-tests, the 15 `crd-diag-overhead-*` CTests, the two script CTests and
the 9 `crd-containers-fatal-*`, the ctest guards; linux-gcc-debug through `wsl-lane.sh`: the same suites. Whole-tree keep-going builds of
linux-gcc-shipping and linux-gcc-relwithdebinfo: no `warning:` line. One linux-gcc-shipping harness link stopped on a
GCC internal compiler error in `flight_recorder.cpp`'s link-time code; the same source had linked in the
whole-tree build, and the rerun linked clean ([host faults](../../MEMORY.md)). win-shipping-profile builds the
harness. `tidy-files.py` clean on the three files, clang-format clean on the added lines, `check-headers.py` on
`array.hpp`, `check-include-budget.py`, `check-allman-braces.py`, `check-master-plan.py`, `check-repository.py`.

## Not done in this batch

- (7)'s authored rendering workload (sandbox headless, a GPU device).
- (l) zstd 1.5.7 and (o) clang-tidy 20 on the WSL host are downloads; this run's only user message approves none, so
  each waits for the user's approval in the conversation.
