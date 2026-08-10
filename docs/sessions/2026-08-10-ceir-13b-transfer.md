# CEIR-13b — the `ceir.transfer` dialect: movement ops with static per-kind effects (§50)

**Date:** 2026-08-10 · **Slice:** CEIR-13b (D-007 master spine) · **Status:** ✅ CLOSED · **ADR:** none (op-vocabulary +
verifier slice, 12a/13a precedent).

## Contract

D-007 CEIR-13b row: *"`ceir.transfer`: buffer/image copies, upload/readback, clear, mip-gen — each declaring 4a effects."*
→ §50. The data-movement half of first-GPU-contact. Core-only (the `crd-ceir-gpu` bridge lowers at 13d).

## Design (advisor-reviewed at the fork; all verdicts + Q4 surgery integrated)

**`engine/ceir/ops/transfer.ceirop.toml`** — 5 ops, all with **STATIC per-kind** 4a effects (the advisor's forward: a copy
ALWAYS reads src / writes dst — no per-instance access to defer, so NO ambient baseline like 13a's dispatch):
- **`copy`** — operands `(dst, src)` (⛔ memcpy/D3D12 order, NOT Vulkan's src,dst — shouted in the op comment); effects
  `MemoryWrite{0} + MemoryRead{1}`. ONE op spans buffer/image/buffer↔image (kind combo from types, 12a doctrine);
  **subresource copies via VIEW operands are SHIPPED** (a view carries the range).
- **`upload`** `MemoryWrite{dst}` + open `source` symbol; **`readback`** `MemoryRead{src}` + open `dest` symbol (both
  resultless — host materialization is provider-side); **`clear`** `MemoryWrite{dst}` + `value` (the buffer FILL WORD, u32,
  optional); **`mip_gen`** `MemoryReadWrite{image}`.

**Why static effects are sound here (where 13a's GPUCommand-only wasn't):** both ends of the dispatch/export pair were
IR-visible ops, so GPUCommand-only lost ordering; a transfer names a specific operand and its other end (the host source)
is provider-side — no IR op-pair loses ordering. So `upload(dst)`→`readback(dst)` is `MemoryWrite`-then-`MemoryRead` =
**RAW by construction** (the upload→first-read barrier scar, IR edition), and — unlike a dispatch — a transfer does NOT
extend an unrelated resource's 12c live range (precise operand, never the whole Memory class).

**`Context::find_transfer_misuse`** — the advisor's Q4 surgery took it from 2 kinds to **6** (three legal-by-accident shapes
closed): `OperandNotTransferable` (predicate `ceir_is_transferable` = Buffer|Image|View-of-those one-hop; ⛔ opaque
ExternalResource is OUT — import it typed) · `CopySrcIsDst` (whole-resource self-copy by SSA identity; ⛔ distinct views of
one root PASS — tested) · `MipGenNotImage` (operand through a view one-hop is an Image) · `ClearValueInvalid` (present ∧
non-Int — the 13a standalone-walk fold) · `ClearValueOnImage` (a fill word on an image clear — a typed image clear is 13d,
the RAH-1a.1 clear_kind/clear_uint target). Contractual order per op.

**Known hole (doc line, TOML + here):** a transfer through a VIEW escapes `ops_hazard` today (distinct SSA Values
non-aliasing, struck-in-place at 12c; retrofit named-forward). 13b doesn't widen it but makes it easier to hit — so the
scar tests pin hazards on DIRECT resources, not through views. **NO-FOLLOW (TOML):** §50's tensor copy + blit + resolve +
peer copy + sparse page mapping + compression → 13+/26; typed image clear → 13d.

## Tests (`tests/ceir/test_transfer.cpp`, +3 TEST_CASEs, ASCII names — the advisor's checklist)

- well-formed one-of-each (incl. a buffer→image copy, a clear-image-with-no-value, a copy between two distinct VIEWS of one
  root → clean) + BINARY & TEXT round-trip with `source`/`dest`/`value` read-back on the twins.
- one negative per misuse kind (5).
- the **discriminating pins**: `ops_hazard(upload, readback) == HazardKind::Raw` (exact, not `!= None`); a transfer's
  precise effect does NOT extend an unrelated prior transient (the precise-vs-ambient contrast with 13a); and a cross-dialect
  seam block (`upload(%buf)` then a dispatch BINDING `%buf` → `!= None` — 13a's ambient composes with 13b's precise write).

## Gate (GREEN)

- **win-debug 473/473 · win-asan 473/473 · linux-gcc-debug 473/473 · linux-gcc-asan 473/473** (was 467; +3 hand +3 generated
  transfer smoke).
- opgen regen (5 transfer artifacts) + `--check` drift-clean + validator OK. GCC clean (`-Werror=switch`: the 6-arm name).
  LLVM-20 tidy clean: `context.cpp`, `context.hpp`, `test_transfer.cpp` (fixed a `&lts[0]` container-data-pointer nit).
  `crd-ceir-invariants` OK. No recook/fuzz/version-bump.

## Named-forwards (the rest of band 13)

- **13c** CKIR-by-identity — KernelRef resolution through the ADR-0104 cache; interface-hash vs dispatch signature at cook.
  ⛔ this lives in the **crd-ceir-cook BRIDGE** (program_asset.hpp: the CRDR `'CEIR'` chunk + the cook cache) — 13c's recon
  scope shifts OUT of core for the first time (tests likely `crd-ceir-cook-tests`, a different CMake home). **13d** the
  lowering pass (region → command stream, 4d-derived barriers — NARROWS 13a's ambient; the view→hazard retrofit lands with
  it). **13z** the §129 proof (add/reduce/scan/FFT as CEIR assets, Vulkan+DX12) + the ADR-0108 cornerstone flip.

## Proposed commit (the USER commits — no AI co-author trailer)

```
feat(ceir): CEIR-13b ceir.transfer dialect — movement ops with static per-kind effects (§50)

Add engine/ceir/ops/transfer.ceirop.toml (copy/upload/readback/clear/mip_gen):
copy is (dst, src) unified over buffer/image/buffer-image + subresource-via-views;
upload/readback carry open source/dest identity tags; clear's value is the buffer
fill word (typed image clear is 13d); mip_gen is image-only. Effects are STATIC
per-kind (Write{dst}/Read{src}/...), so a transfer hazards precisely — upload then
readback is exactly RAW (the upload->first-read barrier) and, unlike a dispatch,
does not extend an unrelated resource's live range. Context::find_transfer_misuse
enforces transferable operands, copy src!=dst, the clear value fold + image
placement, and mip-gen is-image. Host-only; the gpu-context lowering is 13d.

Gate: 473/473 across win-debug/win-asan/linux-gcc-debug/linux-gcc-asan; opgen
drift/validator, LLVM-20 tidy, crd-ceir-invariants all clean.
```
