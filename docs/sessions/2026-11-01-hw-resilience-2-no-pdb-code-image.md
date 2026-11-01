# 2026-11-01 — HW-RESILIENCE.2: a build with no PDB compares its code with its image

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2). Before: [code bytes](2026-11-01-hw-resilience-2-code-bytes.md).

## CI read

Complete-tier run 41274380605 (de5e10b0, still running) failed twelve jobs. Fixed on main before this batch: the
file-time case (4b9bc837, cc5ee9e9), clang-cl's 128-bit division (7cfed26c) and the multivariate buffers under GCC's
LTO (76330344). One was live: win-release failed the application's wild-write case, `code image: the module is not
a Windows image`.

## Cause and fix

win-release is the one Windows lane that links without a PDB (CMake's Release flags carry no `/Zi` or `/DEBUG`), so
the image has no debug directory and its dump's module entry no CodeView record: `parse_minidump_modules` leaves it
`id_kind` None. `pe_loaded_bytes` took Rsds or PeImage as its sign of a PE module and refused it, although its own
identity check (TimeDateStamp, SizeOfImage and CheckSum against the module entry) is what decides. It now refuses
only a build-id (an ELF module); a None module of a Linux record has no TimeDateStamp and stays refused by that
check. `image_file_path` had the same gate, so `bundle.inspect` could not find such a module's image: every kind but
a build-id now gets the PE key when it has a TimeDateStamp.

Not chosen: marking a loaded module without CodeView as PeImage in the parser. That is the symbol identity, which the
symbol index, flight identity and host tests hold at None ("refuse, don't guess" for a PDB lookup).

Teeth on win-debug: the new cases (a None module matches, a different stamp and a build-id do not, the image key of
a None module) failed on the old engine with the hosted reason and pass with the fix; `[hw-resilience]` and `[diag]`
pass.

Local guards: the ISA-isolation guard's Python process ended with a segmentation fault on this host (the known
misexecution of native Windows Python validators); its pool workers were left holding ctest's pipe until they were
stopped. It was rerun once.
