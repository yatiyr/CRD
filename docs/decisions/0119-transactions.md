# ADR-0119 — Transactions: the atomic authored-mutation surface (commit / rollback)

**Status:** **ACCEPTED** (2026-08-09, advisor-approved under the CEIR-8 gold-standard autonomous cadence) — the
D-007 **CEIR band 8 (Foundation Closure)**, slice **CEIR-8i**, the LAST framework before the 8z band gate. A
`Transaction` records a set of IR mutations against a `Module`, applies them EAGERLY, and either COMMITS them
atomically or ROLLS them back to the byte-identical pre-transaction state.
**Phase:** D-007. Law: `docs/research/2026-08-09-ceir-universality-review.md` §B (row "Transaction model"); mission
§15/§71; the CAD-parametric / agent-editing / reactive-UI domain rows key off this.
**Tags:** `[ceir]` `[transaction]` `[mutation]` `[undo]` `[foundation]`

---

## 1. Context

Every authored edit to a live program — a node-graph editor moving a wire, an agent rewriting a subgraph, a CLI
`ceir edit` command — is a MULTI-OP mutation that must be **all-or-nothing**: a validation failure partway through
must leave the module exactly as it was, not half-edited. CEIR had the mutation PRIMITIVES since 1a (`create_operation`
+ `Block::append`/`insert_before`, `Operation::erase`, `Operation::set_operand`, `Value::replace_all_uses_with`,
`Context::set_attr`) but no atomic grouping, no rollback, and no seam feeding the 8h incremental engine. This is the
last U-§ foundation gap before the band gate; the CAD-parametric, agent-editing, and reactive-UI universality domains
are all blocked on it.

Three prior slices are the reused substrate: **8d stable ids** (a transaction names a pre-existing op by its
content-independent `StableId`, which survives the edit — never a pointer/pre-order that a concurrent edit
invalidates), **8h `IncrementalDag`** (a committed transaction reports the touched node-set → the incremental engine
recomputes), **8g `DiagnosticEngine`** (a rejected edit / failed commit reports through a `Diagnostic`, never an
assert).

## 2. Decision

### 2.1 The model — eager mutation + an inverse-op UNDO JOURNAL (reverse-order replay)

A `Transaction` (`transaction.hpp`) holds a `Context&`, the `Module&` it edits, a `DiagnosticEngine&`, and a
tx-owned journal `Array<MutationRecord>`. Each mutator APPLIES its edit eagerly (so the transaction body observes its
own edits — an editor builds op X then wires Y→X within one transaction) and pushes an INVERSE record. `rollback()`
replays the inverses in **reverse order**; `commit()` finalizes and discards them.

