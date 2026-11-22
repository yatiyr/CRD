# 2026-11-22 — NET-PATH: CI fixes, a fuzz oracle that waits for a lost validation, and the refusal's wake

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follows the [client-open note](2026-11-22-net-path-client-open.md).
The PC shut down during the batch after it. Its three edited files survived uncommitted (the `QuicClient::add_path`
wake and two connection cases), and this batch finished and tested them.

## CI first

- CI 44970457476 (e446680a): linux-gcc-release, -relwithdebinfo and -shipping failed their LTO link on
  `-Wnull-dereference` in `unused_peer_sequence` (`*m_cids.peer_at(...)`). The loop now walks the pointer until it
  is null.
- The same run: linux-gcc-asan, the arm64 ASan lanes and win-asan hit stack-use-after-scope in the `[cids]` case "the
  peer's IDs are held per path ID". Three frames kept spans into a temporary `PeerId`. They now name it.
- Fuzz host-check 44963728694 (d8163867), net-quic-connection leg 8402: both arms' inputs replayed on HEAD. The oracle
  "the server sends to the client's address" ran with no time passing after the transfer. In one input the script
  lost the client's PATH_RESPONSE. In the other, a `rotate_connection_ids()` retirement reached the server before the
  client's responses, which carried the retired IDs. In both, the server was still validating the address, with its
  next challenge on a timer (RFC 9000 section 8.2.1). The engine was right: both inputs now end with the server
  following, with neither end closed. The oracle requires that validation to be under way, and runs the clean path with
  timers (`Pair::settle_until`) until it resolves. Then the server must follow; only an end that closed with the
  address unvalidated is excused.

## What landed

- `QuicClient::add_path` wakes the connection whatever the answer, so a refusal's PATHS_BLOCKED or
  PATH_CIDS_BLOCKED leaves in the loop's next turn.
- Cases: a lost PATH_CIDS_BLOCKED sent again exactly once; a client that has issued no ID of its own for the path
  ID owes no frame; and a hosted client whose third open the server's limit refuses, with the server counting
  PATHS_BLOCKED within 100 ms of a quiet connection.

## Evidence

- linux-gcc-debug (WSL): `crd-net-tests "[net],[cids]"` 896 cases, 889 passed, 7 skipped. The two hosted inputs and
  the 42-input corpus replay clean (44 inputs).
- linux-gcc-asan: `crd-net-tests "[cids],[multipath]"`: 51 cases pass.
- linux-gcc-release (LTO, `-Wnull-dereference`): `crd-net-tests` and `crd-fuzz-net-quic-connection` link clean.
- linux-clang-fuzz: `crd-fuzz-net-quic-connection-libfuzzer -seed=8402 -max_total_time=240`, seeded with the
  corpus and both inputs in a scratch directory: 25146 runs, no fault.
- Teeth: restoring the conditional wake fails the hosted case (line 1720, the 100 ms bound). A server that never
  takes the preferred path once it validates the address (a server-only mutation of both `take_preferred` calls)
  fails the oracle on both inputs. The first form of the oracle (a755cb40) passed that server, because its idle close
  counted as a pass.

## Next

- PATH_ABANDON both ways, as the client-open note's Next lists it, then the rest of that list.
