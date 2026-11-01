# 2026-11-01 — Every network row in one block after NETCRASH

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-netcrash).

Rows: the block from [NET-TIME](../ROADMAP.md#slice-net-time) to [CRASH-FIX](../ROADMAP.md#slice-crash-fix), directly
after [NETCRASH](../ROADMAP.md#slice-netcrash).

## The user's order (verbatim, 2026-11-01 evening)

<a id="user-order"></a>

- "but all net rows mean everything related to multiplayer, scaling, live, precise clock sync between machines
  robotics bridge and anti-cheat and everything! I WANT ALL OF THEM SITTING RIGHT NEXT TO EACHOTHER IN THE ROADMAP AND
  I WANT ALL OF THEM IMPLEMENTED! WHY THE HELL WE ARE LEAVING NET STUFF FOR FAR LATER? WHY WE DON'T PROPERLY IMPLEMENT
  THIS"
- "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD STANDARD NETWORK MODULE!"
- "I see that it is a large amount of work but WE NEED TO DO IT PROPERLY! ALL THE FUTURE SYSTEMS WILL USE OUR NETWORK
  SYSTEM AND WE NEED TO DO IT PROPERLY AND AS SECURE AND FAST AND COMPACT AS POSSIBLE WITH THE LATEST ALGORITHMS AND
  TECHNIQUES"

Earlier the same day (about 01:25), still binding: "approve the downloads we need to do everything properly, I will
contect a lawyer later! DO NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND
PROPERLY DONE AND GOLD STANDARD AND ELITE".

## The block, in dependency order

NET-TIME, COMPLY.5, 4.2c, 4.2d, NET-SERVER, ANTICHEAT, NET-SCALE, COMPLY.9, PLG-1, AUDIO-DRV, MED-6-OPUS, MED-7-OPUS,
MED-8-LIVE, MED-GPU-1, MED-10, MED-11-LIVE, MED-HW, NET-LIVE, 4.2e, NET-TIME-HW, NET, CRASH-FIX.3, CRASH-FIX.4 and
CRASH-FIX. Every moved row keeps its text and evidence and carries a dated **Network block** amendment.

- COMPLY.5 and COMPLY.9 come in at engineering depth, as COMPLY.0 to COMPLY.2 closed: unread instruments are dated
  refresh entries in the legal register and counsel questions are open register entries.
- The call media a working Zoom-class call needs come in ahead of NET-LIVE. Whole rows: PLG-1 and AUDIO-DRV (the
  device path), MED-GPU-1 (the codec-provider interface), MED-10 (software video decode, now owning the AV1 intra tools
  and adding VP8) and MED-HW (hardware video). Split rows, each with its remainder left in place and requiring it:
  MED-6-OPUS from MED-6, MED-7-OPUS from MED-7, MED-8-LIVE (fragmented MP4, CMAF, LL-HLS and live Matroska) from MED-8,
  and MED-11-LIVE (real-time VP8, VP9 and AV1 encode with scalability) from MED-11.

## Inverted requirements

A network row no longer waits for a non-network system; the later row gains the integration requirement and says so.

| Network row | Dropped requirement | The later row that now requires it |
|---|---|---|
| 4.2c | EYLEM-v1j | EYLEM-v1j implements its generic authored state interface |
| 4.2d | SHIP.6 | SHIP.6 implements the Steamworks and EOS adapters against its interfaces |
| NET-SERVER | SHIP | SHIP.3 binds channels to its version handshake; SUPPLY.1 attaches SBOMs to its images |
| NET-SCALE | WORLD.1, FORMAL.1 | WORLD.1 implements its partition interface; FORMAL.1 checks its model against it |
| COMPLY.9 | COMPLY.7 | COMPLY.7 maps communication accessibility onto NET-LIVE |
| NET-LIVE | AUDIO-CEIR, MED-GPU-3 | AUDIO-CEIR.1 hosts its audio processors; MED-GPU-3 imports its recordings |
| CRASH-FIX.3 | AGENT.3 | AGENT.3 extends its provenance tagging and hostile corpus to every adapter |
| PLG-1, MED-GPU-1, MED-10 | MED-12, MED-REVIEW | none: the licensing posture is cited as recorded |
| AUDIO-DRV | JOBS-QOS | JOBS-QOS maps its audio class onto the device-thread class |
| MED-HW | GPU-TIER.0 | GPU-TIER.0 folds the adapters' codec capability into its ladder |

MED-3 and MED-4 now require MED-10, whose decoders they wrap.

## Method and checks

The move is one script keyed by row ID (moves, requirement edits, amendments, new rows, renumbering), so a rebase
conflict in the table is resolved by re-running it on `origin/main`'s table. `check-master-plan.py`, the repository,
Allman-brace and generated-source checks pass; the first unfinished row is unchanged.

## CI

No hosted run has a failure from this track. Push run 41332035560 (c5092073) was pending; complete-tier run
41323535550 (54f062f2) had only its preflight queued; complete run 41298463221's two failed jobs are the ARM64 ASan
lanes on the geometry track's Shewchuk predicate reference.
