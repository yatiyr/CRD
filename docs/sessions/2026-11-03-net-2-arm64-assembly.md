# 2026-11-03: NET.2, AWS-LC's Windows ARM64 assembly under cl.exe (Track C batch 80)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2). Contract: provider decision P3 in
[network-foundation](../design/network-foundation.md#provider).

## CI first

No completed run holds 255decba or later; the only failed job in the active runs is the android boot fault of
41574979037 (26c32908), which e608e642 fixed. Nothing to fix.

## What landed

- `CMakeLists.txt`: on Windows ARM64 under `cl.exe` the configure names clang's GNU driver as its one ASM compiler
  (`--target=aarch64-pc-windows-msvc`) before zstd's `project()` first enables ASM: Visual Studio's LLVM (on an ARM64
  host its native build, then x64; an x64 host only x64), then `%ProgramFiles%\LLVM` (the windows-11-arm image's LLVM
  20.1.6), then PATH. No clang fails the configure. The `OPENSSL_NO_ASM` fallback is gone: every lane runs AWS-LC's
  assembly.
- `provider_assembly()` (provider.hpp): `CRYPTO_has_asm()`; the provider test requires it on every lane.

## Evidence

- Scratch project with the same lookup, `cl.exe` as compiler and AWS-LC's debug `-g`: all 19 win-aarch64 `.S` files
  assemble in Debug and Release with VS 18's clang 20.1.8 (`clang -target aarch64-pc-windows-msvc ...`); every object
  is `coff-arm64` and its 127 external definitions all carry `crd_awslc_` (llvm-nm).
- win-debug: the 290 `[crypto]` cases pass (1 skip as before), the provider case with `provider_assembly()`.
- This PC has no MSVC ARM64 tools, so the link and run are hosted: the change tier's win-arm64-debug lane.

## Next

- Read the first hosted win-arm64-debug run with this commit: configure line `AWS-LC's ARM64 assembly`, the crypto
  CTests and `crd-check-provider-symbols`.
- Then the Keychain lane and the boards against OpenSSL and BoringSSL.
