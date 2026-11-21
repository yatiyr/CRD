# 2026-11-21 — QUIC paths: a client moves to sockets of its own

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S75](../design/network-foundation.md#migration), "The
client's own moves" and "IDs for waiting paths".

## CI first

- win-release and win-shipping (run 44807121941, MSVC 14.52): `test_kernel_dispatch.cpp:497` counted 578 = 34 x 17
  wrong, every guard word of the f16 short calls and no converted value. The signalling-NaN guard, stored as an f32
  value, was quieted by the optimiser. 101970a0 writes it as bytes and counts values and guards apart.
- win-tidy: 14.52's `<isa_availability.h>` declares two CRT externs `cpu_features.cpp` declares; they stay under a
  NOLINT (101970a0). The STL1000 failure (14.52's STL needs Clang 22; the pin is LLVM 20.1.8) is still open: a
  sample run of a local LLVM 22.1.1 found real findings (`bugprone-exception-escape` in `filesystem.cpp`'s noexcept
  wrappers), so the re-pin to 22.1.3 (the image's clang-cl) is a tree-wide slice of its own.

## What landed

- b5a252dd: `QuicConnection::probe_path()` and `migrate()`, `QuicMigration` refusals, `QuicPaths::trust()`, the
  restart on a move, revert on failure, the left path forgotten once proven and followed. Waiting paths take an ID
  on NEW_CONNECTION_ID or a validation's end (`address_paths()`); before, a server whose client moved with no spare
  ID never followed. With none unused, a left path gives its ID up.
- 87a791df: the fuzzer's own-socket ops, the one-way test network, the quiet and one-way cases. The fixture's
  duplicate include and nested conditional (tidy) are gone.

## Evidence

- win-debug (MSVC 14.51): 971 `crd-net-tests` cases, 7 `[client-migration]`; WSL linux-gcc-debug: 972 cases; local
  tidy (LLVM 20.1.8) clean on every changed source; Allman, 120 columns.
- linux-clang-fuzz `crd-fuzz-net-quic-connection`: two findings (90.9 s and 310.3 s), both the path oracle checked
  before a client's revert at the transfer's end was sent; the oracle now settles first, both inputs kept. Then
  541.1 s, 67563 runs, nothing found.
- Teeth: an owed PING on a move and a server move completed when its ID arrives were tried and removed, as no test
  or fuzz input needed them (the first datagram from a new path carries an ACK frame, so it is non-probing).

## Next

The STL1000 re-pin (LLVM 22.1.3, tidy findings fixed); the client host's second socket; preferred_address; interop
with ngtcp2's migrating client; LEO handover.
