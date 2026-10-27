# 2026-10-27 — SIMD-DISPATCH: hesap-direct's supernodal solve loops as a kernel family

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

Run 40515770381 (`31e2b9ce`, complete tier): linux-gcc-asan stopped at link with `No space left on device` (73 MB
free). Every test program links each kernel family once per CPU level, and the ASan debug tree holds 11 GB of test
programs on the WSL mirror (code and the sanitizers' per-site data; compressing the debug sections saves 14%). Both
Linux jobs now drop the image's preinstalled toolchains no step uses (Android, .NET, GHC, Swift, CodeQL) before
anything else and print the free space before and after.