⛔ **Reverse-order replay is the correctness proof.** Every primitive's inverse restores exactly the state its forward
changed; replayed strictly last-to-first, the composition telescopes back to the pre-transaction state. The subtle
cases fall out of the ordering rather than needing bespoke logic: a new op wired to another new op's result un-wires
before its producer is un-inserted (the producer's result is use-free by the time we erase it); an erase that was
preceded by a RAUW off the erased op's results is un-erased first, then the RAUW is undone back onto the restored
results. (Tested both interleavings — §6.)

Two rejected models (§5): a **copy-on-write module snapshot** fights the arena's stable-pointer identity model
(handles ARE identity; a snapshot mints new pointers, breaking every external reference and stable-id continuity); a
**deferred-apply** journal (record now, apply at commit) means the transaction body cannot see its own intermediate
edits — unusable for a real editor.

### 2.2 Transaction RECORDS; Context APPLIES — never a second mutation implementation

The `Transaction` is a pure recorder: every forward edit and every inverse routes through the `Context`, which owns
ALL raw mutation (the frame-graph-is-a-recording-mode discipline). Six thin transaction-only `Context` primitives
(commented like `set_stable_id`'s "deserialization-only") give the recorder the privileged reach it needs without
duplicating mutation logic or touching `ir.hpp` (all ride `Context`'s existing friendships with
`Value`/`Operation`/`Block`/`Region`/`Module`):

- `reinsert_erased_op(op, block, before, operands)` — the inverse of erase: restore `m_num_operands`, re-thread the
  recorded operand values into the (arena-preserved) `Use` slots, re-link at the recorded anchor, clear the tombstone.
- `detach_and_point_use(use, value)` — repoint one `Use` (updates both use-lists); the forward+inverse RAUW atom.
- `rauw_recording(from, to, moved)` — walk `from`'s use-list, capture each `Use*` into `moved`, repoint to `to`
  (the recorder pushes one inverse per captured use → RAUW reuses the `detach_and_point_use` inverse).
- `restore_attr_dict(op, dict, count)` — the inverse of `set_attr`: swap the op's attribute dict back to a snapshot.
- `resync_symbols(module, first_dup&)` — rebuild the module's `SymbolTable` from its ops (the SAME
  `detail::register_symbol` path deserialize/parse use), swapping in a fresh table ONLY on success; on the first
  duplicate `sym_name` it mutates nothing and returns the offender.
- `find_by_stable_id(module, id)` — resolve a `StableId` to its op (pre-order), the "reference by stable id" surface.

The public transaction API: `insert` (create + place an op, including fresh region/block scaffolding INSIDE the new
op), `erase`, `set_operand`, `replace_all_uses_with`, `set_attr`; `commit()` / `rollback()`; `find(StableId)`;
`touched()` / `removed()` (valid after commit).

### 2.3 Graceful reject + poison — the agent-facing mutation surface never asserts

This surface is editor/agent/CLI-facing, so the deserialize=build-raw-graceful-reject scar applies: every mutator
PRE-VALIDATES (op belongs to this module, not already erased, operand index in range, an erase target's results are
use-free) and on a violation EMITS a `Diagnostic`, records nothing, and **poisons** the transaction. A poisoned
`commit()` auto-rolls-back and returns false. Delegating straight to the asserting 1a primitives would turn hostile
agent input into a crash.

⛔ **Erase of a REGION-BEARING op checks BOTH subtree boundaries.** `Operation::erase` does not recurse, so a nested
op's cross-boundary SSA edge would leak: an IN-edge (a nested operand defined OUTSIDE the erased subtree stays threaded
into that external value's use-list after the tombstone — `has_uses()` reads true forever) or an OUT-edge (a nested
result consumed OUTSIDE becomes a live use into a dead subtree, invisible to a root-results-only check). The erase
mutator walks the subtree once and rejects either crossing edge (rewire across the boundary first) — the same
graceful-reject + poison. A CLOSED subtree (every nested edge internal) is accepted and erased whole; rollback restores
it whole (erase never recursed, so `reinsert_erased_op` on the root brings the intact subtree back).

### 2.4 Atomicity + the commit sequence (verify BEFORE the point of no return)

`commit()`:
1. Poisoned ⇒ `rollback()`, return false.
2. **Verify (dry-run, no mutation):** run each touched/inserted op's registered `Context::verify`; then, if the
   transaction touched any symbol-bearing op, `resync_symbols` (which mutates only on success) as the LAST fallible
   check — a duplicate `sym_name` introduced by the edit is a commit failure. Any failure ⇒ emit a diagnostic,
   `rollback()`, return false. ⛔ Verify runs BEFORE any id assignment, so a FAILED commit never advances the
   watermark.
3. **Point of no return — infallible:** `assign_stable_ids(module)` (new ops draw ids); compute `touched`/`removed`.
4. Mark committed, return true.

The money property (§6): a mid-transaction FAILURE (a rejected edit, or a commit-verify failure such as a duplicate
symbol) leaves `serialize(module)` **byte-identical** to the pre-transaction bytes.

### 2.5 Stable-id allocation under rollback (the real subtlety)

