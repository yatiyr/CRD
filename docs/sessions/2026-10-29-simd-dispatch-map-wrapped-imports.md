# 2026-10-29 — SIMD-DISPATCH: the ISA guard read dropped imports as function starts

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §6](../decisions/0134-runtime-cpu-dispatch.md);
follows [the specimen null stores](2026-10-29-simd-dispatch-null-store-and-probe-namespace.md).

## CI first

Scheduled run 40849112570 (fea7ee0a) is still running (win-release, win-shipping, win-debug, win-asan, win-tidy, the
Linux GCC debug, asan, tsan and public-checks lanes, the ARM64 ASan lanes); nothing was cancelled. Its newly completed
failed jobs were read from their logs. All but one are the refs manifest pair (fixed by 74a95125) and, on the GCC
release lanes linux-gcc-shipping included, the specimen and probe failures fixed by a721d147 and d334c12f.

The new one: win-relwithdebinfo (123363959065), `crd-check-isa-isolation`, two failures in
`crd-sandbox-showcase-tests.exe`: "dumpbin decodes no instruction at function start 0x140000000" and `0x14000000d`.

## The defect

Both addresses are inside the PE header. link.exe lists an import piece it dropped (`.idata$6`) at a wrapped offset
below its segment, so its Rva+Base is the image base; the local maps show the form in the data segment
(` 0002:ffd80000 .idata$6 0000000140000000 ntdll:ntdll.dll`). In this image it fell in a segment that holds code, so
`read_map` took it as a function. b9e796e3's re-decode of a start the sweep did not align then decoded the header
and reported it; before b9e796e3 such a start was silently skipped.

`read_map` now keeps a code symbol only when its segment offset lies inside that segment's code span (the CODE
section lines). The full symbol list, which the general-object and `__isa_available` checks read, is unchanged. On
the six local release maps the code symbol count is identical before and after.

## Checks

`test_map_code_symbols_lie_inside_their_segments_code` (a map with wrapped `.idata$6` entries in a code and a data
segment, a symbol past the code span and `__ImageBase`); the full tooling tests; the repository validators.

## Left

- The next win-relwithdebinfo guard run on this fix.
- From the previous note: a fully green ARM64 job at or after 74a95125, and the first green guard run on the GCC IPO
  lanes.
