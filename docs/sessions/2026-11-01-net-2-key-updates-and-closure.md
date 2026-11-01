# NET.2: key updates, the usage limits and closure on the TLS sessions

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 37; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [TLS sessions](../design/network-foundation.md#tls-sessions),
decisions S7 and S8, and the [TLS 1.3 profile](../design/network-foundation.md#the-tls-13-profile).

## The user's decisions this batch acts on

- 2026-11-01, about 01:25: "approve the downloads we need to do everything properly, I will contect a lawyer later! DO
  NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD
  STANDARD AND ELITE". No download was needed: the pinned AWS-LC and OpenSSL 3.5.9 cover this slice.
- 2026-11-01, about 14:50: "I wanted you to finish all net rows and you stalled on very currently unnecessary things. Of
  course we think about the laws but it seems you are over complicating everything! I WANT NET DONE PROPERLY AND GOLD
  STANDARD AND WITHOUT ANY DEFERS!" The batch takes the next clause of NET.2's Remaining: the TLS 1.3 profile's key
  usage limits, key updates, the 2^48-1 bound and `user_canceled` then `close_notify`.

## CI first

Nothing to fix from this track. Complete-tier run 41323535550 (54f062f2) and 41319531955 were still queued; push run
41323514367 was pending; every earlier push run since 438b8932 was cancelled before it started.

## What the provider does and does not do

Read in AWS-LC 5.12.0's libssl before writing the code, then proven by the tests below:

- `SSL_key_update` only queues a KeyUpdate, is a no-op while one is queued, and seals it at once under the old key
  (the write sequence returns to 0); a zero-length `SSL_write` moves it into the outgoing bytes, and its return value
  cannot tell success from failure, so success is `SSL_get_key_update_type` reading none.
- A peer's request is answered unasked, queued until the next write, and the provider enforces neither the usage
  limits, nor one outstanding request, nor any bound on the number of updates.
- A TLS 1.3 `user_canceled` warning is skipped; more than four warnings in a row are `unexpected_message`. There is no
  API to send a warning alert, and `SSL_shutdown` during the handshake writes nothing.

## What landed (decisions S7 and S8)

- `TlsConfig::key_update_records` (0: the negotiated AEAD's limit; at least 3; lowered to the limit) and
  `TlsConfig::max_key_updates` (0: 2^48-1; at most 2^48-1), `tls_key_usage_limit` (floor(2^24.5) for AES-GCM, 2^64-1
  for ChaCha20-Poly1305), `kTlsMaxKeyUpdates`, `kTlsMinKeyUpdateRecords` and `kTlsAlertUserCanceled`.
- `send()` updates the key between records so data takes at most `key_update_records() - 2` records per key (room for
  the KeyUpdate and one alert), counts the updates and a queued answer against the outgoing bound, and sends a message
  whole or not at all.
- `update_keys(request_peer)`; a second request while one is outstanding only updates the session's own key.
- `read()` moves the provider's answer to a peer's request into the outgoing bytes when they have room; `update_keys`
  sends a queued answer before its own update.
- `key_updates_sent()`, `key_updates_received()`, `records_under_key()` and `key_update_records()`.
- The bound: an own update that would reach `max_key_updates` sends `close_notify` instead (`Closed`); an answer that
  reaches it closes the session after it. The count never passes the bound.
- Closure: a peer's `close_notify` during the handshake, after `user_canceled` or not, makes the session `Closed` (it
  was `Failed`); `close()` then answers `Closed` without claiming a `close_notify` it cannot send. Cerid never sends
  `user_canceled`; a handshake is abandoned with `clear()`.
- The standards register's `tls.v13` names the evidence and the profile points proven; it stays planned until
  resumption, ticket hygiene and the QUIC and raw UDP record layers land.

## Evidence (cloud machine cerid-track-c, linux-gcc-debug unless named)

- `crd-crypto-tests`: 61 cases, 95,465 assertions; the `[tls]` cases now 19. Four new cases: record budgets for each
  AEAD (exact outgoing bytes: 10 one-byte records and 9 updates; a 60,000-byte message with 3 updates and the bound to
  the byte), updates on demand (answer at once, an answer held while a request is outstanding, a queued answer counted
  by `send()` and `update_keys()` and sent first), the bound (own updates, a record budget reaching it, a message that
  would reach it part way, a peer's request taking the last update) and the peer's `user_canceled` and `close_notify`
  during the handshake (ignored alone, then the handshake completes; both alerts; five warnings). Configuration
  refusals for both new fields.
- `crd-crypto-tls-interop-openssl`: 20 of 20 cases against the pinned OpenSSL 3.5.9, five new: a Cerid client
  updating before every record under a three-record budget, asking for an update, not asking, both together, and an
  OpenSSL client asking a Cerid server, each end's KeyUpdate counts matching the other's. The OpenSSL driver sends an
  owed KeyUpdate with a zero-length write, since OpenSSL otherwise holds its answer until its next record.
- `crd-fuzz-crypto-tls`: the oracle now admits `Closed` from a peer's close (no alert sent; every call refused with
  `Closed` after it) and checks `update_keys` and the counters; three new seeds (user_canceled then close_notify to a
  client, five warnings to a server, user_canceled ahead of an honest ClientHello); the corpus replays clean.
- 16 source mutations, each caught by the `[tls]` cases: an uncounted KeyUpdate, a second outstanding request, one
  record reserved instead of two, an own update reaching the bound, an answer left queued, a peer's close failing the
  session, `close()` claiming a `close_notify`, the queued answer missing from `send()`'s bound, an answer at the
  bound leaving the session open, an own update not sending the queued answer first, a budget above the AEAD limit
  kept, the budget minimum and the 2^48-1 maximum unchecked, a bound reached part way through a message, the outgoing
  bound ignored by `update_keys` and the AES-GCM limit rounded up.
- The linux-gcc-release preset (link-time optimisation): `crd-crypto-tests` 61 cases and the TLS corpus pass with no
  warning. LLVM 20.1.0 strict analysis of the five changed C++ files is clean. 81 scoped CTests (crypto, TLS, the
  `crd-no-*` and `crd-check-*` guards) pass; `crd-check-isa-isolation` fails only on ten objects of targets this
  partial tree never built (perf, gpu-context, hesap-tensor, kir, geometry tests from other tracks' commits), none of
  them crypto, so it is a property of the tree, not of this change.
- Commit range: the feat and docs commits of this batch on main.
