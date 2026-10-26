# RAH-0 review brief (2026-10-26)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md#slice-rah-0). Rules: [AGENTS](../../AGENTS.md).

Planning only: no code, benchmark or runtime result. Prepared ahead of the row so the user can decide while DIAG closes.
RAH-0 reviews the canonical GPU model and its migration
([execution contract](../design/renderer-ui-execution-contract.md#rah-completion),
[August audit](../archive/systems/rah-0-canonical-model-audit.md), [ADR-0106](../decisions/0106-unified-frame-graph-runtime-render-graph.md)).
The August audit predates CEIR-14 to CEIR-35; its field census, line numbers and executor count must be refreshed
against the current tree before migration. A quick count on 2026-10-26 still finds the old shapes in engine sources:
`IGBufferTarget` (5 files), `gbuffer` (15), `visbuffer` (22), `kMaxBindlessTextures` (3), `native_args` (5),
`ray_tracing_pipeline` (10), `kMaxColorAttachments` (9), `read_pixel` (7).

## Decisions for the user

1. **The nine breaking changes** (audit §4), owned by RAH-1 to RAH-8:
   - typed attachments replacing `visbuffer`/`gbuffer`/fixed color slots;
   - one typed image with typed views, retiring the target zoo; readback becomes a transfer;
   - resident resource tables (bindless) replacing the eight-texture cap and fixed input slots;
   - typed indirect arguments and geometry variants keyed by stable IDs;
   - a production ray-tracing model (pipeline vs inline query as data, SBT, AS build/update/refit/compaction);
   - the transfer/sparse/streaming command family;
   - program-contract validation at cook/install and recording;
   - a dependency-driven program registry replacing the hand-listed retire-all caches;
   - a capability and maturity manifest replacing fixed caps.
   Accept all nine, or remove or defer any.
2. **GPU feature floor for the new model.** Resource tables, buffer device addresses and typed views assume modern
   features. Choose:
   - (a) a modern floor: Vulkan 1.3 with descriptor indexing and buffer device address; DX12 feature level 12_1 with
     shader model 6.6 resource heaps (about 2016+ GPUs); older GPUs are unsupported;
   - (b) the same floor plus an explicitly authored fallback path for older GPUs (more work and more tests in every
     RAH row).
3. **Old cooked assets.** Blob formats change. Choose:
   - (a) bump each format version and re-cook every asset from source; no reader for old blobs (no external users today);
   - (b) write migrators that read old blobs.
   The RAF-12.3 byte-identity round-trip discipline applies either way.
4. **Order.** The audit's order is RAH-1 typed attachments first (it unblocks deferred, visibility and the I2D canvas),
   then RAH-2 to RAH-8. Accept, or reorder.
5. **No-loss gate.** RAH-0 closes only when a refreshed census proves every shipped asset and mechanic maps onto the
   target model (audit §5, re-run on today's tree). Confirm that this refreshed proof, not the August table, is the gate.

## Recommendation

1 accept all nine; 2(a), since this engine targets modern hardware and Windows/Linux first; 3(a); 4 as audited; 5 confirm.

## What RAH-0 then does (after AUD-2 and CORE-USE)

Refresh the field/constructor/consumer/backend/serialized-form census, record old and new ownership and migration
order, and re-prove the no-loss table on today's tree. It changes no code.
