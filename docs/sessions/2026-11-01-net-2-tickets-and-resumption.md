# NET.2: tickets and resumption on the TLS sessions

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 38; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract:
[TLS sessions](../design/network-foundation.md#tls-sessions), decisions S6 and S9, and the [TLS 1.3 profile](../design/network-foundation.md#the-tls-13-profile)'s tickets.

## The user's decisions this batch acts on

- 2026-11-01, about 01:25: "approve the downloads we need to do everything properly, I will contect a lawyer later! DO
  NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD
  STANDARD AND ELITE". No download was needed: the pinned AWS-LC and OpenSSL 3.5.9 cover this slice.
- 2026-11-01, about 14:50: "I wanted you to finish all net rows and you stalled on very currently unnecessary things. Of
  course we think about the laws but it seems you are over complicating everything! I WANT NET DONE PROPERLY AND GOLD
  STANDARD AND WITHOUT ANY DEFERS!" The batch takes the next clause of NET.2's Remaining: the TLS 1.3 profile's ticket
  hygiene with resumption.

## CI first

Nothing to fix from this track. In complete-tier run 41298463221 (13453acb), which is still running, two jobs failed:
linux-clang-arm64-asan and linux-gcc-arm64-asan. Both fail the same three tests, `orient2d`, `orient3d` and `incircle`
against Shewchuk's predicates. AddressSanitizer reports a stack-buffer-overflow read in the vendored reference
`tests/geometry/geometry-primitives/reference/shewchuk-predicates.c:1064` (`fast_expansion_sum_zeroelim` reading past
`bterms` in `orient2dexact`). This is third-party reference code on the geometry track's row. It is not yet in
[third-party-defects.md](../third-party-defects.md), so the owning track should register it there. Track C did not
trace or change it. Complete runs 41311293265, 41319531955, 41302856153 and 41323535550 still had at most their
Preflight job queued, and push run 41326995883 (06459c77) was pending.

## What the provider does

Read in AWS-LC 5.12.0's libssl before the code was written, then proven by the tests:

- A TLS 1.3 server writes its NewSessionTicket after the client's Finished, but holds it until the server writes
  (`do_send_new_session_ticket`); a zero-length `SSL_write` releases it.
- Resumption is psk_dhe_ke only. A server checks a ticket's session ID context, its version, its exact cipher suite
  and its age before it resumes. A client checks the session ID context again, and does not offer a session that has
  expired.
- On a resumption the verify callback does not run: the peer's certificate comes from the ticket.
- Renewing a ticket on resumption keeps the longer of the remaining and the configured lifetime, capped by the
  non-renewable authentication timeout of seven days.
- The built-in ticket keys rotate inside one process every 48 hours and cannot be shared. `SSL_TICKET_AEAD_METHOD`
  hands sealing and opening to the caller.

## What landed (decisions S6 and S9)

- `TlsConfig::ticket_key`, `previous_ticket_key` and `ticket_lifetime` (servers only; 32-byte keys; two days by
  default, at most seven), `kTlsTicketKeyBytes`, `kTlsDefaultTicketLifetime` and `kTlsMaxTicketLifetime`.
- The ticket format sealed by the context's ticket method: the key's 16-byte name (HKDF-SHA256 of the key), a random
  32-byte salt, and the serialized session under AES-256-GCM with a key and nonce derived from the ticket key and the
  salt. Each derived key seals one ticket, so a fleet shares no ticket count. A server without a key draws one at
  `init()`. A ticket under an unknown name, too short or forged is ignored.
- One ticket per connection, a resumed one included, sent with the handshake's last flight. A server keeps no
  session cache.
- `TlsTicketStore` (the caller's, one per correlation domain) and `TlsBinding`, the SHA-256 of the role, the server
  name in lower case, the own key, the pins and the accepted algorithms. `TlsSession::init(context, tickets)` takes
  one ticket from the store and offers it only when it parses as a TLS 1.3 session of this binding. `read()` hands the
  store every ticket the server issues. Without a store a client offers nothing and ignores the server's tickets.
- The binding is the provider's session ID context on both ends. A client key that is no longer pinned therefore
  authenticates in full and is refused with `general_error`, and another credential never answers for the key a ticket
  proved.
- `TlsSession::resumed()`. After a resumption the session reads the peer key from the certificate the ticket carried.
- The verify callback's key check is now `pinned_peer`, used by full and resumed handshakes alike. The zero-length
  write is one `flush()`, used by key updates and by the server's tickets.
- 0-RTT stays off (S6). Early data may carry only operations a schema declares idempotent, and NET.2's schemas and
  anti-replay windows come later in the row.

## Evidence (cloud machine cerid-track-c, linux-gcc-debug unless named)

- `crd-crypto-tests "[tls]"`: 26 cases and 4,599 assertions, with seven new cases and the configuration refusals
  (all of `crd-crypto-tests`: 68 cases, 96,712 assertions):
  - **Resumption once.** A resumption with an ML-DSA-65 server key takes the four flights of a full handshake. The
    server's flight is 1,291 bytes instead of 10,086. On the wire there is one identity led by the key's name, with
    pre_shared_key last, psk_dhe_ke alone and a fresh X25519MLKEM768 share each way, and the ServerHello selects
    identity 0. Each ticket has a new salt, the ticket used is gone, and a client without a store offers nothing.
  - **Bindings.** Other pins, accepted algorithms, an own credential or another name each offer nothing. The name binds
    without regard to case. A careless store's ticket of another binding is dropped, and so is a ticket that does not
    parse.
  - **Fleet and rotation.** A fleet's second context resumes. Rotations resume under the previous key, a key that
    sealed none of the tickets runs in full, drawn keys stay with their own context, and another credential runs in
    full and reports its own key.
  - **Pinned clients.** A pinned client resumes and its server knows its key. Once unpinned it is refused with
    `general_error`.
  - **Damaged and expired tickets.** A byte changed at rest in the name, the salt, the ciphertext or the tag runs a
    full handshake. A one-second ticket is not offered after two seconds.
  - **HelloRetryRequest.** A resumption through a HelloRetryRequest offers the ticket twice and keeps X25519MLKEM768
    in six flights.
  - **Lifetimes.** A client keeps each ticket for the lifetime its server states (the default two days, 60 seconds and
    seven days), read back by the provider from the store's bytes. AWS-LC caps a client's ticket at the client
    context's own lifetime (two days by default), so a client context now allows the profile's seven days.
- `crd-crypto-tls-interop-openssl` (`tls_interop.py`): 24 of 24 cases against the pinned OpenSSL 3.5.9, four of them
  new, each with tickets and sessions kept in files across processes:
  - a Cerid client resuming twice at OpenSSL servers that share a ticket key;
  - an OpenSSL client resuming twice at a second Cerid server of a fleet;
  - an OpenSSL client resuming after two rotations and running in full at a server whose key sealed none of its
    tickets;
  - an OpenSSL client with a pinned credential resuming, with the Cerid server reporting its key.

  The driver checks the resumed hellos on the wire, and a Cerid ticket's key name against its own HKDF.
- `crd-fuzz-crypto-tls`: the server seals under a fixed key, and a new input mode hands the input to a client as the
  ticket its store gives out. The oracle requires that such a client still starts its handshake and keeps no ticket.
  Three new seeds: a ClientHello offering a forged ticket under the server's key name, a ticket that is not a
  session, and an empty ticket. The corpus of 13 replays clean.
- 17 source mutations, each caught by the `[tls]` cases:
  - no ticket per connection, or tickets held until the server writes;
  - the pins or the own key left out of the binding, or the name bound with its case;
  - a stored ticket of another binding offered, or the store never asked;
  - the previous key forgotten, or the key name not checked;
  - a zero salt, or a forged ticket failing the handshake;
  - the lifetime ignored;
  - resumption unreported, or the peer key not read after a resumption;
  - a client given ticket keys, or a lifetime above seven days;
  - a client keeping its tickets for two days whatever its server states.
- The linux-gcc-release preset (link-time optimisation), run before the lifetime case was added:
  `crd-crypto-tests` 67 cases (96,637 assertions) and the TLS corpus pass with no warning.
- LLVM 20.1.0 strict analysis of `tls.cpp`, `test_tls.cpp`, `fuzz_tls.cpp` and `cerid_tls_peer.cpp` is clean. The 19
  scoped CTests the tree registers (crypto, TLS, guards) pass.
- Commit range: the feat and docs commits of this batch on main.
