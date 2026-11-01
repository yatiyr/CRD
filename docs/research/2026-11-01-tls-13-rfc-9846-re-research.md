# TLS 1.3 after RFC 9846: the standards register's full re-research

<!-- doc-role: reference -->
> Technical reference and proposals; verify dated claims against current contracts/source. Live slice state exists
> only in [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md). Agent entry: [START_HERE](../../START_HERE.md).

**Date:** 2026-11-01. **Status:** Recorded research; the full run of the standards register's
[review](../design/registers.md#snapshot-and-review). Its amendments are proposals for the user. Nothing in
[ROADMAP](../ROADMAP.md) was edited by this research.

## Question, method and evidence boundary

The register's first refresh (2026-10-31) found the pinned RFC 8446 obsoleted by RFC 9846, and its trigger
(`tls.v13`) owed a full re-research by 2026-11-25. The question: what does RFC 9846 change for
[NET.2](../ROADMAP.md#slice-net.2)'s TLS 1.3 profile, and what else must the register hold because of it?

`scripts/register-review.py plan --scan full` planned 66 sources: the 65 records of a refresh plus the full text of
the document the trigger pins. All 66 were fetched over HTTPS on 2026-11-01 and are pinned by URL, size and SHA-256
in the scan's report, [2026-11-01-standards-register-full-review.md](../sessions/2026-11-01-standards-register-full-review.md).
The download falls under the user's approval of 2026-11-01 ("approve the downloads we need to do everything
properly"). RFC 9846 was read in full:
<https://www.rfc-editor.org/rfc/rfc9846.txt>, 339413 bytes, SHA-256
`b1bee06a814f92ca4677c85b97519be53869b2bf8edc79ade955fb51c0402b17`.

RFC 9847, which RFC 9846 cites for the registries' "Recommended" column, was read by hand:
<https://www.rfc-editor.org/rfc/rfc9847.txt>, 33240 bytes, SHA-256
`5f26e46fdda1b04860afef42b8e3162d861fdf86dad6ec200cd21fff9f279c6f`.

The items read by hand in the first refresh (the NIST object identifiers, FIPS 204, FIPS 186-5, NIST IR 8547, W3C
Trace Context and the IETF meeting calendar) were read on 2026-10-31, one day earlier, and are not read again.

Cerid writes no TLS: a vetted provider ([crypto](../design/network-foundation.md#crypto)) implements it. Every
requirement below is therefore something NET.2 proves of the provider and of Cerid's own record layers, not code
this research adds.

<a id="delta"></a>
## What RFC 9846 changes

RFC 9846 (July 2026, from draft-ietf-tls-rfc8446bis-14) is a minor revision of TLS 1.3. It keeps the version number
and the wire format and is backward compatible. Section 1.2 lists its technical changes. It also obsoletes RFC 5077,
5246, 6961, 7627 and 8422 and updates RFC 5705 and 6066. No register entry pins any of those documents, and neither
does the network foundation design.

| Change (RFC 9846) | What it means for Cerid | Row |
|---|---|---|
| Key shares are never reused between connections (4.2.8). Receivers must still accept a reusing peer, because RFC 8446 permitted reuse. | Every Cerid endpoint generates a fresh share per connection, including the ML-KEM half of each hybrid share. A peer's reuse is tolerated, never relied on. NET.2's provider conformance tests observe it. | NET.2 |
| TLS 1.0 and 1.1 must not be negotiated (RFC 8996). | Cerid endpoints speak TLS 1.3 only. Hybrid key exchange exists only in TLS 1.3, and the design names no TLS 1.2 path. A ClientHello without TLS 1.3 is refused with `protocol_version`. | NET.2 |
| The hash used with pre-shared keys and HelloRetryRequest is unambiguous. | The provider's PSK binder and stateless retry follow it. A stateless HelloRetryRequest stores only the hash of ClientHello1, which is the T01 design. | NET.2, NET.4 |
| A client that does not support resumption must silently ignore NewSessionTicket. | Cerid clients without a ticket store (agents, one-shot crash uploads) ignore tickets. | NET.2 |
| Initiating a key update before the key usage limits is now a MUST, and the scope of the limits is clarified (5.5). For AES-GCM that is 2^24.5 full-size records per key. Early data cannot be rekeyed, so it must stay under the limit. Receivers should not enforce the limits. | Every record layer Cerid runs counts the records protected under each key and updates the key before the limit. That covers TLS records, QUIC packets (the confidentiality and integrity limits of RFC 9001, section 6.6) and the authenticated raw UDP path. 0-RTT data is capped below the limit. A receiver never closes a session for its peer's count. | NET.2, NET.3 |
| The number of KeyUpdate messages is limited (4.7.3). The epoch must not exceed 2^48-1, a sender has at most one `update_requested` outstanding, and a sender that would exceed the bound ignores the request. | A key update is also where a session adopts a new policy generation ([agility](../design/network-foundation.md#agility)), so policy swaps are counted in the same epoch. A session that reaches the bound closes cleanly and resumes. | NET.2 |
| `close_notify` is again a warning-level alert. `user_canceled` must be followed by `close_notify` and is otherwise ignored. | Orderly closure and the error taxonomy of a session follow it. | NET.2 |
| A new `general_error` alert (117) for errors with no specific alert, or whose specific code the sender conceals. | Toward a peer that is not yet authenticated, a Cerid server answers with `general_error` where a specific alert would be an oracle: for example whether a PSK identity or capability exists (F.6). Authenticated peers get the specific alert. | NET.2, NET.4 |
| Length bounds corrected: `CertificateRequest.extensions` and `ClientHello.extensions` lower bounds, `NewSessionTicket.extensions` upper bound. | The fuzz and malformed-input corpora of NET.4 include the boundary lengths. | NET.4 |
| Key-exchange text is generic ("KEM-based" as well as (EC)DHE). | The hybrid groups of RFC 10024 are first-class in the base specification's terms. Nothing changes. | none |
| The transcript hash's interaction with future specifications is clarified. | Cerid's own negotiation binds its transcript independently ([negotiation](../design/network-foundation.md#negotiation)). Nothing changes. | none |
| The requirement for RSA-PSS is removed, matching RFC 9963 (legacy RSASSA-PKCS1-v1_5 codepoints for TLS 1.3). | Cerid's policy accepts no RSA, so nothing changes. | none |
| "master" secrets are renamed "main", and `extended_master_secret` becomes `extended_main_secret`. | Editorial. Cerid never speaks TLS 1.2. | none |

The revised security considerations and implementation notes add four requirements for the profile:

- **Tracking prevention** (C.4). A client never reuses a ticket and never offers tickets across connections meant to
  be uncorrelated. A server issues a new ticket with every connection. This meets T07's single-use tickets and T15's
  metadata privacy. External PSK identity labels are random, never an identifier of the user.
- **Unauthenticated operation and misbinding** (C.5, F.9). Raw public keys and pinned Cerid credentials (T03) are
  safe only when bound out of band. When they authenticate a server, the server checks that `server_name` names its
  expected identity, or both sides use `external_id_hash` (RFC 8844).
- **PSK identity exposure** (F.6). A server that accepts only PSK handshakes treats an unknown identity and a wrong
  binder identically.
- **External PSKs** (F.7, F.8). A PSK is never shared between TLS 1.2 and TLS 1.3, and is never shared beyond one
  client and one server unless it goes through an importer (RFC 9258).

<a id="recommended-column"></a>
## The registries' Recommended column (RFC 9847)

RFC 9847 (December 2025) updates RFC 8447. It adds a third value, "D" (discouraged), to the "Recommended" column of
the TLS registries:

- "Y": the IETF recommends the item for its purpose.
- "N": the IETF has not evaluated the item, or its applicability is limited. This is not a flaw.
- "D": the item should not or must not be used, as its references explain.

Setting a value to "Y" or "D", or moving it away from "Y" or "D", requires Standards Action with Expert Review or
IESG Approval. RFC 9847 itself marks these entries "D":

- in ExtensionType Values, `truncated_hmac`, `connection_id (deprecated)` and two reserved values;
- in Cipher Suites, the Kerberos, NULL, RC4 and export suites and the two integrity-only TLS 1.3 suites;
- in Supported Groups, the binary and small prime curves (values 1-8 and 15-21);
- in HashAlgorithm, md5, sha1 and sha224;
- in ClientCertificateType Identifiers, three reserved values.

None of them is a codepoint Cerid uses.

The column is now register data. Every verified codepoint of the TLS Supported Groups, SignatureScheme and
ExtensionType registries records IANA's value. The review reads it (`<rec>` in the Supported Groups and ExtensionType
files, `<recommended>` in the SignatureScheme file). A change is applied. A pinned entry whose codepoint IANA comes
to discourage triggers a full re-research, and the guard refuses that entry until it is dropped or deprecated
(`tls.registry-recommended`). This scan recorded the column for the first time:

| Codepoint | Entry | Recommended |
|---|---|---|
| X25519MLKEM768 (0x11EC) | `tls.hybrid-kex` | Y |
| SecP256r1MLKEM768 (0x11EB), SecP384r1MLKEM1024 (0x11ED) | `tls.hybrid-kex` | N |
| x25519, secp256r1, secp384r1 | `crypto.ecdh` | Y |
| ed25519, ecdsa_secp256r1_sha256, ecdsa_secp384r1_sha384 | `crypto.eddsa`, `crypto.ecdsa` | Y |
| mldsa44, mldsa65, mldsa87 (0x0904-0x0906) | `tls.ml-dsa` | N |
| encrypted_client_hello, compress_certificate | `tls.ech`, `tls.certificate-compression` | Y |

The "N" values are deliberate choices, not defects: SecP384r1MLKEM1024 is the high-assurance group, and ML-DSA signs
Cerid-issued certificates. No codepoint the register pins is discouraged.

<a id="ml-dsa-in-tls"></a>
## ML-DSA in TLS 1.3

The crypto policy pins ML-DSA (RFC 9881) for Cerid-issued service certificates. A TLS 1.3 server presenting one
signs CertificateVerify with an ML-DSA signature scheme. The register held the certificate profile's object
identifiers (`pki.ml-dsa`) but no TLS codepoints. That was a gap. The TLS Supported Groups, SignatureScheme and
ExtensionType records read in this scan show:

- `mldsa44` (0x0904), `mldsa65` (0x0905) and `mldsa87` (0x0906) are assigned in the TLS SignatureScheme registry by
  draft-ietf-tls-mldsa, Recommended "N".
- The draft's revision -06 (posted 2026-09-11) is in the RFC Editor queue.

The new planned entry `tls.ml-dsa` (NET.2) holds the three codepoints, which this scan confirmed against IANA, so
`crd/crypto/register_ids.hpp` gains them as verified constants. When the draft is published as an RFC, a light scan
proposes re-pinning the entry.

The "classical companion" the policy pairs with ML-DSA is not yet a TLS signature scheme. Composite ML-DSA has no
TLS codepoint in the registry this scan read. Until it has one, NET.2 decides between a dual-certificate deployment
and a composite scheme (proposal 4 below).

## Proposed ROADMAP amendments (for the user)

1. **NET.2 cites RFC 9846, RFC 9847 and draft-ietf-tls-mldsa** for its TLS 1.3 profile. It adds the requirements in
   [What RFC 9846 changes](#delta) to its provider conformance tests: fresh key shares, key updates before the usage
   limits on every record layer, the epoch bound, `general_error` toward unauthenticated peers, ticket hygiene,
   misbinding defence, and TLS 1.3 only. This track holds NET.2 and applies the amendment when it reaches the row.
2. **masque.connect-udp-listen (NET-REACH)** is unchanged since 2026-10-31: still in the RFC Editor queue. NET-REACH
   decides once it is an RFC.
3. **media.rtp-over-quic (NET-LIVE)** is unchanged since 2026-10-31: expired. NET-LIVE decides whether it stays
   watched.
4. **NET.2 decides the classical companion of ML-DSA in TLS**: a dual certificate, or a composite signature scheme
   once one is registered. Pure ML-KEM groups (MLKEM512, MLKEM768, MLKEM1024; values 512-514 from draft-ietf-tls-mlkem,
   Recommended "N") are also assigned. NET.2 decides whether a high-assurance profile (CNSA 2.0) offers pure
   ML-KEM-1024 beside the hybrid default. The register watches neither until NET.2 decides.

## Register changes of this run

The report lists the 18 changes the scan applied:

- the Recommended values above;
- `tls.ml-dsa`'s three codepoints confirmed and its revision -06 recorded.

These were made by hand, before the scan, so that it would read their sources:

- the new entries `tls.ml-dsa` and `tls.registry-recommended` (RFC 9847, implemented by the guard and the review);
- `tls.v13`'s source, note and review date, moved to this document.

The run is recorded as the register's second full run. The next review is the light scan after IETF 127.
