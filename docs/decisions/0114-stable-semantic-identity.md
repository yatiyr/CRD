# ADR-0114 — Stable semantic identity: content-independent stable ids for ops/functions/state-slots

**Status:** **ACCEPTED** (2026-08-09, advisor-approved under the CEIR-8 gold-standard autonomous cadence) — the
D-007 **CEIR band 8 (Foundation Closure)**, slice **CEIR-8d**. Pays the ADR-0109 §6 semantic-identity IOU (only
`SourceLoc` landed at 1c; the *stable id* — item 1 — never did). Retires pointer/pre-order identity from the
persistent state-schema surface; unblocks CEIR-10a state migration, CEIR-8i transactions, semantic diff, and visual
identity.
**Phase:** D-007. Law: ADR-0109 §6; `docs/research/2026-08-09-ceir-universality-review.md` §B (row "Semantic
identity"); mission §10/§12; U-§101/U-§12.
**Tags:** `[ceir]` `[identity]` `[serialization]` `[state]` `[foundation]`

---

## 1. Context

ADR-0109 §6 fixed the semantic-identity model "before the editor": (1) **stable semantic ids** — every
function/subgraph/op carries an identity independent of textual position or graph coordinates, so a semantic diff is
position-independent; (2) source spans (`SourceLoc`); (3) layout metadata separate from semantics, keyed by the
stable id. Only (2) landed at CEIR-1c. Today ops/functions are **pointer + pre-order** identified: the binary body
encodes ops in creation order with *implicit* ids (position IS identity), and the §20 state schema in the interface
hash (`program_asset.cpp`) walks StateEdge cells in **body order** — so a private-func reorder changes the hash (a
SAFE false-incompatible, but a false one; 7a/8c named it). Every consumer that must survive an edit — state
migration, transactions, semantic diff, an editor's node identity, and 8c's per-instance location identity — has no
identity to hold onto.

## 2. Decision

### 2.1 One id space: a `StableId` on `Operation`

A `StableId` (a `u64`, `0` = invalid per the id.hpp convention) rides `Operation`. **One id space** serves every
identity consumer: a function IS a `func.func` op (its op stable id is its function identity); a state slot IS a
`StateEdge` op (its op stable id is its slot identity); a visual node's identity (editor, future) IS its op's stable
id. No separate id namespaces, no per-kind counters. The layout side-table (ADR-0109 §6 item 3) is deferred — no
consumer exists until the editor band; it will key on THIS id.

### 2.2 Assignment: module-scoped, deterministic, one-time (NOT a Context counter)

`Context::assign_stable_ids(const Module&)` — **idempotent, pre-order**: scan the module pre-order for the current
max stable id `M`; walk pre-order again assigning `++M` to every op whose id is `0`. Already-assigned ops are never
re-derived (the stability contract: an edit/reorder/rename keeps every surviving op's id). A FRESH module (all ids 0)
gets `1..N` in pre-order.

⛔ **The monotone WATERMARK (advisor pre-close, the id-reuse blocker).** A max-of-*live*-ids scan is not enough:
`erase()` tombstones + unlinks an op, so it vanishes from the pre-order walk — a later append would then draw the
**erased op's id**, and the §2.7 delete/re-add discriminator would silently pass a resurrected id (state corruption,
the exact failure identity exists to prevent). So the `Module` carries a `stable_id_watermark` = the max id EVER
assigned; assignment draws from `max(scan_max, watermark) + 1` and bumps the watermark. Identity is **monotone per
module** — a freed id is never reused, in-memory OR across a serialize/load (the watermark is serialized in STID and
restored on decode; the decoder also rejects any id `> watermark`). Blob purity holds: a fresh module's watermark is
`N`, a pure function of content.

⛔ **Why NOT a Context-global create-time counter** (the advisor's correction): `test_binary.cpp`'s second gate —
*"the blob is a pure function of module CONTENT, not of Context history"* (the same graph built in a clean vs
pre-polluted Context serializes byte-equal) — would DIE, because a Context counter makes id VALUES depend on how
many ops the Context ever created. Module-scoped pre-order assignment keeps ids a **pure function of content**, which
buys two properties free: (a) identical content → identical ids (no spurious interface-hash divergence between
independent identical builds), and (b) two machines migrating the same pre-8d blob produce the same post-8d blob.
The id field is `mutable` and assignment is memoization (logical-const): `serialize`/`interface_hash` take
`const Module&` and call `assign_stable_ids` internally, so a never-persisted op still gets a deterministic id the
moment any consumer needs one, and all consumers agree (one routine — §2.6).

### 2.3 Serialization: an additive `STID` chunk, NO `kBinaryVersion` bump

A new FourCC chunk `'STID'` = `u32` count + `u64` watermark (§2.2) + one `u64` stable id per op in body pre-order. It is
**forward-skippable** (an unknown FourCC is skipped by length — binary.cpp:419), so a pre-8d decoder ignores it and
a pre-8d blob (no STID) decodes fine (its ops get fresh pre-order ids). ⛔ NO `kBinaryVersion` bump. Decode
(build-raw-graceful-reject, the 8b scar): read the STID list, assign to ops in the same pre-order the decoder
rebuilds them; **reject** a `0` id, a duplicate id, or a count ≠ the op count — never assert on hostile input; a
single-byte-corruption fuzz sweep lands with the slice. Deserialize needs no counter restore — `assign_stable_ids`
derives max+1 on the next edit.

### 2.4 Content hash stays id-INDEPENDENT: `stable_hash` skips the STID bytes

The central tension: stable ids are content-*independent* identity, but `stable_hash` (the cook-cache content key)
must stay id-*independent* — else re-authoring identical content cache-misses. Resolution: **`stable_hash` hashes
the blob MINUS the `STID` chunk bytes**. A no-STID blob hashes identically to today ⇒ **zero content-hash churn**:
pre-8d cooked content hashes stay valid, cache hits survive the migration. The principle generalizes: **content
projections are id-free** (`stable_hash`, and the text form — §2.5); **identity/persistence surfaces carry ids** (the
binary STID chunk + the in-memory Module).

### 2.5 The TEXT form is the id-free content projection (documented-why)

Text does NOT emit stable ids — it stays the human/debug/authoring projection, exactly as `stable_hash` excludes
them (§2.4). Rationale: (a) the same content-vs-identity-surface principle; (b) emitting a per-op id token would
change every op line and churn every existing print/round-trip golden for no persistence gain — the BINARY form +
the in-memory Module are the identity-persistence surfaces an editor operates on, never a text re-parse. Consequence
(pinned by a test): a FRESH module's text round-trip reproduces IDENTICAL ids (parse re-seeds pre-order, matching
the fresh assignment); only *post-edit id history* is lost through text (fully preserved through binary). This is a
deliberate, documented divergence from the 8a/8b U-§56 text≡binary parity — identity is not content.

### 2.6 One shared assignment routine

`assign_stable_ids(const Module&)` is the SINGLE implementation of the pre-order/max+1 rule, called by `serialize`
(before emitting STID) AND `interface_hash` (before reading ids for the state schema). Two implementations of the
rule would drift (the 8h content-addressed-incremental hazard). Idempotent ⇒ any call order yields the same ids.

### 2.7 The §20 state schema re-keyed by stable id — the false-incompatible fix

The interface-hash state schema (`program_asset.cpp`) changes from body-order `(type, depth)` to
`(stable_id, type, depth)` **sorted by stable id**. ⛔ The id VALUE goes into the hash, not just the sort order (the
advisor's discriminator): v1 `{id=1: f32}`, v2 deletes it and adds `{id=2: f32}` — an order-only hash calls these
compatible and 10a migration then finds no id-1 and silently loses state behind a "compatible" verdict; hashing the
id value makes them correctly incompatible. Sorting by id makes a reorder invariant (the fix). This CHANGES the
interface hash of every stateful module ⇒ **`kCeirCookSchema` 2→3, a NAMED recook** (like 8c). No `kBinaryVersion`
bump (the module blob is byte-unchanged for content; STID is additive/skippable).

### 2.8 Runtime state cells stay `Operation*`-keyed (transient, not persistent)

The interpreter's `m_cells` (`exec.cpp`) is a per-run, scratch-allocated map where `Operation*` is a valid, optimal
key (pointers are stable within one run). It is NOT a persistent surface, so 8d does not re-key it. The persistent
identity is the StateEdge op's stable id (§2.7); cross-version cell MATCHING by that id is migration — CEIR-10a.

## 3. Consequences

- **Zero content-hash churn** (stable_hash skips STID); a NAMED interface-hash recook (kCeirCookSchema 2→3) for
  stateful modules; NO `kBinaryVersion` bump.
- **`Operation` grew one `u64`** (`mutable`); the stale-.obj rebuild across all 3 targets is the build cost.
- **Identity survives edits/reorders/renames** and serialize/deserialize (binary); text preserves fresh-module ids
  but not post-edit history (§2.5).
- **Unblocks** 10a state migration (cells now have stable identity), 8i transactions, semantic diff, visual node
  identity, and gives 8c's built-in location kinds their per-instance identity anchor.

## 4. Alternatives rejected

- **Context-global create-time counter** — breaks the blob-is-a-pure-function-of-content gate (§2.2).
- **Content-seeded ids** — would change on every edit; violates the survives-edits contract.
- **STID inline in the BODY op records** — would change the body format ⇒ a `kBinaryVersion` bump + a non-skippable
  break; the additive chunk is strictly better (old decoders cope).
- **stable_hash over the whole blob (incl. STID)** — pollutes the content-addressed cook cache (identical content,
  different ids ⇒ cache miss). Rejected; stable_hash skips STID.
- **Ids in the text form** — churns every print golden for no persistence gain; text is the content projection
  (§2.5). (Reconsider if/when the editor needs a text-native identity round-trip; named-forward.)
- **Re-keying the interpreter's runtime `m_cells` by StableId** — it is transient within-run state where `Operation*`
  is correct; cross-run matching is 10a migration (§2.8).

## 5. Test matrix (`test_stable_id.cpp`, `[stable-id]`)

Identity: a fresh module gets pre-order `1..N`; `assign_stable_ids` is idempotent (a second call is a no-op); a
REORDER of the body preserves every op's id; an op appended after assignment gets max+1 (never a reused/pre-order
id). Serialization: a round-trip preserves ids (binary); a **duplicate/zero id** in a hand-crafted STID chunk is a
graceful reject (not an assert); a count mismatch rejects; a single-byte-corruption fuzz sweep over a
stable-id-bearing blob never crashes (ASan). Content vs identity: two modules with identical content but different
ids have the **same `stable_hash`** (the STID-skip property) and a pre-8d blob's stable_hash is unchanged after a
round-trip (fixpoint: decode→serialize→same stable_hash, then byte-exact on the second round-trip). Blob purity: the
same graph in a clean vs pre-polluted Context serializes byte-equal (ids are content-pure — the gate the Context
counter would have broken). State schema: a reorder of StateEdge cells leaves the interface hash unchanged (the
false-incompatible fixed); deleting cell id=1 and adding id=2 makes it change (the value-in-hash discriminator);
`kBinaryVersion == 2` (unchanged). Text: a fresh module's print→parse→`assign_stable_ids` reproduces identical ids.
