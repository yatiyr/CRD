# 2026-11-04: NET.2, the Keychain and the Secure Enclave on the macOS lane (Track C batch 92)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Design: [K16](../design/network-foundation.md#key-store).

## CI first

Dispatch 41717350998: its three remaining failed jobs (linux-gcc-release, -relwithdebinfo, -debug-scalar) fail only
crd-repository-tools and crd-license-manifest, batch 89's unrendered manifest (8a66ef13, fddede9a). Nothing new.

## What landed

- **macOS x64 closure of crd-crypto** (`macos-x64-debug`: Apple Clang 17, deployment target 14.4, `CRD_MODULES`
  crypto): crd-jobs' SysV switch assembles as Mach-O, the worker semaphore waits on `os_sync_wait_on_address`, core's
  CPUID include and its Windows/Linux-only helpers are guarded, and AWS-LC's headers are named with `-I` there (Apple's
  clang searches `/usr/local/include`, Homebrew's OpenSSL, before every `-isystem`; `--system-header-prefix=openssl/`).
- **`crd::parse_f64`** (capability text.parse-float, crd-core): `std::from_chars` where the library has it for floating
  point, `strtod_l` in the C locale under Apple's libc++; JSON, CEIR text and the ceridc fuzz target use it.
- **K16**: `PlatformKeyProtector` "keychain" (data-protection keychain for a signed application, default file keychain
  otherwise; a no-prompt lookup searches only unlocked keychains) and `PlatformKeyDevice` "secure-enclave" (P-256,
  data-protection keychain). The Secret Service's hidden cases became the platform vault's (`[.platform-vault*]`), run
  on macOS by `keychain_session.py` in a scratch keychain (unlocked, seeded then locked, file gone).
- **The `macos` job** (macos-15-intel, complete tier), its pins (runner, Ninja 1.11.1's macOS archive) and the own
  CI's host (the rented Mac, under Rosetta 2). `CRD_REPOSITORY_CHECKS=OFF` on the lane; the ISA guard's Mach-O reader,
  the repository tools on a macOS host and the macOS memory, resource and crash probes are MAC.1's (its row says so).
- Fixed on the way: units' suffix parse spun forever on "1.85 _m" (a constant-length trim loop; new test); the fuzz
  replay read files through `istreambuf_iterator` (sign conversion on libc++); provider-prefix.py now skips a BSD
  archive's `__.SYMDEF SORTED` index under a `#1/` name (test fixture made BSD-faithful, teeth: 1 error without it).

## Evidence

- Throwaway probe workflow (branch `probe-macos` on the GitHub mirror, deleted after), eight runs. 41742520653: all
  713 units built, 321 of 324 CTests passed; the locked run hung on a file keychain's unlock prompt (the no-prompt
  lookup now searches only unlocked keychains) and the absent run's `available()` answered true.
  41751217685: the locked run passed; a forget that found nothing answered Denied because the system domain's
  keychain counted as skipped (the search is now the user domain's). 41753205451: **324 of 324 CTests passed**, the
  three crd-crypto-keychain runs (unlocked: protect, recover, a changed format or handle byte, 256 rounds, four threads,
  a store revoked under it; seeded then locked: Denied without a prompt; file gone: Unavailable) among them; the
  Secure Enclave case skips (no enclave in a virtual machine, and an unsigned test keeps no enclave key).
- win-debug: `[parse-float]` 74 assertions, units `[parse]`, asset-io, ceir (580 cases), `[key-store]` (10088), the
  fuzz corpora and guards pass; tidy (LLVM 20.1.8) clean on 12 changed files. WSL linux-gcc-public-checks: 37 CTests,
  both crd-crypto-secret-service runs under the renamed tags.

## Next

The complete-tier dispatch with the macos job; then NET.2's last items (ARM64 assembly's hosted proof).
