# Test instruments: bounded ingestion fuzzing and the sanitizer model of the fiber scheduler

<!-- doc-role: reference -->
> Contract for REPO.DEV.9. Status lives only in [ROADMAP](../ROADMAP.md#slice-repo.dev.9); the accepted research is
> [public consumption and stronger instruments](../research/2026-09-12-large-cpp-development-and-ci.md#public-consumption-and-stronger-instruments);
> the harness is [tests/support/fuzz](../../tests/support/fuzz/include/crd/fuzz/harness.hpp) with
> [CrdFuzz.cmake](../../cmake/CrdFuzz.cmake) and [fuzz.py](../../scripts/fuzz.py); the sanitizer model is
> [sanitizer_fibers.hpp](../../engine/foundation/jobs/src/sanitizer_fibers.hpp).

**2026-09-14 extension:** [DIAG](runtime-diagnostics.md) owns the next instrument programme after REPO.DEV.
The measurements below are dated evidence. TSan switch flags and the instrument-only fence substitution require
negative-control/model qualification in DIAG.1b; a clean probe does not prove annotations cannot conceal races.
DG05's immediate free-list repair transfers from CORE-USE.2 to DIAG.1a. DIAG.3f owns MSVC UAR and initialization/leak
coverage; DIAG.10a/DIAG.11b own expanded fuzzing/CI. Existing REPO.DEV evidence is preserved.

Purpose: the two existing ingestion surfaces (CEIR text and binary modules, CKIR text and binary graphs) have
reusable, bounded fuzz targets whose corpus is a permanent regression set on every lane; every sanitizer the tree
offers requires qualification rather than assumption, so a lane cannot print a report and stay green; fiber stack
coverage and concurrency annotations require explicit controls before any lane claims
concurrency coverage. The row qualifies instruments and the current consumers; a defect it finds in an allocator or
the scheduler gets an owner row, not a repair here.

## The bounded fuzz harness

A fuzz target is one translation unit defining `LLVMFuzzerTestOneInput` and `crd_fuzz_seeds`. `crd_fuzz_target()`
builds it twice: the replay executable (the target plus `replay_main.cpp`, which walks corpus directories, feeds
every file to the target, reports the count, the bytes, the inputs the target rejected and the slowest input, and
exits non-zero on any failure or an empty corpus) is registered as the `<target>-corpus` CTest of every
configuration; under `CRD_ENABLE_FUZZER` the same source also links libFuzzer as `<target>-libfuzzer`.

Bounds: a target returns early above `kMaxInputBytes` (64 KiB); each input gets a fresh `BudgetAllocator` (64 MiB)
whose allocation beyond the budget ends in a fatal that names the budget, a reported finding with a small footprint
instead of an RSS kill; the fuzzing side adds libFuzzer's `-max_len`, `-timeout`, `-rss_limit_mb` and
`-malloc_limit_mb` through `scripts/fuzz.py`. Oracles: a rejected input must leave nothing behind; an accepted input
must satisfy the round-trip the unit tests already hold (`print(parse(print(x))) == print(x)`, byte-exact binary
serialization, `serialize_graph(ckir_read(ckir_write(g))) == serialize_graph(g)`), so a fuzz finding is a loader or
serializer defect, never a harness opinion. Since DIAG.10a each loader target also crosses to the other form: an
accepted CEIR text module must serialize, deserialize and print back to the same text, an accepted CEIR blob must
print text the parser accepts and prints identically, and an accepted CKIR blob must write `.ckir` text that reads
back to the same blob; the CKIR text target already compared the blobs of the two reads.

| Target | Loader | Corpus (seeds + adopted findings) |
|---|---|---|
| `crd-fuzz-ceir-text` | `crd::ceir::parse` | 27: the rich graph's text, the hand malformed table of `test_malformed.cpp`, eight adopted findings and one input adopted after it crashed only inside a libFuzzer process (DIAG.10a) |
| `crd-fuzz-ceir-binary` | `crd::ceir::deserialize` | 30: the rich graph's blob, truncations, a flipped count, garbage, ten blobs holding a name or value the text form cannot express (DIAG.10a, each checked refused as it is written), ten adopted findings |
| `crd-fuzz-ckir-text` | `crd::kir::ckir_read` | 13: the D1 scale kernel and the raster fragment entry as written, the malformed table of `test_ckir_asset.cpp`, six adopted findings |
| `crd-fuzz-ckir-binary` | `crd::kir::deserialize_graph` | 30: the two graphs' blobs, truncations, a flipped byte, garbage, seven non-canonical records (DIAG.10a: a dim past the rank, a rank past 8, out-of-range op, dtype, tier and stage, a stray stage output; each checked refused), nine adopted findings and one input adopted after it crashed only inside a libFuzzer process |
| `crd-fuzz-perf-bundle` | `crd::perf::import_bundle` (DIAG.5d) | 12: the golden bundle, truncations, traversal, a lying length, a decompression bomb, a wrong schema, a hostile section count, build identities |
| `crd-fuzz-memory-allocators` | every production allocator and the `SlotMap`, as operation sequences, the `VirtualMemoryAllocator` and `StreamingAllocator` on small per-input reservations (DIAG.10a) | 37: 31 written streams (one or more per allocator, malformed lengths and alignments, a stream cut mid-operation, one per finding of the written seeds; for the arena exact exhaustion, commit blocks, purge and recommit, reallocation paths; for the streaming layer the budget charge, unservable requests, policy pressure, a lying policy, staging and the category view) and 6 adopted libFuzzer inputs |
| `crd-fuzz-perf-flight` | `crd::perf::decode_flight_record` (DIAG.10a) | 30: seven captures a real `FlightRecorder` wrote (every end reason, a Basic policy, a suppressed trigger, a replay recipe, a clipped message, an empty post window, the fatal path without the ring) with fixed clock readings, ten records that contradict themselves in one way each, truncations, a trailing byte, a disagreeing count, a text length at its bound, an unknown detail level, garbage |
| `crd-fuzz-perf-assets` | `parse_flight_policy_asset`, `parse_fault_plan_asset`, `parse_fault_plan` and crd-toml beneath them (DIAG.10a) | 41: the two committed assets, documents for every refusal the readers name, crd-toml's nesting, integer-range and number-length limits, plan texts at their bounds |
| `crd-fuzz-perf-capture` | `crd::perf::validate_capture_buffer` and `CaptureView` (DIAG.10a) | 33: three captures in the writer's exact layout (one with frames and a correlation section, one whose frame span overflows a 64-bit difference) and one buffer per reader defect DIAG.10a fixed, truncations, garbage, and one adopted libFuzzer finding (a sample span the Perfetto export overflowed) |
| `crd-fuzz-ceir-record` | `crd::ceir::cook::decode_record`, the run-record reader (DIAG.10a) | 57: eight records in the three recorders' shapes (plan, host and device; a bounded trace, an unavailable read, a reloading host's asset and generation, an ulp and an exact envelope) with a fixed build, 30 records that contradict a recorder in one way each, truncations as given and resealed, a trailing byte, a wrong checksum, an older schema, three counts past the payload, garbage |
| `crd-fuzz-ceridc-diag` | command decoding: ceridc's MCP server (`McpServer` over `crd::assetio::json::parse`, the dual-era protocol of MCP-ADAPT.0), its diag tool and `DiagCommandService::execute` with every registered command's argument check (DIAG.10a) | 58: the JSON-RPC lifecycle and the 2025-11-25 handshake, the 2026-07-28 per-request `_meta` (version error, malformed metadata, a cursor, ping), the `jsonrpc` member, every id form (string, integer, null, fraction, past 2^53, object), batch arrays and bare values, names with a NUL, the parser's number, UTF-8, escape and nesting boundaries (15 refused), the diag tool's fields and refusal order, `bundle.inspect`'s `replay` argument, `state.divergence`'s directory and `reference` argument, cursors, a 65-byte command, native requests of raw bytes (a stale cursor before any snapshot, a cursor past 2^53), the flight, fault and CEIR commands under hosts with and without a root, and `hardware.check` within its host's bounds |
| `crd-fuzz-resources-reload` | `crd::resources::crdr_read` and the `ResourceManager` pack ingress: `mount_manifest`, `unmount`, `load_sync`, `reload_mount_now` and eviction (DIAG.10a) | 34: seven packs that mount (honest, compressed manifest, empty, a name past the pool, a zero-length entry, artifacts the loaders refuse, trailing bytes), six refused whole (torn, a range that wraps, a duplicated id, not a PACK, no manifest, a manifest of partial entries), four compressed-chunk claims (past the bound, a sum that wraps, a byte past the frame, exact), three structural, and 14 scripts (reload swaps, a torn and a wrapping reload, a shadowed id, a retyped id reloaded and re-issued, unmount falling back, a reload that drops and adds, a resized reload, a duplicated id on reload, pins and drains, placeholders and last-good, in-place corruption, invalid mounts) |
| `crd-fuzz-ceir-differential` | the compiled execution tier (`crd::ceir::plan::compile` and `run`) against the reference interpreter (`crd::ceir::exec::Interpreter`) on generated programs, with their printed and serialized twins (DIAG.10a) | 35: 32 seeds: straight-line arithmetic over i64 edges, if, for, while, switch and match, the three scope kinds, the three cell kinds with feedback and depths, calls, tokens (launch, spawn, worker, main thread, join, await, race, fiber wait, continuation), host inputs (with a script, exhausted, without a source), map_reduce in index order, and one per finding (a while condition at a small budget, recursion at and past the nesting bound directly and through a map_reduce body, a re-entered map_reduce, a 2^31-index and a full-i64 parallel range, a for step past INT64_MAX, a fold that runs out of steps, a loop that launches tokens and runs a parallel op) and three adopted libFuzzer inputs (a parallel op inside a parallel body; a token store that grew quadratically; a parallel body re-entering the program, its work growing with each nesting level) |
| `crd-fuzz-net-negotiation` | crd-net's negotiation (NET-STD): `decode_negotiation` of offers and answers against a restated wire layout, an honest server's answer to any accepted offer that a client which could have sent it accepts, the fixed client's verdict on any answer restated from the selection and downgrade rules, and generated policy pairs through the whole exchange | 16: an honest offer and answer from the writers, a truncation of each, no versions, a future format, another protocol, a trailing byte, a repeated extension, answers selecting a major above the offer's minor, a downgrade and a GREASE major, an answer accepting a known extension the client did not offer, and generated exchanges sharing a major, sharing none and with an invalid policy |
| `crd-fuzz-net-admission` | crd-net's frames and admission stages S6 and S7 (NET.3): any bytes on a stream and in datagrams against a restated frame layout, integrity hash, padding and admission rule, a stream fed at any split reading exactly as it reads whole, fragments of generated messages lost, repeated and reordered delivered intact at most once, and every quota, pending place and buffer given back | `tests/foundation/net/fuzz/corpus-admission` |
| `crd-fuzz-wire-delta-test` | crd-net's baseline deltas (NET.3), generated from their test schema: on a baseline its decoder accepts, what a delta rebuilds passes the decoder or is refused and its delta is exactly the input, and the rest read as a second message has a delta that rebuilds it exactly | `tests/foundation/net/fuzz/corpus-wire-delta-test` |
| `crd-fuzz-net-coded-frames` | crd-net's coded frames (NET.3): any header and body on coded channels against a restated rule and decode that decide every verdict, a stream at any split, and a writer's frame honest and damaged; every budget and buffer comes back | `tests/foundation/net/fuzz/corpus-coded-frames` |
| `crd-fuzz-compression-messages` | crd-compression's entropy coder and dictionary frames (NET.3): any bytes as a coding (what decodes re-encodes to itself), and any bytes coded both ways | `tests/foundation/compression/fuzz/corpus-messages` |
| `crd-fuzz-net-endpoint` | crd-net's address and endpoint texts (4.2a): any text read both ways, every accepted value printed in its bound and read back to itself with one text, and on Linux the same verdict and bytes as glibc's `inet_pton` | `tests/foundation/net/fuzz/corpus-endpoint` |
| `crd-fuzz-net-ingress` | crd-net's QUIC headers and admission stages S1 to S3 (4.2a): any datagram parsed with views inside it, varints written back in their shortest form, and through a server only held connections routed, only full client Initials admitted, every Version Negotiation or Retry within three times the datagram and parsing back (one reserved version listed; the Retry tag verified); and through a client's ingress only held connections routed, never 0-RTT, and nothing answered | `tests/foundation/net/fuzz/corpus-ingress` |
| `crd-fuzz-net-qpack` | crd-net's QPACK (4.2a): any bytes on the encoder stream, decoder stream and as sections; a table written apart predicting every accepted or refused instruction, field, blocked stream and acknowledgement; encoder and decoder over a delayed, split stream; Huffman round trips | `tests/foundation/net/fuzz/corpus-qpack` |
| `crd-fuzz-net-ticket-cache` | crd-net's client ticket store (4.2a): any keeps, takes and clock steps under any bounds, against a model of the contract: every ticket returned byte-exact and once, the newest of its binding, the size and every counter as predicted, nothing allocated. |
| `crd-fuzz-net-state-hash` | crd-net's `StateHashExchange` (NET.3): any script of records, honest and damaged reports and a restart, against an unbounded restatement that decides every verdict, chain and divergence | `tests/foundation/net/fuzz/corpus-state-hash` |
| `crd-fuzz-net-delta-stream` | crd-net's `DeltaReader` (NET.3): any frame bodies, whatever is delivered passing the generated decoder, every read counted, no read allocating; and a `DeltaWriter`'s frames delivered, repeated and lost by the input's script, whatever is delivered being byte for byte what was written at a sequence above every earlier one | `tests/foundation/net/fuzz/corpus-delta-stream` |
| `crd-fuzz-wire-session-control`, `crd-fuzz-wire-test` | crd-net's generated wire formats (NET.3), one target generated per schema by `scripts/gen-wire-schemas.py`: on every accepted message a list walk, a repeated decode, the schema's size bounds and a re-encoding byte for byte with the input (a decoded value of quantised fields decodes again at the same size), at the schema's minor revision, older ones and newer ones. |
| `crd-fuzz-crypto-capability` | crd-crypto's capabilities (NET.2): `decode_capability` and `decode_revocations` against restated layouts, bounds and validation orders (the exact error, and an accepted token's counts, dates and views); and generated lives, each a root signed by an ML-DSA-44 and Ed25519 issuer pair, up to five delegations by Ed25519 and ECDSA holders that are narrower or widened in one field, a revocation generation (none, fresh, expiring, or listing the token, a key of the chain, an issue cut-off or a policy generation), an audience (its lifetime bound, peer-only), a time and a presentation (honest, another session's binding, a flipped proof byte, another peer): every delegation refused exactly when a restated containment rule says it widens, every verdict the one a model of the verification order gives, and any flipped bit of a token that verified refused | 21: an honest token, the same truncated, with a trailing byte, a wrong magic, a future format, 17 ranges and a lifetime of years, a delegated token and the same with an empty block signature; an honest revocation generation, truncated, with a trailing byte and with an entry twice; and lives: honest, delegated to an ECDSA holder bound to its peer, a refused widening, a bit flipped after verifying, late, revoked by its token, under another session's binding and a Proof token at a peer-only audience |
| `crd-fuzz-crypto-policy` | crd-crypto's policy generations (NET-STD): `decode_policy` against a restated layout and validation order, and generated sequences of generations written, signed by the test signer and applied to one store against a model of the adoption rules, with every snapshot's freshness, algorithm status and version minimums checked at the window and validity boundaries | 13: an honest generation from the writers, a truncation, a trailing byte, a wrong magic, a future format, too many minimums, an expiry before issue, no signature and a repeated signer, and sequences of honest growth, a rollback and replay, a forged and a half-signed generation, and an expired and a future one |
| `crd-fuzz-crypto-provider` | crd-crypto's provider interface over AWS-LC (NET.2): AEAD openings of hostile input, which must re-seal to exactly their input or release no byte; signature verdicts that are deterministic and refused by length; key agreement with arbitrary own and peer keys (an accepted EC peer is an uncompressed point, an X25519 secret is never zero); HKDF outputs of two lengths, the shorter a prefix of the longer; keys from arbitrary seeds whose signatures verify and do not verify a changed message; ML-KEM keys from arbitrary seeds that decapsulate their own encapsulations, a changed ciphertext to another secret and any ciphertext of the right length deterministically (implicit rejection); encapsulation keys accepted exactly when a restated FIPS 203 modulus check accepts them; and the table PRF against a restated SipHash-2-4 | 14: an honest ChaCha20-Poly1305 message and its forgery, RFC 8032's second Ed25519 signature, X25519 with the base point and with a zero peer, an off-curve P-256 peer, HKDF within and above its bound, ML-DSA-65 sign-then-verify from a fixed seed, an ECDSA scalar of zero, an ML-KEM-768 round trip with a changed bit, an ML-KEM-1024 encapsulation key from a fixed seed and the same key with a coefficient of q, and SipHash-2-4 with the paper's key |
| `crd-fuzz-crypto-quic` | crd-crypto's QUIC packet protection and address-validation tokens (NET.2): header protection refuses exactly the packets too short to sample (restated) and round-trips; a packet that opens re-seals to its input and one that does not releases no byte; a Retry is refused by length as restated and verifies only with the computed tag; packet numbers decode to the restated closest candidate and every encoding decodes back; and a token's verdict is the one its restated format, binding and lifetime give, once only for a NEW_TOKEN token under a replay filter | 23: a packet of both versions under each of the three suites and the same six forged, RFC 9001's and RFC 9369's Retries, RFC 9000's packet-number examples, and tokens sealed by the restatement (a Retry token, a NEW_TOKEN token for an IPv6 client, one under the previous key replayed; refused: another port, short, late, under an unknown key, a wrong sealed length byte) |
| `crd-fuzz-crypto-mls` | crd-crypto's MLS foundations (NET.2): vector headers and presence octets against a restatement of RFC 9420 section 2.1.2, tree math against its own invariants, HPKE round trips with any bit changed, hostile openings and signatures through the labelled operations, derivations (determinism, the length in the label, DeriveTreeSecret as ExpandWithLabel), and sequences of reads, spends and sends on a secret tree against a model of the receive window and section 9 restated | 22: the header extremes, the widest and a three-leaf tree, an HPKE round trip in each suite and the same changed, a hostile opening and signature, a derivation at the widest length, and secret-tree sequences |
| `crd-fuzz-crypto-mls-commit` | crd-crypto's MLS proposals, commits and external joins (NET.2): a fresh member that joined an honest group of three from its Welcome (each suite of the profile, public and private messages) processes any message after the honest Update proposal, and the honest proposal, the honest commit (the Update by reference, a path) and the honest external commit of a fourth member changed in any byte; a refusal leaves the member as it was (epoch, authenticator, tree hash, cached proposals) and stages nothing, the honest proposal is cached under its sender's reference and the honest commits are accepted, stage exactly their committers' epoch authenticators and merge to them, and no other message is accepted; a joiner refuses the creator's published GroupInfo changed in any byte and is left empty, and from the intact one joins into an epoch the member then takes | 69: an empty message, a public and a private header of no group, a Welcome and a short input; the honest commit and proposal unchanged in each suite and wire format, and each changed at twelve offsets; the honest external commit and GroupInfo unchanged in each suite, and each changed at eight offsets |
| `crd-fuzz-crypto-mls-group` | crd-crypto's MLS groups (NET.2): every group decoder (Welcome, GroupInfo, GroupSecrets, GroupContext, required_capabilities, an ExternalSender and the external_senders list) writes back exactly the bytes it took; an Extension list's verdict on repeated types against a pairwise restatement of RFC 9420 section 13.4 and each type's lookup against a linear scan; an honest Welcome a creator sealed to a joiner (each suite of the profile, the tree carried in the GroupInfo or fetched) changed in any byte is refused with the group left empty, or it is unchanged and the joiner reaches exactly the honest epoch authenticator | 24: a Welcome, a GroupInfo, GroupSecrets with a path secret and a PSK, required capabilities, two external senders, extension lists with and without a repeat and a short input; a Welcome unchanged in each suite and changed at twelve offsets |
| `crd-fuzz-crypto-mls-message` | crd-crypto's MLS messages (NET.2): every structure decoder (MLSMessage, Proposal, Commit, KeyPackage, LeafNode, FramedContent, AuthenticatedContent) writes back exactly the bytes it accepted, a private message's header is the restated prefix, a private message opens to what was protected and, changed in any byte or hostile, is refused with its plaintext zeroed and nothing spent (then Replayed once opened), and a public message verifies exactly when unchanged | 31: a public Remove, a commit by reference, a commit leaf with an X.509 credential, a private message's shape and a Welcome with no body (refused); a private message of each content type in each suite and one changed per suite; a well-formed private message with zero seals and one with short sender data; a public message from each sender, unchanged and changed |
| `crd-fuzz-crypto-mls-tree` | crd-crypto's MLS ratchet tree and TreeKEM (NET.2): an accepted ratchet_tree extension writes back exactly and a copy writes back the same, every resolution and filtered direct path against RFC 9420 section 4.1 restated over the public accessors, the join-time checks giving one verdict, an accepted UpdatePath writing back; Add, Update and Remove against a model (doubling, truncation, each leaf, unmerged lists and blanked paths) with the held tree hashes equal to a fresh decode's; an honest update path changed in any byte refused with the tree unchanged, or decrypting only to the honest commit secret | 20: a hand-written three-leaf tree in each suite, it ending at a parent, a lone blank node and a short input; change sequences that grow, churn, shrink and touch blank leaves; a path unchanged and changed at eight offsets |
| `crd-fuzz-crypto-tls` | crd-crypto's TLS 1.3 sessions (NET.2) fed hostile bytes as a server that has received nothing and as a client that has sent its ClientHello, delivered whole or in pieces: neither is ever established (that needs keys from its own fresh share or a signature by its pinned key), `receive` answers only the documented errors, a failure is final, a peer's `close_notify` closes the session without an alert from it and for good, send, read, close and key updates are refused, no key update is counted, and every buffer stays bounded; a client offered any ticket by its store starts its handshake and keeps none; QUIC handshakes fed CRYPTO data per level: never established, only the documented answers, no client Application secret, every held secret derived once; ECH (RFC 9849): a client given any ECHConfigList is refused exactly when the list's restated shape is wrong, and otherwise refuses to start or sends an outer ECH offer without its server's name, over records and QUIC, and a server holding two ECH keys reports no ECH for any hello; and compressed Certificates (RFC 8879) decode only for zstd or zlib, to a claim within the bound that their bytes can reach (restated), leaving a refused claim's output untouched, with zlib's Adler-32 restated and the same bytes twice | 40: a ClientHello with RFC 7748's X25519 key whole and in 7-byte pieces and with a forged ticket, a ServerHello with RFC 7748's other key, tickets that are no session and empty, a record header above the record limit, an SSL 2.0 hello, a handshake_failure alert, nothing, `user_canceled` then `close_notify` to a client, five warnings in a row to a server, a `user_canceled` ahead of an honest ClientHello, a Certificate-shaped message compressed with zstd and with zlib claiming its length, one byte more and far more than the bound, and a brotli claim; 8 QUIC inputs (the hellos as CRYPTO data, a QUIC ClientHello with and without a forged ticket, a ServerHello and EncryptedExtensions, data at a level not read yet, a TLS KeyUpdate, an overlong piece); and 12 ECH inputs (the server's list, after another version, with an unusable KEM, a mandatory extension, an IPv4-like public name, cut short and empty; outer hellos with the current, an unknown and the previous config_id, the inner type and lying lengths) |
| `crd-fuzz-comply-rules` | crd-comply's rule generations (COMPLY.0): `decode_rules` with the contract's invariants (named scopes, unique ids, real and ordered days, no claimed proof) and a canonical reprint restated from `scripts/gen-comply-rules.py` that must decode to the same generation, and generated sequences applied to one `RuleStore` against a model of monotonic adoption that compares whole texts and a brute-force query in request order | 14: a generation with escapes and month and year days, a truncation, an unknown key, an unknown and a duplicate scope, an impossible day, a sunset before its effective day, a claimed proof, a future format, control-character escapes, and sequences of adoption, a conflict and its replay, rollbacks, and a rule queried on its effective and sunset days |
| `crd-fuzz-comply-sanctions` | crd-comply's sanctions screening (COMPLY.1): `decode_sanctions` with the contract's invariants (named authority, real days, a bounded age, a SHA-256, unique parties and destinations, ISO regions, births only for individuals, folded names within bounds, identifiers `kind[.cc]:value` in compared form) and a second decode to the same digest; the name matcher's idempotent folding (folded words written back and folded again), fixed-point letters, self-score 1000 and score bounds; and screening requests against one store checked by a model of gaps by day, brute-force party scores, identifiers agreeing by kind, value and issuing country, birth intervals, dispositions from the agreements, destinations by region and the verdict's precedence | 28: a list with escapes (a birth interval, an identifier's issuing country, an unknown listing day), a truncation, a duplicate party and destination, an individual's birth on an entity, a bad region, an age past a year, an identifier in capitals and one with an upper-case country, a reversed birth interval, a wordless alias, a future format, name pairs (Turkish letters, a subset, joined words, legal forms, Cyrillic, Greek and fullwidth letters, ill-formed bytes) and screenings (a birth match, an identifier, one agreeing across a missing country and one refused by another country, a birth interval, a discount, an entity, a missing and a stale list, destinations, a day before publication) |
| `crd-fuzz-hesap-erasure` | hesap-net's Reed-Solomon code (HESAP-NET): `ReedSolomonCode::decode`, `encode` and the prepared path against the refusal order restated from erasure.hpp, every encoded block held to the n - k checks of RFC 5510's dual code with a shift-and-add multiply, every decoded symbol equal to its lost source, no byte written by a refusal or past a symbol's length; and the `hesap.erasure` dot product and sum of every level the CPU runs, at any alignment, against a shift-and-add evaluation and a byte loop | 24: decodes of (3, 7), (1, 1), (1, 255) and seeded patterns of (200, 255), (16, 24) and (255, 255); each decode refusal (a count, a received index past n, a repeat, a received source asked for, a repeated loss, an extra output, a 32-bit index, a short pattern); encodes of (4, 8) and (100, 160) and their refusals; kernel requests in the self-check's shape, misaligned and long, accumulating, and without sources |
| `crd-fuzz-hesap-flexfec` | hesap-net's FlexFEC parity (RFC 8627, HESAP-NET): `flexfec_parse` against a parser restated bit by bit from Figures 10 to 15 (status, fields and protected set), `flexfec_encode` and `flexfec_recover` against the fault each input injects with no byte written on a refusal, every repair packet equal to §6.2's bit string built literally, every member recovered from the others, and `flexfec_recover_window` against a peeling of the index sets of a 2-D block | 25: Figure 12's three mask widths, Figure 13's L and D over two streams, a retransmission, each header refusal (R=1 with F=1, L = 0, an empty mask, a cut mask chain, no streams, sixteen); masks over one and three streams, rows across the wrap, columns, each encode fault (a member dropped, repeated or foreign, not RTP, one byte short, L = 0); Figure 16's and Figure 7's loss patterns, a block across the wrap with repairs lost, a row without columns |
| `crd-fuzz-hesap-link` | hesap-net's link models (HESAP-NET) on any setting and word: the three loss machines and the correlated word restated as a transition table decide every packet and every word consumed, the table delay's expression restated decides every table delay, refusals restated and writing nothing, every delay between its floor and ceiling and reproducible from its seed, the slot gate's gaps within their span, medium grants in order and within their budgets, gaps and lapses | `tests/numerics/hesap-net/fuzz/corpus-link` |
| `crd-fuzz-hesap-raptorq` | hesap-net's RaptorQ code (RFC 6330, HESAP-NET) on any ESIs and object parameters: the refusal order restated from raptorq.hpp, the solver's verdict against the rank of the literal matrix A of Figure 6, its intermediate symbols against the encoder's and every encoding symbol it was given, the FEC payload ID and object transmission information against a reader restated bit by bit, §4.3's derivation against its definition, and an object's blocks and sub-symbols covering each octet exactly once with zero padding. |
| `crd-fuzz-hesap-red` | hesap-net's redundant audio data (RFC 2198, HESAP-NET): `red_parse` against a reader restated bit by bit from §3's two header figures (status, count, fields, timestamps and the bytes each block views; a refusal leaves the blocks as they were), `red_encode` against §3's field widths and its time-interval rule with no byte written on a refusal, every written payload equal to one built bit by bit and read back to its blocks | 20: §7's header, a primary alone, three levels, lengths that leave an empty primary, each parse refusal (an empty payload, a header cut at one and three bytes, a chain that never ends, more blocks than room, lengths past the payload); §7's packet, a primary alone, the widest fields, each write refusal (a type, an offset and a length past their fields, offset 0, an offset twice, a primary type past its field, one byte short) |
| `crd-fuzz-hesap-estimators` | hesap-net's network estimators (HESAP-NET) on any feedback and clock a peer can send: `RttEstimator` against the clamped samples' bounds with a refused sample changing nothing, `InterarrivalJitter` against RFC 3550 A.8's update restated in 64 bits, `PacketGroupTracker`'s output written only when it returns true and within restated bounds, `TrendlineFilter`, `KalmanDelayFilter` and `DelayOveruseDetector` always finite with the threshold in [6, 600] and refusals changing nothing, `WindowedMax` against a brute-force history, `AckedRateEstimator` always finite | 12: one op of each kind at small values and at the extremes |
| `crd-fuzz-hesap-quantize` | hesap-net's bounded floats and smallest-three quaternions (HESAP-NET) on any format, value and word a peer can send: formats against their restated conditions with no write on a refusal, every kernel level bit for bit, codes within half a step, every decoded word finite and unit length and refused exactly as restated from the word, and an encoder's word never refused. |
| `crd-fuzz-hesap-motion` | hesap-net's interpolation, extrapolation, rewind history, dead reckoning and projective velocity blending (HESAP-NET) on any snapshot, time and limit: every kernel level bit for bit against the expressions restated in the target, nothing written past a batch, nlerp finite and unit for any input, slerp within reach of the true arc on unit inputs, the history's slots, samples and refusals against a linear search over a plain list of stamps; IEEE 1278.1's nine algorithms with any columns at every level, in place, nothing written past a batch or into a column an algorithm does not move, and with columns a real entity holds following the true rotation and its integral; projective blending against its restated expressions. |
| `crd-fuzz-hesap-clock` | hesap-net's clock filters (HESAP-NET) on any timestamps a peer can write: `on_wire_sample` against the exchange restated modulo 2^64 with no write on a refusal, `ClockFilter`'s order, stages, chosen sample and event against their definitions with a refused update changing no bit, `ClockDriftFilter` and `ClockTracker` finite with a refusal changing nothing, `MediaClockMap`'s estimate from its own line, `CaptureClockAligner` never in the future and monotonic while the system clock is. |

Corpora are raw bytes under `tests/<owner>/fuzz/corpus/<target>/`, `-text -diff -merge` in `.gitattributes` so no
normalization ever rewrites the bytes that reproduced a finding; seeds carry a content-hash name from
`<replay> --seed <dir>`, merged units carry libFuzzer's SHA-1 name. The harness directory `tests/support/fuzz` is a
registered test directory owned by the `ceir`, `ceir-cook`, `ceridc`, `kir`, `perf`, `memory` and `resources` rows,
so a fuzz executable's link set is the target's own module plus core, containers and memory. Every `crd_fuzz_target()` has exactly one
`fuzz.py` entry naming its corpus and build-relative executable, and no entry is stale (`test-repository-tools.py`,
DIAG.10a). The replay driver declares itself headless before the first input (`set_assert_headless`, and on Windows
no abort dialog), so an engine assertion inside a target ends the replay with its evidence instead of waiting on a
modal dialog until the CTest timeout.

The stateful target `crd-fuzz-memory-allocators` (DIAG.10a) is the same harness with a different input shape: the
first byte picks an allocator and its configuration, the rest is an operation stream replayed against the allocator
and an independent reference model. Only contract-valid operations reach a raw allocator (allocation through
`try_allocate`, frees of live blocks, a TLSF reallocate only when a probe shows its fallback cannot reach the fatal
out-of-memory path, strictly increasing ring epochs within K in flight), so a failure is an allocator defect. The
malformed requests the contract covers (zero, near-`SIZE_MAX` and past-the-arena sizes, top-bit and
non-power-of-two alignments) are generated on purpose and must be refused without change, and the checked layers
take the hostile operations they promise to diagnose (double, interior and foreign frees, redzone overruns and
writes into quarantine for the `DiagnosticAllocator`; stale, double-erased and forged handles for the `SlotMap`).
The oracles are the model's: no overlap, alignment, ownership, intact payloads, exact success and failure where the
allocator's capacity model is exact, `validate_structure()`, statistics that return to zero, an offset span that
coalesces back to one region, a parent that gets every byte back, and the same outcome trace on a second run. The
parent is page aligned so placement does not depend on where the system allocator put the arena. The address-space
allocators (2026-10-11) reserve at most 16 MiB per input and release it with the allocator: the
`VirtualMemoryAllocator` is held to an exact model of its bump offset and committed high-water mark (with recommitted
bytes reading zero and the committed tail poisoned under ASan), and the `StreamingAllocator` runs under a residency
policy the input scripts, with every category's charge tracked exactly through `used()`, the budget held on every
success, the retry bound and no policy consultation for a request nothing can serve. Kinds are appended to the enum
so that every committed input keeps the kind its first byte selected.

The evidence-reader targets (DIAG.10a) hold each reader to what its writer can produce. `crd-fuzz-perf-flight`
restates, independently of the decoder, what `FlightRecorder::trigger()`, `build_capture()` and `finish()` guarantee
(the trigger and every suppressed kind armed by the recorded policy, one contiguous run of events across the
trigger, the end reason and truncation derived from the post window, a capture without the ring fatal and empty), and
an accepted record must re-encode to exactly its own bytes. `crd-fuzz-perf-capture` computes the CPROF layout from
the header itself and requires every `CaptureView` accessor to answer from exactly where that layout says, with every
name terminated inside its own field or the blob's strings. `crd-fuzz-perf-assets` offers every input to all three
policy readers: a refusal names its reason and leaves the output untouched, and an accepted policy or plan writes a
canonical document or text that reads back to the same value. Their seed functions check each seed's verdict while
writing it (`VerdictSink`), so a regenerated corpus cannot carry an honest seed the reader now refuses, or a forged
one it now accepts. The perf corpora live in `tests/foundation/perf/fuzz/corpus/<reader>/` (the bundle corpus moved
to `corpus/bundle/`). `crd-fuzz-ceir-record` decodes each input as given and again with its header's payload size and
checksum recomputed, so a mutated payload reaches the payload reader instead of stopping at the checksum. It restates
what `replay.record`, `record_host_run` and the device recorder write (each executor's own fields and errors, never a
cancelled run, each input's state derived from its need, every kept read of an input the program needs, each kernel's
text with its hash), requires an accepted record to re-encode to its own bytes, to name the missing inputs its states
name and to show no divergence from a trace that repeats it, and bounds the decoder's allocations to eight times the
input plus 64 KiB, so a count is checked against the bytes left before anything is reserved for it. It carries the
program blob without reading it: the CRDR reader is `crd-fuzz-ceir-binary`'s. `crd-fuzz-ceridc-diag` reads an input
as a host line (the grant, and an optional file root that does not exist) and request lines: JSON-RPC lines fed to
one server as ceridc's stdio loop would (an initialize line selects revision 2025-11-25 for the input's later lines),
the diag tool's `arguments` spliced into a fixed 2026-07-28 envelope (`@`), and native requests of raw bytes (`!`). A line that could reach one of ceridc's file verbs is not fed: they read and write paths
the input names. An independent iterative RFC 8259 validator (UTF-8 decoded to code points) must agree with the parser
within its documented limits, every reply must be one line of valid UTF-8 JSON that follows the JSON-RPC rules with
the request's id echoed exactly, the diag tool's report must equal the native answer for the page it names byte for
byte, the service must refuse in its documented order without running a handler, and every snapshot is walked to its
end with repeatable cursors. Its seed function lists the seeds the parser refuses and checks each verdict as it writes
it.

`crd-fuzz-resources-reload` (DIAG.10a) drives the cooked-pack ingress through real files: the production entry points
take paths, so each pack an input names is written under the platform temp directory with a per-process name and
removed after the input. An even first byte makes the rest one pack file (read by `crdr_read` directly, then mounted,
its ids loaded, reloaded, evicted, loaded again and unmounted); an odd one makes it a script over two pack files and
one manager. The CRDR layout and the pack rules are restated in the harness (zstd decodes the compressed chunks), and
a model of the manager (live table by newest mount, loaded blocks with state, generation, bytes and resident size,
pins, reload events) is compared with `find_entry`, `entry_count`, `mount_count`, `current_memory_use` and every held
handle after each operation. Two test loaders (one with a placeholder) copy their artifact and refuse one leading byte,
and each requires that only its own type's payloads reach it. Inputs whose compressed chunks claim more than 16 MiB in
all are skipped as out of scope; `poll_hot_reload` is not driven (one-second mtimes), both reload paths sharing
`do_reload_mount`. On Windows Debug builds the replay driver turns a Debug CRT report (heap corruption found at a free)
into an immediate failure instead of a modal dialog.

`crd-fuzz-ceir-differential` (DIAG.10a) compares the two CEIR execution tiers. Every input decodes to a program, never
to a parse failure: a header (one to four functions with their parameter and result counts, a step budget of 64 to
131,072, a host input script or no source at all, the entry arguments) and then each function's body as a stream of op
choices over arith, core control flow and the three cell kinds, `func.call` (recursion included), async and task
tokens, data-parallel bodies and the input ops. The generator stays inside the module-validity set a cook enforces
(the structure, domain, token and borrow verifiers and the recursion policy), so a refused program is out of contract
and not run. Both tiers run each program at the same budget with the same scripted input: they must agree on the error
class (the documented ExecError and RunError pairs), the values, every cell, every map output on success and, on an
error, the stable id of the op it blames. A compile refusal is accepted only for the documented tier difference (a
parallel body that holds a cell or reads host input, refused whole); a reference error with no compiled counterpart is
a finding. The plan run twice, a fresh reference session, and the printed-and-parsed and serialized-and-loaded twins
must give the same outcome. On a failure the program's text is printed with the two outcomes.

`scripts/fuzz.py` never builds: `run` fuzzes a working copy of the committed corpus for `-max_total_time` seconds
with the limits above, artifacts (`crash-*`, `timeout-*`, `oom-*`, `leak-*`) under scratch, and writes the final
statistics as a board (executions, executions per second, new units, peak RSS, artifacts; a reused scratch keeps
earlier findings, which the board names only when this run wrote them again); `merge` adds to the
committed corpus only the units that add coverage (`-merge=1`); `minimize` shrinks an artifact
(`-minimize_crash=1`); `adopt` copies a minimized artifact into the committed corpus under its SHA-1 name; `replay`
runs the replay executable verbosely. Findings policy: a crasher fixed in the loader is adopted as a passing
regression; one whose repair is out of scope is committed as raw bytes under the owner's `fuzz/pending/<target>/`
directory, which no replay test reads, and is listed with its symptom and owner in the session record, so it is
never a committed red test and never a silent skip. The harness ends every failure with an abort that carries no
Debug CRT dialog on Windows, so a failing replay CTest fails instead of hanging the lane (proved on MSVC 19.51:
the round-four CKIR text inputs, before their fix, ended `crd-fuzz-ckir-text.exe` with exit code 3 in under a
second); the CEIR text and
CKIR text targets print the parser's reason and offset before the abort when the printer's own output is rejected.

The fuzz configuration is `linux-clang-fuzz` (diagnostic tier, in no CI tier; hosted only by the dispatched
[host check](#host-separation-diag10a)): clang++ with ASan and UBSan,
`CRD_ENABLE_FUZZER` adding `-fsanitize=fuzzer-no-link` to the tree, and `CRD_WARNINGS_AS_ERRORS=OFF`, because
clang++ 18 under the GCC warning set fails 139 translation units of the ceir and kir closure (323
`-Wsign-conversion` sites, almost all in `ckir_kernel_eval.hpp`, `ckir_glsl.hpp`, `ckir_hlsl.hpp` and `ckir_rt.hpp`;
`-Wnested-anon-types` on the `String` small-buffer union in every unit; 13 `-Wdouble-promotion`). The diagnostic
build shows those warnings instead of failing on them, the way `win-tidy-local` previews clang-tidy; every qualified
configuration keeps warnings as errors, and the default graphs are unchanged (the flag is the same string). Since
2026-10-17 the same switch puts `-Werror` on GCC and Clang link lines too: GCC judges link-time optimization's
diagnostics by the link line alone, so a compile line's `-Werror` never reached them, and `lto-wrapper`'s own notices
ignore `-Werror` either way ([session](../sessions/2026-10-17-diag-11b-warning-gates.md)). The GCC link line also
names the compile set's warnings that the link-time compiler reads: `lto-wrapper` passes on no C-family option, `-Wall`
included, so `-Wall`'s members (`-Warray-bounds`, `-Wmaybe-uninitialized`, ...) are named one by one, and GLFW builds
without link-time optimization so that its code never meets them
([session](../sessions/2026-10-17-diag-11b-link-time-warning-set.md)); `-Wnull-dereference` is among them since its
link-time locations were repaired, 80 on linux-gcc-shipping and 42 more with asserts compiled in
([session](../sessions/2026-10-17-diag-11b-link-time-null-dereference.md)). An unchecked lookup that may miss is such a
location waiting for an inlining change, so `crd-ceir-invariants` (I7) holds every `func::func_body_block` caller to a
nullptr test; callers with a body take `func::func_body`
([session](../sessions/2026-10-17-diag-11b-func-body-lookups.md)). A link stops at its first failing
link-time partition, so `-flto-partition=one` on a failed link command lists all of a program's link-time diagnostics
at once (diagnostic use only). Third-party sources keep their own flags and are held to no warning as well: zstd's two
clang-cl `-Wunused-function` take registered patches and its configure warnings are answered, except one deprecation
CMake before 4.0 prints until the zstd 1.5.7 pin ([pinned inputs](pinned-inputs.md#patches-on-a-build-owned-copy)). clang++ on Linux is not a qualified compiler of
this tree; that audit belongs to the platform/preset matrix row (REPO.DEV.11). clang-cl on Windows was tried first and
dropped: its ASan has no debug-CRT runtime and, linked through lld-link, its objects and runtime disagree on the runtime
library.

## What the first bounded runs found

Four rounds on the `linux-clang-fuzz` build (clang 18, ASan+UBSan, two workers per target, 600 s then 300 s,
300 s and 120 s per target; every round ended early on the first artifacts of a worker):

| Round | ceir-text | ceir-binary | ckir-text | ckir-binary |
|---|---|---|---|---|
| 1 | 1.29 M executions at 64 k/s, 2 artifacts in 26 s | 13 k, 2 artifacts in 0.3 s | 182 k at 182 k/s, 1 artifact in 1.3 s | (interrupted by the rebuild) |
| 2 | 1.56 M at 55 k/s, 2 in 36 s | 676 k at 78 k/s, 2 in 8 s | 1.58 M at 124 k/s, 2 in 12 s | 668, 2 in 0.1 s |
| 3 | 1.98 M at 61 k/s, 2 in 46 s | 108 k, 2 in 1.7 s | 6.83 M at 96 k/s, 1 in 288 s | 2.90 M at 143 k/s, 2 in 41 s |
| 4 | 153 k, 2 in 3 s | 1.41 M at 94 k/s, 2 in 23 s | 7.96 M at 77 k/s, 2 in 111 s | 440 k at 168 k/s, 2 in 3 s |

Rounds 1 to 3 produced 21 artifacts, all adopted into the corpora. Twelve were root-caused to nine loader defects,
each fixed as a rejection: a signed overflow in the CEIR exponent parser; an operation name longer than the interning buffer (an assert
instead of a rejection, guarded in both CEIR loaders through `Context::kMaxOpNameBytes`); an empty attribute
name in the CEIR binary decoder; an attribute dict whose first name is quoted, which the printer emits and the
parser's brace lookahead took for a region; region nesting without a depth cap in both CEIR loaders; a CKIR
text entry whose `n_out` disagreed with its `[[out]]` blocks; a CKIR blob whose input count exceeded its node
array; the CKIR blob reader validating none of the bounds the text reader validates (now the same checks); and
unbounded integer accumulation in the CKIR token and node-ref readers. The other nine have no established cause.
Six never reproduced in isolation (single-input and whole-corpus replays on both hosts): three round-two CEIR
reports (an unknown-crash in the text parser's keyword comparison, a stack-buffer-underflow and a `string_view`
with insufficient space in the binary decoder) and three round-three CKIR reports (a misaligned `string_view` in
the text reader, a misaligned cursor and a stack-buffer-underflow in the blob reader), the CKIR three found by a
build that already carried every CKIR fix. Three round-three CEIR reports (an unknown-crash under `parse_type`, a
span offset overflow and a jump to address zero in the binary decoder) replay clean after the region caps and
the lookahead fix without a traced cause. An adopted artifact with an unknown cause is still a regression input:
it replays clean on both hosts and a future failure names the input. Round 4's eight artifacts were resolved by
the [REPO.DEV.11 audit](../sessions/2026-09-14-infrastructure-audit.md): the two CKIR text reports were
deterministic writer/reader identity gaps and are fixed (the writer elided a `-0.0` constant through a `!= 0.0`
test, so the sign bit died in the text form while the blob kept it; a `[[out]]` block without `node =` left a
stage output at `-1`, which the reader accepted and the writer emitted as `"n-1"`; both the text and the blob
reader now reject an output that names no node). The other six (two CEIR text reports shaped like stack
overflows, two CEIR binary dangling-view reports, two CKIR blob cursor reports) did not reproduce on a fresh
clang 18 ASan+UBSan build, alone or over 5,000 in-process repetitions each, and were adopted as corpus inputs.
The policy that follows: a libFuzzer-process finding that does not replay in isolation is adopted, counted and
recorded with its repetition evidence; a recurrence with a reproducing artifact gets an owner through the
ordinary route. No `fuzz/pending/` directory remains. Exception (2026-10-11,
[container-overflow control](../sessions/2026-10-11-diag-10a-container-overflow-control.md)): the development host
now faults in sanitized and unsanitized builds, with and without libFuzzer and independent of the input (a
committed seed failed in-process, and an input handled correctly thousands of times produced a wrong result once).
Until a run on another machine settles whether the host is the cause, findings from this host that do not replay
alone are counted and recorded in the session, not adopted: such an input carries no regression value. The
[host check](#host-separation-diag10a) settled it on 2026-10-14
([session](../sessions/2026-10-14-diag-10a-host-check.md)): about 140.6 M executions of the same targets in two runs
on five hosted CPU models gave no event that does not replay, so the rule stands for findings from this development
host.

Every adopted artifact replays clean on MSVC 19.51 (`win-debug`), clang 18 (ASan+UBSan) and GCC 13.3
(`linux-gcc-asan`, UBSan without recovery, `-Werror`), and the CEIR and CKIR unit suites (534 and 311 cases) pass
with the fixes on all three; one whole-suite GCC ASan run ended in an unrelated evaluator `SEGV` that four further
runs did not repeat, recorded in the session record and closed by the
[REPO.DEV.11 audit](../sessions/2026-09-14-infrastructure-audit.md) as not reproduced (one observation, five
clean whole-suite runs, no defect established; the hosted ASan lane runs every case in its own process).

## Host separation (DIAG.10a)

The exception above waits on one question: is the development host the cause? The
[host check](../../.github/workflows/host-check.yml) answers it on hosted machines. It is a workflow of its own,
triggered only by `workflow_dispatch`, so it never joins a CI tier ([CI tiers](ci-tiers.md)); `check-ci-tiers.py`
holds every workflow file besides `ci.yml` to that and `check-pins.py` holds it to the same pinned inputs. Each of
its seven legs (`ubuntu-24.04`) configures `linux-clang-fuzz`, builds one libFuzzer target and runs
[fuzz-host-check.py](../../scripts/fuzz-host-check.py): the rounds of the
[unknown-origin session](../sessions/2026-10-11-diag-10a-unknown-origin-artifacts.md) (three of ceir-binary, one
of ceir-text, three of ckir-binary, 540 s each, one process, a fixed seed per leg), each
under the default ASan options and then under the container-overflow control (`detect_container_overflow=0`) for
the same time, plus three 10,000-repetition replays of the two adopted units `cd301be8` (ceir-text) and `ace35b97`
(ckir-binary) per arm. A round fuzzes a scratch copy of the committed corpus, which is only read, and restarts with
the next seed after an event until its time is spent. An event is an artifact, a nonzero exit, or a sanitizer,
assertion or libFuzzer report in a log; the step summary names the CPU from `/proc/cpuinfo`, the executions of every
process and the stack head of every report, the logs and crash files are uploaded, and a leg with an event fails.

Two more legs (JOBS-HARD.3, [session](../sessions/2026-10-25-jobs-fiber-pool-contention.md)) carry the same question
to ThreadSanitizer. On this host the `linux-clang-tsan` run of the jobs streaming case ("fiber keep: while tiny jobs
stream ...") faults about one run in ten with values no store wrote (a `{pid, uid}` word, instruction bytes, a null
from a `unique_ptr` that held its array), and so does
[a Cerid-free control](../../scripts/tsan_host_control.cpp) of the same shape: in its fiber mode,
and in its plain mode, which runs no fiber at all; the same control unsanitized was clean. Each TSan leg configures
`linux-clang-tsan`, builds `crd-jobs-tests` and runs [tsan-host-check.py](../../scripts/tsan-host-check.py):
`tsan_rounds` rounds (150 by default, so the two legs run the jobs case 300 times) of four interleaved arms (the
jobs case under the test preset's `TSAN_OPTIONS`, the control on fibers, on plain threads and unsanitized), every
process pinned to two CPUs beside a busy loop pinned to each. An event is a nonzero exit, a timeout, a missing pass
line, or a ThreadSanitizer report or
`=== WRONG` line in a log; the summary names the CPU and tallies runs, events and seconds per arm, failing logs are
uploaded, and a leg with an event fails. The arms separate the causes: a hosted event in the jobs arm beside a clean
control is a jobs defect, an event in the plain control anywhere is that machine's, and clean legs where this host
faults put the fault on this host.

The `experiment` input picks the legs (`all`, `fuzz`, `tsan`, `arm64-sanitizer-cost`, `x64-asan-stack-alignment`,
`vulkan-sdk-installer`). The ARM64 sanitizer-cost legs
(SIMD-DISPATCH, [session](../sessions/2026-10-29-simd-dispatch-arm64-asan-cost.md)) ask why linux-gcc-arm64-asan runs
cases up to a thousand times slower than its debug lane, where linux-clang-arm64-asan and x64 GCC ASan stay within
tens. One leg per ARM64 ASan preset (`ubuntu-24.04-arm`) builds the preset and a copy without UBSan, then times five
cases through ctest in two interleaved rounds under the defaults, `detect_stack_use_after_return=0`,
`malloc_context_size=0`, `quarantine_size_mb=0`, `detect_leaks=0` and ASan alone; the summary and the uploaded table
give the seconds of every arm, and the leg records one `verbosity=1` start of the runtime.
The x64 stack-alignment leg runs [TP-7](../third-party-defects.md#tp-7)'s Cerid-free control under the pinned
Intel SDE with every compiler of the image, with and without GCC's width caps; it fails when a capped build faults
or the image's default `g++` runs clean without them, the trigger that retires the caps.

The Vulkan SDK installer legs (REPO.3, [session](../sessions/2026-11-01-repo-3-vulkan-installer-options.md)) ask
which options of the Qt Installer Framework remove the two findings of the cold Windows lane's pinned installer run,
Qt's `QFont::setPixelSize: Pixel size <= 0 (-1)` warning and its fetch of update metadata from LunarG's remote
repository, without changing what it installs. The framework has no offline switch. One fresh `windows-2025`
machine per arm runs [vulkan-sdk-installer-check.py](../../scripts/vulkan-sdk-installer-check.py) `run`: it fetches
the pinned installer through the registry and runs it with the production arguments plus the arm's options (none
twice, `--verbose`, `--platform minimal` or `offscreen`, `--set-temp-repository` naming an empty local directory or
a local repository whose `Updates.xml` lists no package, and each platform with the latter), and records the exit, every
line of either finding, the hosts the output names, the framework's metadata cache before and after (with the
repository each cached entry came from), and the path, size and SHA-256 of every installed file. The judge job
compares each arm's tree with the baseline's, leaving out the paths on which the two baseline runs disagree, reads
both findings again from each arm's log (so recorded evidence is judged by the current patterns), and names an arm
that exits 0, prints neither finding and installs the same tree; it fails only when a baseline left no complete
install. The remote finding is the framework's `Retrieving meta information from remote repository` line: its
`Fetching latest update information` line is printed for a local repository too. Run 41298875777
([verdict](../sessions/2026-11-01-repo-3-installer-verdict.md)): the local repository whose `Updates.xml` lists no
package removes the remote fetch with the same tree and is now the production argument; no platform option touches
the `QFont` line. The emitter leg ([census](../sessions/2026-11-01-repo-3-qfont-emitter.md)) runs `trace`: the
production install under `cdb` with a breakpoint on the installer's `QFont::setPixelSize` (by RVA from the image's
entry point; the census holds only for the pinned installer's digest and `trace` refuses another), logging the size
and twelve frames of every call; each call's caller is named from the census of the function's 15 direct callers,
and the leg passes only when the install completed and a call passed a size of zero or less.

## The sanitizer audit

**UBSan recovered.** `-fsanitize=undefined` alone prints a report and continues, so the Linux ASan+UBSan lane could
report undefined behaviour and stay green. The hosted `linux-gcc-asan` job log of run 34780682504 (15,918 lines)
carries no `runtime error:` line, so the lane now compiles and links with `-fno-sanitize-recover=undefined` and a
report fails the run. In the same policy an instrument the toolchain cannot provide fails the configure instead of
warning: `CRD_ENABLE_UBSAN` on MSVC, `CRD_ENABLE_TSAN` on MSVC or together with ASan, `CRD_ENABLE_FUZZER` without
clang.

**ASan and the fiber stacks.** The scheduler switches stacks in hand-written assembly at three sites (a fiber
parking in `counter_wait`, a fiber completing in `job_fiber_trampoline`, the scheduler dispatching or resuming a
fiber in `run_job_in_fiber`). `sanitizer_fibers.hpp` gives each site `__sanitizer_start_switch_fiber` before and
`__sanitizer_finish_switch_fiber` after the switch and the trampoline's first instruction a finish call that
restores the fake stack the fiber saved when its previous job completed (entering with none leaked one fake stack
a job until 2026-10-24, [DIAG.11b](../sessions/2026-10-24-diag-11b-sanitizer-cost.md)), so ASan's
notion of the current stack, its bounds and the fake stack of `detect_stack_use_after_return` follow the fiber; the
scheduler side keeps its state in the dispatching frame, the fiber keeps its own (the fake stack it left, the
scheduler stack it returns to) in the `Fiber` record because it may resume on another thread. The helpers are empty
unless the compiler defines the sanitizer, and `-DCRD_JOBS_SANITIZER_FIBERS=0` reproduces the unannotated model for
the audit. Measured on `crd-jobs-tests` and a scratch probe, annotated and unannotated, under `win-asan`
(MSVC 19.51) and `linux-gcc-asan` (GCC 13.3, WSL2):

| Probe | MSVC annotated / unannotated | GCC annotated / unannotated |
|---|---|---|
| `crd-jobs-tests` | 22 of 22 / 22 of 22 | 22 of 22 / 22 of 22 |
| one byte past a stack buffer on a fiber (through a pointer) | stack-buffer-overflow naming the fiber frame / same | same / same (an array subscript is caught by UBSan's bounds check first) |
| use after return on a fiber (`detect_stack_use_after_return=1`) | no report / no report | stack-use-after-return / same |
| 2,000+ nested waits across four threads | clean / clean | clean / clean |

The probes show no behavioural difference between the annotated and the unannotated model on these two compilers;
the annotations stay because they are the documented contract that makes the bounds and the fake-stack ownership
explicit per fiber, and they cost nothing uninstrumented. MSVC's use-after-return detection needs its own compile
switch (`/fsanitize-address-use-after-return`), which no preset sets: on `win-asan` that class is not exercised,
and the table says so rather than claiming it. GCC on AArch64 leaves it out too (`--param=asan-use-after-return=0`):
GCC 13's fake-stack frames there cost about 30 times Clang's on the same runtime and runner, a Cerid-free control
shows it ([TP-6](../third-party-defects.md#tp-6)), and with them `linux-gcc-arm64-asan` could not finish in the 6 h
job cap. `linux-clang-arm64-asan` and `linux-gcc-asan` keep the class, so it stays covered on both architectures.

**ThreadSanitizer and the fiber model.** `CRD_ENABLE_TSAN` (GCC or clang, exclusive with ASan) builds with
`-fsanitize=thread`; every fiber stack gets a TSan context at pool init (`__tsan_create_fiber`), every switch
announces its target immediately before it (`__tsan_switch_to_fiber`, flags 0: the switch is a synchronization
point on the switching thread, cross-thread ordering being the work queues' acquire/release, which the switch
composes with), a completing fiber's trampoline pops its own frame from the fiber's shadow stack before it
leaves (its context is rebuilt for the next job, so it never returns), the contexts are destroyed at shutdown, and
the work-stealing deque's standalone seq_cst fence, which GCC's TSan cannot model (`-Wtsan`), becomes a seq_cst
read-modify-write on the same location under the instrument only. On this WSL2 kernel TSan aborts with "unexpected
memory mapping" unless address-space randomization is off for the process (`setarch x86_64 -R`, no root needed; the
sysctl is unreadable there).

The recorded probe: the switch path itself produced no report in 22 tests and a 2,048-wait probe. This does not prove
the model preserves all required race visibility; DIAG.1b supplies that qualification. The recorded reports are
outside the switch path: two lock-free free-lists read the `next_free` link of the head element outside the
compare-exchange that validates it (`fiber_pool.cpp` `release_to`:296 against `acquire_from`:239; `counter.cpp`
`CounterPool::acquire`:66 and :75 against its release), a plain read racing a plain write, benign in practice by the
code's own argument and a data race by the language. Five of the 22 jobs tests fail on those reports (16 in the
suite, 68 in the probe, no other site). The immediate repair now belongs to [DIAG.1a](../ROADMAP.md#slice-diag.1a);
atomic links are a candidate requiring a memory-order/reclamation proof. A clean suite reports no detected race on
its instrumented executions; it cannot prove absence of races or ordering bugs.

**The hosted lane (DIAG.1b, 2026-10-05).** `linux-clang-tsan` runs in the complete tier, four tests at a time, with
`halt_on_error=1` and a committed suppression file
([tsan-suppressions.txt](../../tests/support/diag/tsan-suppressions.txt)). The file may name only third-party
libraries that are not built with `-fsanitize=thread`: today Mesa lavapipe and the Khronos validation layer, whose
internal ordering TSan cannot see. A report in Cerid code is fixed, never suppressed. The first full run found and
fixed two engine races (the diagnostic snapshots' fiber and counter fields; the frame-history seqlock payload), a
32-bit fiber tag that could be 0, a test that skipped the pool's sanitizer switch protocol, and specimens that assumed
ASan; see the [inventory and triage](../sessions/2026-10-05-clang-werror-and-tsan-inventory.md). The WSL2
address-space note above did not reproduce with clang 18 on the 2026-10 reference host.

## Seeded detector failures (DIAG.10b)

[diag-seeded-faults.py](../../scripts/diag-seeded-faults.py) breaks one instrument at a time and requires the
diagnostic suite to notice. The seeds live in [seeded-faults.json](../../tests/support/diag/seeded-faults.json): each
is a set of exact source edits (each seeded text must occur once in the tree) or extra environment for the test run,
with the lane facts it applies to (`os`, `asan`, `tsan`, `msvc`, ... read from the build tree) and the exact CTest
names that must fail, each with the diagnostic text its failure must print. Per seed the runner builds the targets and
runs the tests unseeded (all must pass), applies the seed, rebuilds and reruns with JUnit output, then restores the
files byte for byte with a newer timestamp, rebuilds and reruns. The verdict comes from the JUnit and the test
output, never from CTest's exit status: a seed that leaves a test passing SURVIVED, one whose test fails without the
named text is a WRONG_FAILURE, a mutant that does not compile is BUILD_FAILED and never a detection, and no test is
rerun. After the restore git must report each edited file exactly as before (an uncommitted change survives). Every
seed names the targets holding its detecting tests, so even an environment seed tests the current tree. Contract and
the classes it must cover: [DIAG.10b](runtime-diagnostics.md#diag-10b). Run it inside the MSVC environment on
Windows, one lane at a time: a seed edits the shared checkout, so a second lane building from it would compile the
seed.

The seeds run hosted (2026-10-11): the `Seeded detector failures` step of the `windows` and `linux-gcc` jobs runs the
runner on win-debug, win-asan, linux-gcc-debug, linux-gcc-asan and linux-clang-tsan, the lanes the seeds were
qualified on, after the lane's build and tests pass, in the lane's own tier. It passes `--preset` so every run starts
from the test preset's environment (linux-clang-tsan's TSan options and suppressions), and writes the report and each
phase's build log, CTest log and JUnit under `build/<preset>/evidence/diag-seeded-faults`, which the lane's evidence
artifact carries. The runner puts the lane's own `Testing/Temporary/Last*.log` back as the lane's test run left them
and removes any its runs created, so the uploaded record is the lane's, not a `LastTestsFailed.log` naming the tests a
seed broke. The step is a multi-preset entry of `preset_steps` in the [tier mapping](ci-tiers.md), and the
verdict tests derive the hosted lanes' facts from those presets, so a seed no hosted lane runs, or a hosted lane no
seed applies to, fails them. Applicable seeds: 3 on win-debug and linux-gcc-debug, 5 on win-asan, 6 on
linux-gcc-asan and linux-clang-tsan.

The committed sanitizer suppression files may name only third-party shared libraries (`lib*.so*`): the verdict tests
(`test-diag-seeded-faults.py`, the `crd-diag-seeded-faults` CTest and a repository CI step) refuse any other entry,
so the rule above about Cerid code is enforced, not only stated. A seed may add such an entry for its own run.

## Diagnostic coverage and the nonreproduced-failure register (DIAG.10b)

The coverage configuration is `linux-clang-coverage` (diagnostic tier, never hosted): clang++ with
`CRD_ENABLE_COVERAGE` (`-fprofile-instr-generate -fcoverage-mapping` on every TU, exclusive with the sanitizers and
libFuzzer). [diag-coverage.py](../../scripts/diag-coverage.py) runs the test set named by
[coverage-scope.json](../../tests/support/diag/coverage-scope.json) once (selected CTests plus the Catch2 programs
CTest does not register), requires every selected test to pass in the JUnit, merges the raw profiles and reads
llvm-cov's export of the scope files. It reports regions, branches, functions and lines per scope file with the
denominator stated (scope files, files this lane instrumented, files excluded by path). Percentages are never a
gate. The gate is the outcome sets: every value of a registered result enum must be produced at a site that ran,
and every transition site of a registered state machine must have run, unless an exclusion owns it with a class
(`dying-child`, `windows-only`, `other-configuration`, `resource-failure`, `no-producer`, `default-value`,
`owned-gap`) and a reason. An exclusion that names something that now runs, no longer exists, or calls a site
compiled on this lane `windows-only` is stale and fails the run. A child that dies writes no profile, so paths
reached only inside dying specimens are owned as `dying-child`, never counted as untested. Closing an owned gap
removes its exclusion in the same change; on 2026-10-11 twelve were closed by cases
([session](../sessions/2026-10-11-diag-10b-owned-gaps-closed.md)) and the last three through test-only engine seams
([session](../sessions/2026-10-11-diag-10b-engine-seams.md)), so no `owned-gap` exclusion is left.
The harness's own Timeout verdict is exercised with the CPU-hotspot specimen under a 200 ms bound.

[nonreproduced-failures.json](../../tests/support/diag/nonreproduced-failures.json) registers flaky and
nonreproduced failures: first seen, lane, target, signature, reruns, status (`nonreproduced`, `explained`,
`toolchain`, `host`), explanation and session. It is evidence only. The verdict tests (`test-diag-coverage.py`, the
`crd-diag-coverage` CTest and a repository CI step) hold every entry complete and fail if any build file, CI step or
script reads it, so it can never select, skip or retry a test.

<a id="source-routes"></a>
## Source routes (DIAG.11b)

A source that no target compiles is a test that never runs or code that rots unseen, and nothing else notices: the
CEIR-17a scene-dialect cases (`tests/execution/ceir/test_scene.cpp`) were in no target's source list until 2026-10-17.
A lexical CMake rule cannot see this (159 test sources are globbed, six are named by another directory's CMakeLists),
so `check-source-routes.py --build <tree>` (the `crd-check-source-routes` CTest on every full configuration with a
compile database) reads what the configured tree compiles, its `compile_commands.json`, and judges every first-party
C-family source under `engine/`, `tests/`, `tools/`, `runtime/`, `sandbox/`, `bench/` and `scripts/` (tracked, and
untracked unless ignored) against [scripts/source-routes.json](../../scripts/source-routes.json):

- a source that no entry names is compiled by a target of the tree;
- `built_when` states when the tree compiles an entry's sources, as conditions the guard reads from the tree: the
  platform (`CMAKE_SYSTEM_NAME`: the D3D12 backends, the fiber-switch assembly, Metal), an option in `CMakeCache.txt`
  (absent reads OFF; a CMake file must define the name: `CRD_BUILD_BENCHMARKS`, the peer-bench options), or a target
  that compiles a source in the tree (the CUDA, WebGPU and HIP backends, configured where their toolkit is found, so
  one manifest also holds on the hosted runners, which have none). A covered source is compiled exactly when its
  conditions hold:
  a source that falls out of its target fails, and so does an entry that no longer describes the tree;
- `built_by` names the file that builds a source outside the tree's targets (a peer script, the installed-package
  consumer's own project, the GenMC model's checker); it must name the source, and the tree must not compile it;
- `included_by` names the source that includes a generated kernel;
- `unrouted` names the open ROADMAP row that owns giving a peer or vendor measurement program a route or retiring it;
  it is refused in `engine/`, `tests/`, `tools/` and `sandbox/`, for a Done row, and for a source the tree compiles.

Every entry needs its reason and every pattern must match a source. The peer-free retained benchmark programs of
`runtime/examples` build under `CRD_BUILD_RETAINED_BENCHES`, which linux-gcc-debug and the presets inheriting it turn
on, so a hosted lane compiles and links each one. Teeth: dropping `test_scene.cpp` from crd-ceir-tests again fails the
CTest after a reconfigure, an untracked source in a test directory fails it without one, and fifteen mutations of the
guard's rules each fail its cases in `test-repository-tools.py`
([session](../sessions/2026-10-19-diag-11b-source-routes.md)).

## Not in scope

REPO.DEV.9 remains the bounded ingestion baseline. DIAG now owns the immediate diagnostic extensions above;
clang++ as a full Linux compiler and not-yet-existing product consumers retain their respective qualification rows.
The original templates/routes contribution work remains REPO.DEV.10. Random program generators, differentials and a
reducer for CEIR, CKIR and CHIR, with Centipede/FuzzTest and AFL++ beside libFuzzer, are
[IR-FUZZ](../ROADMAP.md#slice-ir-fuzz)'s; property-based and mutation testing are
[TEST-METH](../ROADMAP.md#slice-test-meth)'s; the perceptual golden metric is
[IMG-ORACLE](../ROADMAP.md#slice-img-oracle)'s; TLA+ models and error bounds are
[FORMAL.1](../ROADMAP.md#slice-formal.1)'s.
