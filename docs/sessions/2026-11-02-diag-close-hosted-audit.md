# 2026-11-02 — DIAG close: the hosted audit of complete-tier run 41409186119

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-diag).

Rows: [DIAG.11c](../ROADMAP.md#slice-diag.11c), [DIAG.12](../ROADMAP.md#slice-diag.12) and the
[DIAG](../ROADMAP.md#slice-diag) parent, closed at the user's order of 2026-11-01
([record](2026-11-01-user-order-close-waiting-rows.md#user-order)).

## The run

Complete-tier run [41409186119](https://github.com/yatiyr/CRD/actions/runs/41409186119) at `fb8452af`, dispatched on
2026-11-02 after the repairs the two runs before it found (41323535550 at `54f062f2` and 41342960677 at `59a3347e`):

- `47474b0b`: the module-selection case expects crd-crypto's aws-lc and wycheproof packages;
- `9ef40e7e`: the GREASE check compares unsigned bits (MSVC C4389 under /W4 /WX);
- `df0d359f`: the CI-4 parity table carries the Vulkan SDK installer emitter experiment;
- `3cf07ddc`: the device check widens its f32 input explicitly (Clang -Wdouble-promotion);
- `59a3347e`: a pinned-key lookup clang-cl's STL accepts;
- `d178d59b`: the fft cross-level search uses `std::ranges::any_of` (win-tidy);
- `cb087620`: LocalStore reads under its writers' lock (a Windows open racing a rename was refused; a new case saw
  47 to 66 refused reads in 3,000 to 5,200 before the repair, none after);
- `fb8452af`: a CPU pin is confirmed from the thread's affinity (Intel SDE answers CPU 0 to
  GetCurrentProcessorNumberEx on every CPU, so the hardware check pinned none under SDE; reproduced on the reference
  host and passing natively and under SDE after the repair);
- track C's `b37a4177`, `6e5dbb36` and `3a59a528` (AWS-LC within the Windows guards, the compressed-certificate case,
  AWS-LC's dispatch-test instrumentation out of every configuration).

## Audit

Every one of the 29 test lanes ends its gate with no unexpected failure. The only observed failure is linux-gcc-asan
shard 1's registered [TP-5](../third-party-defects.md#tp-5) case (`assetio: OUR tangents vs the REFERENCE mikktspace.c
ORACLE`), which the gate requires; skips are the registered ones (Vulkan on hosted Windows, the window cases on headless
runners). The cases the DIAG rows name, read from the job logs:

| Case or test | Lanes that ran it | Failures |
| --- | --- | --- |
| crd-perf-tests `doctor:` cases | 20 | 0 |
| `diag commands: a command declaring a second class needs both grants` | 18 | 0 |
| `crash contract: dump paths are UTF-8 both ways` (Windows lanes) | 11 | 0 |
| `catch2 fault:` cases and the wild-read case | 24 | 0 |
| the restricted-record flight case and `crd-fuzz-perf-flight-corpus` | 18 | 0 |
| `crd-diag-seeded-faults` (every seed DETECTED) and `crd-diag-coverage` | 23 | 0 |
| the `diag-workflow` label | 25 | 0 |

ARM64 lanes run their declared subset; the sharded ASan lanes run each case in exactly one shard.

DIAG.11c's and DIAG.12's hosted clauses hold on this run, and DIAG.11b closed on 2026-11-01 on the user's recordings.
Every DIAG child is Done, so the DIAG parent closes with them; MAC.DIAG and WEB.DIAG remain the later-platform gates the
parent names.
