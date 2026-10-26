# User decision: the network foundation and crash-to-fix window (2026-10-27)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

## Decision

Cerid needs a completely robust and secure networking foundation used everywhere, with no product-class limit: games
from duels to massively multiplayer worlds, collaborative editors, DAW live sessions and performances, Zoom-class calls
and webinars, the crash pool and agent hubs. The user approved, on 2026-10-27:

- **The [NETCRASH](../ROADMAP.md#slice-netcrash) window** directly after OWN and before AUD-2: NET.1 to NET.4, 4.2a,
  4.2b, the new [NET-SIM](../ROADMAP.md#slice-net-sim), NET.5 (Windows and Linux), CRASH-FIX.1 and CRASH-FIX.2. The
  guarded fix agent ([CRASH-FIX.3](../ROADMAP.md#slice-crash-fix.3), .4 and the parent) follows AGENT.3, because
  crash packs are hostile input (prompt injection through logs and names) and AGENT.3 owns that defence. None of it depends on the renderer, UI or media rows; it follows
  HESAP-JOBS-RETUNE (I/O threads) and OWN (one owner for `crd-net`), and AUD-2 audits it.
- **New rows:** NET-SIM (deterministic network fault simulation, a gate for every networked row),
  [NET-DEBUG](../ROADMAP.md#slice-net-debug) (live lag, jitter, loss and outage conditioning inside `crd-net`, a local
  cluster launcher with bot clients, qlog/pcapng capture, multi-peer record and replay, a packet inspector),
  [NET-SCALE](../ROADMAP.md#slice-net-scale) (sharded persistent worlds with seamless handoff) and
  [NET-LIVE](../ROADMAP.md#slice-net-live) (Zoom-class real-time media, NAT traversal, MLS end-to-end encryption).
- **Amendments:** NET.2 (hybrid post-quantum key exchange, MLS group encryption, mutual service authentication, key
  management, a formally verified handshake), NET.4 (continuous fuzzing, flood defence, an independent security review),
  4.2a (QUIC with datagrams primary), CRASH-FIX.2 (the collector moves from OPS.1; tickets with a GitHub Issues adapter
  and renewable agent leases), CRASH-FIX.3 (claims tickets under leases), CMD (its journal feeds reproduction packs).

## Ordering

The application layers stay after the systems they need and now depend on NET.5: 4.2c (scene sync and rollback), 4.2d,
4.2e, NET-SCALE, NET-LIVE (audio graph and media rows), the research rows on dedicated servers and anti-cheat, and
COLLAB (document model). Browser channels qualify under WEB.5.
