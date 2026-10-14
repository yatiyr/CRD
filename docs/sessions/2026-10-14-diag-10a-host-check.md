# DIAG.10a host check on hosted machines, 2026-10-14

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10a](../ROADMAP.md#slice-diag.10a). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10a); harness:
> [test instruments](../design/test-instruments.md#host-separation-diag10a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a container-overflow control](2026-10-11-diag-10a-container-overflow-control.md).

## Goal

The last clause of DIAG.10a: separate the development host (an Intel Core i9-14900K under WSL2) from the software.
Locally, rare input-independent ASan and UBSan reports appeared in `crd::containers::Array`'s container-annotation
path and never reproduced; the container-overflow control showed the annotation is where the corruption is noticed,
not where it is made, and that neither libFuzzer, the sanitizers nor the input is needed. A GitHub-hosted runner is a
different physical machine running native Linux, so the same exposure there is the discriminating run.

## The instrument

[host-check.yml](../../.github/workflows/host-check.yml) runs only when dispatched; `check-ci-tiers.py` now holds
every workflow besides `ci.yml` to that (triggered only by `workflow_dispatch`, never cancelling a run), so it never
joins a CI tier, and `check-pins.py` holds it to the same pinned actions, runners and credentials. Seven
`ubuntu-24.04` legs mirror the rounds of the
[unknown-origin session](2026-10-11-diag-10a-unknown-origin-artifacts.md): three of `ceir-binary` (seeds 7101 to
7103), one of `ceir-text` (7201) and three of `ckir-binary` (7301 to 7303). Each leg configures `linux-clang-fuzz`
(clang 18.1.3, the runtime of the local rounds), builds its one libFuzzer target and runs
[fuzz-host-check.py](../../scripts/fuzz-host-check.py): one libFuzzer process for 540 s under the default ASan options,
then the container-overflow control (`detect_container_overflow=0`) for another 540 s, on a scratch copy of the
committed corpus (only read), with the harness limits and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. After
an event the round restarts with the seed plus 100 on the same working corpus until its time is spent (so the
restart seeds of the `ceir-text` leg, 7301 onwards, are not the `ckir-binary` legs' seeds). The `ceir-text` leg also
replays the adopted unit `cd301be8` and one `ckir-binary` leg the unit `ace35b97`, three times 10,000 repetitions in
one process per arm, with the note's repetition options. An event is an artifact, a nonzero exit, or a sanitizer,
assertion or libFuzzer report; the step summary carries the CPU from `/proc/cpuinfo`, every process's executions and
the stack head of every report, and the logs and crash files are uploaded.

A 30 s smoke of the same commands on WSL (`ceir-text` with `cd301be8`, `ckir-binary` with `ace35b97`, `ceir-binary`)
ran clean before the push: 0.26 M to 0.84 M executions per round, 100-repetition replays, no event.

## Run 38590995552 at b536a697

[Run 38590995552](https://github.com/yatiyr/CRD/actions/runs/38590995552), four CPU models, each leg a 4-CPU Azure
guest (kernel 6.17.0-1022-azure, microcode hidden by the hypervisor):

| Leg | CPU | Default ASan | Container-overflow control |
|---|---|---|---|
| ceir-binary 7101 | AMD EPYC 9V74 | 4,025,393, 0 events | 3,919,105, 0 events |
| ceir-binary 7102 | AMD EPYC 7763 | 3,285,870, 0 events | 3,323,958, 0 events |
| ceir-binary 7103 | Intel Xeon Platinum 8573C | 4,360,328, 0 events | 4,437,904, 0 events |
| ceir-text 7201 | AMD EPYC 9V45 | 3,376,773 in 6 processes, 5 events; 30,000 repetitions clean | 4,408,010 in 9 processes, 8 events; 30,000 repetitions clean |
| ckir-binary 7301 | Intel Xeon Platinum 8573C | 6,426,214, 0 events; 30,000 repetitions clean | 6,658,481, 0 events; 30,000 repetitions clean |
| ckir-binary 7302 | AMD EPYC 9V74 | 7,562,323, 0 events | 7,735,873, 0 events |
| ckir-binary 7303 | AMD EPYC 7763 | 5,038,082, 0 events | 5,164,755, 0 events |

In all, 23.35 M `ceir-binary`, 38.59 M `ckir-binary` and 7.78 M `ceir-text` executions and 120,000 repetitions.

### The thirteen events are one software defect

Every event of the `ceir-text` leg is the same deterministic assertion, at two sites, each with an artifact:

    Assertion failed!
    #4 crd::ceir::Context::intern_type_class(...)  engine/execution/ceir/src/context.cpp:91:5
    #5 Parser::parse_type_class_id()                engine/execution/ceir/src/parse.cpp:749:22
    #6 Parser::parse_extern(unsigned int)           engine/execution/ceir/src/parse.cpp:794:33

(nine events), and the same through `intern_attr_class` (`context.cpp:279`), `parse_attr_class_id`
(`parse.cpp:1413`) and `parse_wrapper_attr` (four events). The text loader handed a `dialect.class` name of 256
bytes or more to the intern call, whose fixed 256-byte buffer asserts (`intern_type_class: dialect.class too long`);
the op-name path already refused such a name (`Context::kMaxOpNameBytes`, a REPO.DEV.9 finding), the class paths did
not, and the binary loader had the same hole for both class kinds. All thirteen artifacts replay with the same
assertion on this host's `linux-clang-fuzz` build. The restarts found it again quickly because the working corpus
kept the inputs near it, so it is one finding, not thirteen. It is a real defect, not the host: it reproduces on
every machine and on every input that carries such a name.

## The fix

`Context::kMaxClassNameBytes` (256) sizes the type-, attribute- and location-class intern buffers; the text reader
(`parse_type_class_id`, `parse_attr_class_id`) and the binary reader (the Extern type record and the extern attribute
record) refuse a longer name before interning ("type-class name too long", "attribute-class name too long", "Extern
class name too long", "ATTR extern class name too long"), and the intern asserts now say the loaders refuse such a
name first. Regression inputs:

- `tests/execution/ceir/long_class_name.hpp` builds a module whose `test.x` op names a class of 255 bytes, the
  longest the buffer takes, and lengthens its printed text or its blob's STRP entry by one byte (the builder cannot
  make a longer name; its intern asserts).
- Written seeds, each verdict checked as it is written: per class kind the longest name (accepted) and one byte
  more (refused), in the `ceir-text` corpus (`seed-96ddb98d11fb8e90`, `seed-9c5921a526f1cf20`,
  `seed-f99da22fbec1c3db`, `seed-93a7ea40e895af0f`) and the `ceir-binary` corpus (`seed-a956b077c5571402`,
  `seed-be941100e70bc283`, `seed-c50c3879967cb266`, `seed-6f8a4fa1ba836b05`).
- Two adopted inputs, the hosted artifacts `crash-424e0f1f` (type class) and `crash-1f967021` (attribute class)
  minimized on the unfixed build (`fuzz.py minimize`): `51baf72326ae1436cb6c7f6aaaf61687f1e5e9d3` (288 bytes) and
  `3a8e846fccf3908e4bfbd18368147b4a8f723a68` (296 bytes).
- One `[diag10a]` case in `crd-ceir-tests` (`test_binary.cpp`): both loaders load the 255-byte name and round-trip
  it, and refuse the 256-byte one with its reason, for both class kinds.

The corpora grow from 27 to 33 inputs (`ceir-text`) and from 30 to 34 (`ceir-binary`). Teeth on win-debug: each of
the four refusals disabled alone makes its own corpus replay end in the intern assertion (exit 3), while the other
corpus stays clean.

## Verification

- win-debug: the whole tree built (`all`); `crd-ceir-tests` 577 cases (10 `[diag10a]`), `crd-ceir-cook-tests` 115,
  the four CEIR fuzz corpus CTests (33 and 34 inputs), the `crd-no-*`/`crd-check*` guards and `crd-master-plan` pass.
- win-shipping, win-clang-cl-shipping and win-asan (inside vcvars): the two corpora, `crd-ceir-tests` 577 and
  `crd-ceir-cook-tests` 115 pass; `--seed` on win-clang-cl-shipping regenerates the committed seeds byte for byte.
- WSL, one lane at a time: linux-gcc-debug (the two corpora, `crd-ceir-tests` 577, `crd-ceir-cook-tests` 115),
  linux-gcc-asan and linux-clang-tsan (`setarch -R`; the corpora, the `[diag10a]`, `[binary]` and `[roundtrip]`
  CEIR cases, 26, and `crd-ceir-cook-tests` 115), and linux-clang-fuzz rebuilt on the fixed source: the corpora replay
  and all thirteen hosted artifacts now load or are refused without a report (exit 0).
- Strict tidy (LLVM 20.1.8) is clean on the six changed C++ sources; clang-format differs only in the repository's
  hand alignment and include grouping. The Allman check, `check-ci-tiers.py`, `check-pins.py`, the pinned actionlint
  1.7.12, `test-repository-tools.py`, `test-dev-workflow.py`, `check-master-plan.py` and `check-repository.py` pass.

## Rerun 38605464765 at 777e8b70, on the fixed source

[Run 38605464765](https://github.com/yatiyr/CRD/actions/runs/38605464765), the same seven legs and seeds:

| Leg | CPU | Default ASan | Container-overflow control |
|---|---|---|---|
| ceir-binary 7101 | Intel Xeon Platinum 8573C | 4,689,342, 0 events | 4,759,322, 0 events |
| ceir-binary 7102 | AMD EPYC 9V74 | 5,064,614, 0 events | 5,083,567, 0 events |
| ceir-binary 7103 | AMD EPYC 9V74 | 4,281,007, 0 events | 3,976,062, 0 events |
| ceir-text 7201 | AMD EPYC 7763 | 1,664,858, 0 events; 30,000 repetitions clean | 1,880,707, 0 events; 30,000 repetitions clean |
| ckir-binary 7301 | AMD EPYC 9V74 | 5,864,216, 0 events; 30,000 repetitions clean | 6,218,950, 0 events; 30,000 repetitions clean |
| ckir-binary 7302 | Intel Xeon 6973P-C | 9,106,595, 0 events | 8,188,614, 0 events |
| ckir-binary 7303 | AMD EPYC 7763 | 5,254,945, 0 events | 4,828,118, 0 events |

In all 27.85 M `ceir-binary`, 39.46 M `ckir-binary` and 3.55 M `ceir-text` executions (70.86 M) and 120,000
repetitions, every process finishing its budget: **no event of any kind**, no artifact, every leg green. At this host's
lowest `ceir-text` rate alone (one event per 400 k executions) about nine events were expected in that leg.

## What this settles

The host question, for the corruption of unknown origin: on five hosted CPU models over the two runs, about
140.6 M executions of the three targets that faulted locally, in both arms, and 240,000 repetitions of the two units
adopted from it produced no event that does not replay, and the rerun on the fixed source produced no event at all. At this host's lowest rate (one event per 400 k `ceir-text` executions; `ceir-binary`
and `ckir-binary` faulted at about one per 1 M to 2 M) the hosted exposure would have shown dozens. The faults follow
the development host, not the software. What the hosted run cannot say is whether this host's CPU or its WSL2 guest
is at fault; only this machine booted into native Linux separates those, and nothing in the roadmap waits on it.
The software defect the run did find is fixed above.
