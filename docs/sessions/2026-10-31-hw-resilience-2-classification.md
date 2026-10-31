# 2026-10-31 — HW-RESILIENCE.2: classifying hardware faults

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2), its fourth part: the impossible-state classification
in `bundle.inspect`, the known-unstable CPU table and the user's message. Design:
[runtime diagnostics](../design/runtime-diagnostics.md#hw-resilience--hardware-fault-resilience).

## What landed (crd-perf)

- `crd/perf/fault_classification.hpp`, registered as `diagnostics.fault-classification` in
  `cmake/capability-owners.json` (its signature: a `BUS_MCEERR_A[RO]` read).
- `read_crash_evidence` reads the fault from either native capture on any platform:
  - a minidump's exception stream (code, `ExceptionAddress`, and for an access violation or in-page error the data
    address) and its loaded and unloaded module lists;
  - a crash record's signal line (signal, signed si_code, `addr`), `rip` or `pc` and module lines.
  A capture that is neither, or whose fault line is malformed, is refused with the reason; every prefix of a record
  and of a dump reads either the whole fault or nothing.
- `classify_fault` signatures:
  - `machine-check`: SIGBUS with `BUS_MCEERR_AR`/`AO`, the kernel's own report of a memory error.
  - `code-address-bit-flip`, `data-address-bit-flip`: the faulting address (a fetch when it equals the pc) lies
    outside every module, and one flipped bit puts it inside a loaded one: a pointer holding a value no store wrote.
    The guards: a sent signal's address (si_code at most 0, the sender's pid and uid; [2026-10-26 finding](2026-10-26-catch2-fault-address.md#the-cause))
    is not read, the first 64 KiB (a null pointer plus an offset) are never judged, and an address inside an
    unloaded module is a dangling pointer, not a flip.
  - `replay-not-reproduced`: the caller says the capture's replay ran on the same build and passed.
  - Verdict: likely on a machine check, two signatures, or one on a listed processor; possible on one; else none.
- `cpu_advisories()`: one entry with its source, Intel's Vmin Shift instability of 13th and 14th Gen desktop parts
  (signature `0xB0671`; Core i5, i7 and i9 of 65 W or more, so no HX, H, P, U, T or i3 part). The fix is microcode
  `0x12F`: Intel's root-cause update of 2024-09-25 named `0x12B` the final mitigation and said microcode does not
  repair a processor already unstable; `0x12F` (2025-05) covers the rare light-load case. The `0xBF` parts (other
  numbering, no published fix revision found) are not listed, since an entry without a source stays out.
- `fault_message`: one sentence naming the evidence and the fix: a BIOS update before the fix, a warranty
  replacement for a listed processor that has the fix and still faults (this host's case: i9-14900K, `0x133`), a
  memory test and no memory overclock for a machine check. A listed processor with no signature is no verdict;
  older microcode is still said.
- `bundle.inspect` reads a crash capture as it is (summary `capture`), or the first crash section a bundle carries,
  and adds a `crash` item and a `classification` item (verdict, signatures, each flip's addresses, bit and module,
  advisory, microcode state, message) before the symbols. It takes `replay=<not-run|reproduced|not-reproduced>`.
  The replay outcome comes from whoever ran the recipe; [CRASH-FIX.1](../ROADMAP.md#slice-crash-fix.1)'s
  `ceridc repro` is that runner (track C's row; relayed).

## Verification (win-debug)

- `crd-perf-tests`, seven new `fault classification` cases: the record's and dump's fault and modules, refusals and
  every cut, the machine check and the sent signal, both flips and their four guards, the advisory's SKU rules and
  microcode states with each sentence, the replay signature, and `bundle.inspect` on a record and on a bundle with
  and without `replay=` (a bad value is `BadArgument`).
- The application's real crash (`diagnostic host: the application's wild write ...`) now also reads its capture:
  the pc equals the dump's exception address, the address is the null store's, the specimen is among the modules,
  and the classification on this machine finds no signature.
- Three existing `bundle.inspect` cases count the new items (and check them).
- Teeth, restored, rebuilt and rerun green: a sent SIGSEGV's address read as evidence failed the sent-signal case;
  the null-region guard removed failed the guard case.
- Hosted CI runs every other lane (the Linux record of the real crash included).

## Left on the row

Faults in code that cannot fault: crash captures carry the faulting instruction's bytes, compared with the module's
image (code no store wrote) and decoded for its memory accesses and divisor.
