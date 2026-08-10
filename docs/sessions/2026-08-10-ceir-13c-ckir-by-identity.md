# CEIR-13c — CKIR-by-identity: KernelRef deps + the cook-time interface-hash check (§85/§107)

**Date:** 2026-08-10 · **Slice:** CEIR-13c (D-007 master spine) · **Status:** ✅ **CLOSED (PART 1 core + PART 2 cook bridge,
both gated)** · **ADR:** none (extends the existing cook decision surface — not a new tier).

## Contract

D-007 CEIR-13c row: *"CKIR-by-identity: kernel asset refs resolved through the ADR-0104 cache; interface-hash checked vs
the dispatch signature at cook (declared-contract)."* → §85 §107. The first slice to reach OUT of `crd-ceir` core into the
`crd-ceir-cook` bridge. Advisor-reviewed at the fork — **Reading B** (the dispatch PINS an expected hash, cook compares
pin == resolved; no cross-layer hash recompute — ADR-0109's one-way contract) — and split, on the advisor's endorsement,
into PART 1 (core extraction) now + PART 2 (cook check) next tick.

## The honest split (both parts now closed, across two ticks)

- **PART 1 (CORE) — DONE + gated:** the schema-driven KernelRef-extraction machinery + the two named-forward strikes +
  core tests.
- **PART 2 (COOK BRIDGE) — DONE + gated:** the CDEP v5 format bump + `cook_program`'s resolver params + the two
  `CookError`s + the declared-contract re-walk + `tests/ceir-cook` tests.

## PART 1 — what landed (schema-driven, I6-clean)

The dispatch's `kernel` is a bare symbol (13a). 13c completes the KernelRef `{asset_id, interface_hash}` and extracts it:

- **opgen `[op.kernel_ref]` table** (`{symbol, interface}`, the `[op.native]` precedent) — the generator VALIDATES the named
  attrs exist on the op with the right kinds (declared-words-validated: `symbol`→a symbol attr, `interface`→an int attr,
  optional) and emits `.kernel_ref_symbol`/`.kernel_ref_interface` into the register_op OpSpec CONDITIONALLY (non-kernel
  dialects regen byte-identical, drift-safe). `test_opgen.py` +6 (promotion, interface-optional, 4 validation negatives).
- **`compute.dispatch`/`dispatch_indirect`** gain an OPTIONAL `kernel_interface` int attr (the expected §107 interface hash,
  a PIN — the u64 stored as its i64 bit pattern; absent = unpinned, dependency-only) + the `[op.kernel_ref]` marker. ⛔
  OPTIONAL keeps 13a's shipped tests + op version untouched (additive, 12b precedent, no version bump).
- **`OpSpec`/`OpInfo`** gain `kernel_ref_symbol`/`kernel_ref_interface` (stored by reference — the generated static attr
  names outlive the Context, the native_provider precedent). `register_op` promotes them.
- **`DependencyRecord.ckir_refs`** (`Array<KernelRefDep{name, interface_hash, pinned}>`) — ⛔ `pinned` is a distinct bool,
  NOT `hash==0` (a real FNV can be 0 — the 8f phantom-id shape). `collect_dependencies` extracts them SCHEMA-DRIVEN from
  `op_info.kernel_ref_symbol` (never op-name-sniffing — I6), reading the kernel symbol + the optional interface pin; deduped
  by name (first-seen pin; the authoritative per-dispatch check re-walks at cook), sorted by name.
- **Two named-forwards STRUCK in place:** §64 (`ckir_refs` LANDED) and program_asset.hpp:28 — rewritten: KernelRef deps are
  DELIBERATELY excluded from `interface_hash` (which kernel a program dispatches is IMPLEMENTATION, not caller-visible
  contract — folding it in would break the §107 hot-swap property; the program→kernel edge rides the §106 deps + the cook
  check instead; a future §107 resource-I/O-surface folding is a distinct unbuilt item).

**Core tests** (`test_compute.cpp` +1): `collect_dependencies` extracts ckir_refs with pin + unpinned + dedup + sorted; the
**I6 pin** (a hand-registered op in another dialect carrying an attr literally named `"kernel"` but NO marker is NOT
extracted); a **high-bit interface hash** (`0x8000…0001`) survives BINARY + TEXT round-trip (the negative-int i64-bit path).

## PART 2 — what landed (the cook bridge)

- **CDEP v5** — the FIRST cooked-format change of the detour (every prior close said "no version bump"): `kCeirCookSchema`
  4→5; `serialize_deps` appends the ckir_refs list LAST (per ref: name string, u64 interface_hash, u8 pinned); `read_program`
  parses it (`Reader` gained `u64`/`byte`). ⛔ a stale v4 blob rejects cleanly at the `read_cooked_header` schema check
  (`BadHeader`) — never mis-parses (the v2/v3 clean-reject precedent). kBinaryVersion + the content hash (`stable_hash` of
  the MODULE, not the blob) UNCHANGED — zero content churn. `ReadResult.deps.ckir_refs` populated.
- **The cook check** — `cook_program(..., KernelResolveFn resolve = nullptr, void* user = nullptr)` (fn-ptr + user, the
  RunHooks precedent; `nullptr` = resolution DEFERRED, the CDEP still persists the refs — a documented phase split). A
  RE-WALK (`scan_kernel_contracts`) via the SAME `op_info.kernel_ref_symbol` field collect_dependencies reads (one schema),
  so `CookResult.op` points at the OFFENDING DISPATCH: existence ALWAYS checked (`KernelUnresolved`), hash IFF pinned
  (`KernelInterfaceMismatch`). `cook_program_text` forwards the resolver (the §121 no-privileged-path property holds).
- **Bridge tests** (`tests/ceir-cook/test_program_cook.cpp` +1): cook a stand-in kernel program (its `interface_hash` is the
  "actual"), resolve via a table-backed mock — pin-match OK / mismatch → `KernelInterfaceMismatch` pointing at the dispatch
  / unknown name → `KernelUnresolved` / unpinned+resolvable OK / no-resolver → cook OK + a `read_program` round-trip returns
  the persisted ckir_refs (two dispatches, one pinned + one unpinned, exercise the count>1 CDEP path, the sort, and the
  `pinned=0` wire branch; the name + interface_hash + pinned bit all survive the v5 CDEP).

## Gate (both parts, GREEN)

- **win-debug 475/475 · win-asan 475/475 · linux-gcc-debug 475/475 · linux-gcc-asan 475/475** (was 473; +1 core extraction
  test [part 1] +1 bridge test [part 2]).
- opgen regen + `--check` drift-clean + `test_opgen.py` **59** (was 53; +6). GCC clean (`-Werror=switch`: the two new
  `CookError` arms). LLVM-20 tidy clean: part 1 (`dialect.hpp`, `dialect.cpp`, `program_asset.hpp`, `program_asset.cpp`,
  `test_compute.cpp`) + part 2 (`program_cook.hpp`, `program_cook.cpp`, `test_program_cook.cpp`). `crd-ceir-invariants` OK
  (I3/I5/I6/U-116 — the extraction + the cook walk are schema-driven, no switch on op.kind).
- ⛔ **The FIRST cooked-format change of the detour (CDEP schema v4→v5)** — stale v4 blobs clean-reject at the schema check;
  content hash + kBinaryVersion unchanged.

## Proposed commit (the USER commits — no AI co-author trailer)

```
feat(ceir): CEIR-13c CKIR-by-identity — KernelRef deps + the cook interface-hash contract (§85/§107)

Core (crd-ceir): an opgen [op.kernel_ref] {symbol, interface} marker (validated,
promoted to OpSpec/OpInfo); compute.dispatch/_indirect gain an optional
kernel_interface hash-pin attr; DependencyRecord.ckir_refs {name, interface_hash,
pinned} extracted by collect_dependencies schema-driven from op_info.kernel_ref_symbol
(I6, never op-name-sniffing). Strikes two named-forwards (ckir_refs landed; KernelRef
deps deliberately excluded from interface_hash to preserve the §107 hot-swap property).

Bridge (crd-ceir-cook): CDEP schema v4->v5 carries the ckir_refs; cook_program gains
a defaulted KernelResolveFn resolver — a dispatch's @kernel must resolve
(KernelUnresolved) and a pinned interface hash must match the resolved kernel's actual
(KernelInterfaceMismatch), diagnosed at the offending dispatch. A null resolver defers
resolution; the CDEP still persists the refs.

Reading B (the dispatch pins, cook compares pin==resolved) — no cross-layer hash
recompute, honoring ADR-0109's one-way CEIR->CKIR contract. The structural
bindings-vs-kernel-params check is named-forward to 13z/kir-side.

Gate: 475/475 across win-debug/win-asan/linux-gcc-debug/linux-gcc-asan; opgen
drift/validator (59), LLVM-20 tidy, crd-ceir-invariants all clean.
```
