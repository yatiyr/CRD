# 2026-11-23: NET-PATH, a client rebound on its way to the preferred address

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S75](../design/network-foundation.md#migration), the
"A client rebound on its way there" bullet.

## CI

Fuzz host-check 45153550994's input 805dc37f and libFuzzer find 0736d765 failed the server follow check on HEAD.
Both were engine defects; the oracle was right. CI 45154721965 has no failed job. Host-check 45165082942 is queued.

## The defects and the fix

In both inputs the client moved to the preferred address while its NAT rebound socket 0. The server moves there only
once it has validated the client's path (RFC 9000 section 9.6.2). Until then it kept sending to the dead old mapping,
and a failed validation then closed the connection.

- **No ID for the rebound path (805dc37f).** The rebound path carried the ID of an earlier path to the preferred
  socket, but admission shared only the active path's ID. Both of the server's client IDs were in use, so the
  rebound path waited for an ID the client could not send. Fix: until the server moves, a new address on the
  preferred socket that carries a path's ID there is that path rebound, and it shares that path's peer ID (section
  9.5). On the active socket the rule is unchanged, so a probe with another path's ID still waits.
- **The server stayed on the old address (0736d765, minimised).** Its first socket held a validated path to the
  client's new address, but the server kept sending to the old one. The client's spare ID had been lost, and it was
  never resent because nothing the server sent reached the client. Fix: while the move waits, the server sends from
  its first socket to the client's address when its path there is validated and addressed (sections 9.3 and 9.6.2).

## Evidence

- **Replays.** Both inputs, and their minimised forms (`-minimize_crash` on the pre-fix binary, 133 and 108 bytes),
  fail before the fix and pass after it. The minimised forms are adopted: a9385bb9 and 8d4bdc2b. The host-check's
  other two inputs (37984311, 678a9eca) pass both before and after.
- **New `[preferred-address]` cases, each failing with its half reverted.** One rebinds on the way there; the server
  shares the held ID and moves at once. The other rebinds while the server cannot validate there yet; its first
  socket follows the client, and the server moves once the loss ends. The 108-byte input also fails without the
  follow.
- **Rejected alternatives.** Giving a superseded validating path's ID to the newest move had no input or case with
  teeth. Dropping the superseded move eagerly would churn IDs against a peer whose ACKs alternate paths.
- **Runs.** linux-gcc-debug: crd-net-tests 1157 cases (7 skipped), every net corpus and the `crd-no-|crd-check`
  guards pass. linux-gcc-release: 333 `[preferred-address],[net-path],[multipath]` cases pass. libFuzzer (`-fork` 2)
  ran 422 s, 45077 executions, no artifact. LLVM 22.1.8 tidy is clean, and clang-format is clean on the added lines.

## Next

- CI first: audit host-check 45165082942 and the first finished CI run with this batch.
- Revisit the follow check's earlier relaxations (the `heard` clause, f370db2c's): two of their causes were engine
  defects. Then picoquic interop.