`begin` (the constructor) calls `assign_stable_ids(module)` up front — settling every pre-existing op's id — and
records the resulting watermark. This is not cosmetic: it is what makes byte-identity UNCONDITIONAL. `serialize`
already assigns ids lazily, so the settled state IS the canonical serialized form; and with pre-existing ids settled
at begin, the ONLY id assignments inside the transaction window are to transaction-CREATED ops (all erased on
rollback). Without it, a mid-transaction `serialize` would interleave fresh ids onto pre-existing ops that rollback
could not return to `0`, and the STID chunk would differ.

`rollback()` replays the inverses AND restores the watermark to the recorded begin value. Transaction-created ops
that were assigned transient ids (only if the body serialized) are erased; their ids sit in dead, tombstoned arena
memory, invisible to every pre-order walk / hash / serialization — so restoring the watermark (and later reusing
those id VALUES for different LIVE ops) is safe. Commit KEEPS the advance. ⛔ This preserves the 8d monotone-erase
guarantee across the rollback boundary precisely BECAUSE rollback fully reverts: after it, the live-op set and the
watermark equal their pre-transaction values, so a subsequent erase+add behaves exactly as if the transaction never
happened — the delete/re-add discriminator still holds (§6 tests it across a COMMITTED transaction: erase id-K op +
insert a replacement ⇒ the replacement's id ≠ K).

### 2.6 The incremental seam (8h) — the transaction reports WHAT changed; the consumer computes revisions

On commit, `touched()` returns the `StableId`s of the inserted + modified LIVE ops and `removed()` the `StableId`s of
the pre-existing ops erased by the transaction. ⛔ NET-OUT: an op inserted-then-erased within the same transaction
appears in NEITHER set; the touched set for a RAUW is the OWNER ops of the moved uses (not `from`'s defining op). The
transaction reports only the id-set; the CONSUMER (a future `AnalysisManager` / `CookDb`, named-forward per 8h)
computes each touched node's content/interface revision (via the existing `stable_hash` / interface-hash projection)
and drives `IncrementalDag::recompute_after_change` + analysis invalidation — the SAME division of labour 8h fixed
(the engine holds structure + revisions + propagation; the consumer supplies the hashes). §6 proves the seam with a
test-only consumer: an `IncrementalDag` mirroring the module deps, a commit, and the touched set applied → the §107
rule fires (an interface change propagates to dependents, a content-only change does not).

### 2.7 Scope — authored edits, not compiler rewrites

Transactions are THE surface for EXTERNAL authored mutation. Compiler-INTERNAL rewriting (5a `fold_constant_if`, the
8g `RewritePattern`/`try_apply` driver) is a SEPARATE discipline and is NOT retrofitted through transactions this
slice — the rewrite driver ADOPTS transactional journaling at CEIR-26 (the MLIR rewriter-listener precedent),
named-forward. There is no second mutation IMPLEMENTATION: both disciplines are entry points over the one `Context`
mutation path. The "single mutation path" mandate binds WITHIN authored editing — an editor must not bypass the
transaction to poke the IR directly.

Vocabulary this slice: op insert (with scaffolding inside the new op), erase, `set_operand`, RAUW, `set_attr`.
⛔ Block-append to a PRE-EXISTING region is named-forward, NOT half-built: 1a has no block-unlink primitive, so its
inverse does not exist yet; adding structural block/region edits (and their inverses) is its own future increment
rather than a partial mutator here.

⛔ **The scaffolding contract.** Raw `create_block`/`append` into a NEWLY-INSERTED op's own region is sanctioned for
USE-FREE STRUCTURE ONLY (empty blocks/regions); every value-TOUCHING inner edit (an op that consumes/produces a value)
must go through `tx.insert`/`tx.set_operand`, so the transaction tracks it and rollback erases it in reverse. A raw
inner op that references an external value is NOT tracked — rollback erases the outer op but leaves that phantom use
threaded into the external value. Within-contract (all edits through the transaction) this cannot happen; a caller that
mixes raw and transactional edits breaks the single-mutation-path mandate.

⛔ **RAUW does not validate `from`/`to` module membership** (semantic trust, no crash path): `rauw_recording` only
touches uses of `from` and records each moved use's inverse, so a cross-module RAUW is nonsensical but never corrupts
this module's state — a deliberate scoping, not an oversight, consistent with the op-taking mutators' cheaper checks.

## 3. The recook story — ZERO format motion

A transaction is runtime edit-session state, exactly like the 8g `AnalysisManager` and the 4b compiler mode — it
SERIALIZES NOTHING. `kBinaryVersion` stays 2, `kCeirCookSchema` stays 4; no new serialized record ⇒ no new fuzz
corpus. A COMMITTED transaction changes the module's CONTENT, so its content hash changes and it recooks — but that is
the ORDINARY content-hash cache miss the existing mechanism already handles, NOT a schema/format recook.

## 4. Consequences

- Atomic multi-op authored edits with rollback — the substrate every editor / agent / CLI mutation builds on; the
  8d/8h/8g investments earn their keep (identity survives edits, the dirty seam feeds the incremental engine,
  failures report as diagnostics).
- A single authored-mutation path (record + delegate) — no rival mutation surface can bypass atomicity.
- The CAD-parametric, agent-editing, and reactive-UI universality domains unblock (they are transaction-shaped).
- ⛔ Use-list LINKAGE order may permute across a rollback (`add_use` pushes at the head); serialization never walks
  use-lists so byte-identity is unaffected, but a "first use" analysis may latch a different-but-equivalent offender
  after a rollback (documented, not a defect).

## 5. Alternatives rejected

- **Copy-on-write module snapshot** — fights the arena's stable-pointer identity model (handles are identity; a
  snapshot invalidates every external reference and breaks stable-id continuity); rollback-by-swap is not worth
  discarding the whole IR's pointer contract.
- **Deferred-apply journal (apply at commit)** — the transaction body cannot observe its own intermediate edits;
  unusable for an editor that builds on prior edits within one transaction.
- **Transaction as a friend of the IR classes** — spreads privileged mutation across four classes and duplicates the
  logic `Context` already owns (a second mutation implementation); the recorder-over-Context design keeps ALL raw
  mutation in one place.
- **Forcing `fold_constant_if` / the 8g rewrite driver through transactions now** — steals shipped-band scope and
  conflates compiler rewriting with authored editing; named-forward to CEIR-26.

## 6. Test matrix

`tests/ceir/test_transaction.cpp` (`[ceir][transaction]`, ASCII names): commit applies + `touched`/`removed`
correct; **the A/B money test** — a mid-transaction FAILURE (a poisoned edit) rolls back to a byte-identical module
(`serialize` before == after); commit-verify-failure auto-rollback byte-identity (a duplicate-`sym_name` commit);
reverse-replay ordering (interleaved erase/insert of adjacent ops, BOTH orders → exact block order restored); the
delete/re-add discriminator across a COMMITTED transaction (erase id-K op + insert replacement ⇒ new id ≠ K);
watermark-restore under rollback WITH a mid-transaction serialize; the poisoned-tx reject path (erase-with-live-uses
⇒ diagnostic + no mutation); the attr-undo lifetime proof (`set_attr` overwrite AND add, rollback, **destroy the
Transaction**, then read attrs + serialize — the linux-asan UAF guard, because a rolled-back attr snapshot BECOMES
live module state and MUST be Context-arena, not tx-owned); the 8h seam (build an `IncrementalDag` over the module
deps, commit, apply `touched()` → `recompute_after_change` fires the §107 rule); the region-bearing erase
subtree-boundary rule (reject a nested IN-edge, reject a nested OUT-edge, ACCEPT a closed subtree — commits gone AND
rolls back byte-identically). Total **12** `[ceir][transaction]` cases. The 4-config CEIR gate
(win-debug/asan + linux-gcc-debug/asan) + GCC `-Werror=switch` + LLVM-20 tidy + opgen drift/validator; zero
serialization ⇒ no fuzz corpus, no version/schema motion.
