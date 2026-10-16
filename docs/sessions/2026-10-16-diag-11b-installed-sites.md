# DIAG.11b: the installed application's event sites, 2026-10-16

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b) (the event-site form) and
> [public consumption](../design/public-consumption.md#the-diagnostic-family-diag11a). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [overhead harness](2026-10-16-diag-11b-overhead-harness.md).

## Goal

The follow-up the overhead batch queued: the installed application, `tests/support/package/diag-consumer`, still
formatted every event's message with `snprintf` and then called `FlightRecorder::record()`. It is the program an
application author copies, and at the basic detail it formatted forty messages to record five. This batch moves every
site of the application to the engine's site form, `CRD_FLIGHT_RECORD`, and makes the package test hold it: the
deliberate failure runs at both details, and the messages the application formatted must equal the events the
recorder kept. Committed locally, not pushed.

## What changed

| Piece | Where | What it does |
| --- | --- | --- |
| Site helper | `consumer_support.hpp/.cpp`: `site_text`, `print_sites` | `site_text(buffer, capacity, format, ...)` formats one message (clipped as the recorder clips it, printf-checked under GCC and Clang) and counts it in a process-wide atomic; `print_sites` prints `CONSUMER_SITES detail= formatted= recorded= filtered=` from that count and the recorder's `total_recorded()`/`total_filtered()` |
| Sites | `main.cpp` (forty orders), `example_allocator.cpp` (reserved, released, the heap violation), `example_jobs.cpp` (a slice summed, on the pool's workers), `example_reload.cpp` (limits loaded or kept), `example_capture.cpp` (a frame) | each is `CRD_FLIGHT_RECORD(recorder, code, severity, site_text(...), a, b)`; the violation handler's null-recorder branch is the macro's own null check. The capture example's request text for `flight.trigger` is a command argument, not a site, and keeps its own buffer |
| Proof | `scripts/test-package-consumer.py` (`Session.failure`, `investigation_problems(..., detail)`, `sites_problems`, `kept_orders`) | the failure runs at the installed policy's full detail and at a copy of it with `detail = "basic"` (its own run directory). At full: `CONSUMER_SITES detail=full formatted=40 recorded=40 filtered=0` and the bundle's forty orders as before. At basic: `formatted=5 recorded=5 filtered=35`, and the bundle's flight item says `detail` basic with five events before the trigger, the refunds 6, 13, 20, 27 and 34 in order, each a warning with `value_b` 1 and its own message. The investigation now also checks `detail` and every event's `value_b` |
| Judge tests | `scripts/test-repository-tools.py` (`test_package_consumer_judges_symbols_and_the_diagnostic_applications_investigation`) | the basic bundle judged clean at basic and named at full (detail, counts); a full bundle named at basic; an informational event in a basic bundle; a refund without `value_b`; the sites line: clean at each detail, an eager count (40 at basic), a wrong detail, a missing line |

The engine is unchanged: `CRD_FLIGHT_RECORD` and `admit()` landed with the overhead batch and ship in the installed
headers.

## Teeth

On the kept `win-debug` scratch, the application's copied `main.cpp` was changed to format the order's message before
the macro (an eager site, the form before this batch) and only the application was rebuilt; its failure was run in a
fresh directory and judged with the package test's own judges:

| Build | basic arm | full arm |
| --- | --- | --- |
| as committed | clean (`formatted=5 recorded=5 filtered=35`) | clean (`formatted=40 recorded=40 filtered=0`) |
| eager site | `CONSUMER_SITES formatted: expected '5', got '40'` | clean: the full detail cannot tell an eager site, which is why the basic arm exists |
| restored (byte-identical copy, touched, rebuilt) | clean | clean |

## Verification

| Run | Result |
| --- | --- |
| `test-package-consumer.py --preset win-debug` | PASS: full arm 40 of 40, basic arm 5 of 40, both investigations clean, the five examples, the Release and shared-library refusals |
| `--preset win-shipping` (profiling not compiled) | PASS: the same two arms (the basic detail works in a binary without profiling), fault injection unavailable |
| `--preset linux-gcc-debug` (WSL, `dwarf`) | PASS: the same two arms and the five examples |
| application, strict compilers | MSVC `/W4 /permissive- /WX` (win-debug prefix) and GCC `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wformat=2 -Werror` (linux-gcc-debug prefix): clean; `site_text` is printf-checked under GCC |

- Strict tidy (LLVM 20.1.8) against the application's own compile database: the six sources clean through
  `tidy-files.py`; the two headers clean with `clang-tidy` directly (the helper maps no header of a project outside
  the engine's build).
- clang-format `--dry-run`: no diagnostic on an added or changed line; every file keeps its earlier count (the hand
  alignment of the event-code table and of local declarations).
- `test-repository-tools.py` (71 tests), the Allman check, the `crd-no-*`/`crd-check*` guards (win-debug),
  `check-master-plan.py` and `check-repository.py` pass. No engine source, CMake file, preset or workflow changed, so no
  engine lane was rebuilt; `win-shipping-profile` and `linux-gcc-shipping` were not rerun for this application-only
  change.

## Seen in passing (the next batch's follow-up)

The hosted linux-gcc-shipping lane of run 38640651640 prints 44 GCC LTO diagnostics (13 distinct), not only the three
the overhead batch named. All come from link-time value ranges; most are outside DIAG:

| Site (as GCC names it) | Inlined from | Count |
| --- | --- | --- |
| `crd/kir/ckir_grad.hpp:25` `-Wstringop-overflow` (`swap_last2`, a `perm[]` write bounded only by `sh.rank`) | `reverse_ad` at `ckir_grad.hpp:169`, `:170` | 16 |
| `string_fortified.h` memset/memcpy bound "exceeds maximum object size" | `crd/containers/string.hpp:181`/`:198`/`:200` (`String::resize`/`reserve`) from `tests/tools/ceridc/test_ceridc_diag.cpp` (lines 550, 761, 907, 1052, 1206, 1381, 1541) | 15 |
| `crd/containers/array.hpp:276` memset bound | `ckir_eval.hpp:148` and `:171` (`resize(static_cast<usize>(nn), ...)` of an `int` count) | 5 |
| `hesap/dense/detail/gemm_pack.hpp:53` `-Waggressive-loop-optimizations` | `blas3.cpp:173` from `test_blas3_parallel.cpp:128/153`, `test_blas3_real.cpp:69` | 3 |
| `string_fortified.h` memcpy `-Wstringop-overread` | `string.hpp:443` (`init_from`) from `hesap/src/command_registry.cpp:188` | 2 |
| `<span>:289` `-Waggressive-loop-optimizations` | `crd/math/float_convert.hpp:563` from `runtime/examples/smoke_hesap_tensor.cpp:36` | 1 |
| `perf-ui/src/diag_panel.cpp:323` `-Wstringop-overflow` (`m_path[path.size()]` after the `size >= kPathCapacity` refusal) | `tests/ui/perf-ui/test_diag_panel.cpp:345` (a 512-byte path) | 1 |
| `string_fortified.h` memcpy into "a region of size 0" | `string.hpp:261` (`String::append`) from `tests/foundation/perf/test_diag_host.cpp:888` (a 44-byte string past the 32-byte inline buffer) | 1 |

The two DIAG sites read as false positives (the refusal returns before the write; the appended string lives on the
heap, not in the inline buffer), but that reading is to be confirmed on the lane, not assumed; nothing is suppressed.

## Hosted evidence this batch needs

`crd-package-consumer` on `win-public-checks` and `linux-gcc-public-checks` (complete tier) prints both failure arms,
"at the full detail its sites formatted 40 of 40 messages" and "at the basic detail its sites formatted 5 of 40
messages", each followed by a clean investigation, and the five examples' "showed what it must".
