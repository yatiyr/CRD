# 2026-11-23: NET-PATH, a challenge answered by a packet that is not probing on every path ID

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md). Design: the Validation bullet of [S75](../design/network-foundation.md#migration).
Follows [the datagram-counted turn](2026-11-23-net-path-datagram-turn.md); widens
[the challenge answer](2026-11-23-net-path-challenge-answer.md) from path ID 0 to every path ID.

## The defect

Fuzz host-check 45173760052 (82a3f40a), net-quic-connection seed 8403, input `8601da7f`, failed the follow check's
`!heard || counters.path_failures != 0U`; it also failed on 00cb5d9b. After the client's NAT rebound, a stale
non-probing packet moved the server's path ID 1 back to an old, validated address. Path ID 0 was the shorter, so the
client's path ID 1 sent no data and its PATH_ACKs rode path ID 0. The server challenged the client's real address on
path ID 1; the client answered with a PATH_RESPONSE alone, a probing packet, so nothing moved the server back (RFC 9000
section 9.3.3), and no validation failed. The owed answer (`m_answer_owed`) only covered path ID 0. Its comment assumed
a further slot's response rides with that slot's PATH_ACK, which PATH_ACK placement no longer guarantees.

## The fix

`m_answer_owed` is a bit per slot: a challenge on any slot's active path makes that slot's next packet steer, with a
PING added if it does not. Each path ID moves its own 4-tuples (RFC 10069 section 3.1.2). The bit clears when its slot
stops (`begin_abandon`), so a stopped slot never reports content that no packet will spend.

## Evidence

- The case "a challenge on a path ID's active path is answered there by a packet that is not probing ..." now runs
  over path IDs 0 and 1 (an attacker's copy moves the server; `also_at` is per client socket in the fixture). Mutant:
  without the engine change both path ID 1 variants fail (the server stays at the attacker's address). This also
  separates the per-slot rule, which the previous note said no case did yet.
- `8601da7f` replays clean and joins `corpus-quic-connection`. libFuzzer on the final source from the corpus (seed
  8403, 541 s, 24,916 runs) found nothing.
- linux-gcc-debug: all 1163 crd-net-tests cases and the `crd-net*`/`crd-no-*`/`crd-check*`/fuzz CTests (53) pass.
  linux-gcc-public-checks: the ten net interop CTests pass (ngtcp2, picoquic multipath, quiche, quinn, go and the
  rest). Pinned clang-tidy 22.1.8 is clean on the touched sources; clang-format is clean on the added lines.

## Next

- Audit the host-check on this commit and the first finished CI run containing it.
- NET-PATH Remaining: picoquic multipath past the first path (a picoquic client's own path, abandon and status),
  the budget boards against msquic and quiche, and the real-NIC cloud runs.
