#pragma once

// frame_ceir.hpp — CEIR-15a (ADR-0127): the FrameGraphDesc <-> ceir.frame converter. §126 steps 1/2: the `.frame.toml`
// frontend + the `FrameGraphBuilder` both END at `ceir.frame` through this pair. HOST-TO-HOST (crd-frame-cook -> crd-ceir,
// acyclic: crd-ceir is host-only and depends on none of its providers, ADR-0109 §4.2). The frame graph's RESOURCES are
// ordinary `resource.declare`/`import` values (NO frame.resource op); a pass is a `frame.pass` (executor symbol + a
// `resources` operand tail tokened by `access`), so CEIR's hazard/lifetime machinery derives the barriers (§159).

#include <crd/framecook/frame_asset.hpp>

#include <crd/ceir/ceir.hpp>

namespace crd::framecook
{
// FORWARD (CEIR-15a-3a): build a `ceir.frame` module from a FrameGraphDesc —
//   func main { frame.graph { <resource.declare/import>* <frame.draw_list>* <frame.pass>* } }
// The module is owned by `ctx` (its arena). Registers the arith/func/resource/frame dialects on `ctx` (idempotent).
// Returns nullptr on a malformed desc (a pass reads/writes a resource or draw-list never declared) OR on a desc that still
// carries REN-37.6 composition (includes/anchors/injects) — composition is a §39 named-forward SUBGRAPH capability that
// `flatten_frame_graph` resolves into plain passes BEFORE conversion (PIPELINE: `parse → flatten → convert`; a PARSER-layer
// concern per §115). The reject is the PERMANENT contract (a caller that skipped the flatten), NOT an interim loss. The
// result is Context::find_frame_misuse-clean for a valid desc. The BACKWARD converter + the round-trip-identity gate are 15a-3b.
[[nodiscard]] crd::ceir::Module* to_ceir_frame(const FrameGraphDesc& desc, crd::ceir::Context& ctx);

// BACKWARD (CEIR-15a-3b): reconstruct a FrameGraphDesc from a `ceir.frame` module (the inverse of to_ceir_frame). Names +
// the exact FrameResourceKind + sizing/flags are recovered from the resource.declare/import attrs; a pass' reads/writes are
// recovered from its `resources` operands' `access` tokens + the operands' `name` attrs; the executor from its symbol. `out`
// is filled (cleared first); `alloc` allocates the sub-descs. Returns false on a non-frame module. The round-trip-identity
// gate is `emit_frame_toml(desc) == emit_frame_toml(to→from(desc))`.
[[nodiscard]] bool from_ceir_frame(const crd::ceir::Context& ctx, const crd::ceir::Module& m,
                                   crd::memory::IAllocator* alloc, FrameGraphDesc& out);

// ── CEIR-15c-1c: validate_ceir_frame — the GRAPH-SEMANTIC verifier layer (§115), the frame-cook home for the checks that
// Context::find_frame_misuse (crd-ceir CORE, per-op structural) cannot own: the ones needing a cross-op sweep, the @output
// endpoint convention, a FrameResourceKind, or (15c-1d) the executor registry. CEIR-NATIVE POINTING diagnostic (an offending
// `ce::Operation*`, the find_frame_misuse precedent), NOT a FrameCookError and NOT an extension of core's FrameMisuseKind (a
// layer violation). The transitional DIFFERENTIAL ORACLE (this verdict == the desc-side cook pipeline's verdict) lives in
// tests only. 15c-1c-1 = NoOutputPass + DuplicateName; the resource-shape/def-use/frame_kind rows land in 15c-1c-2+.
enum class FrameSemanticKind : crd::u8
{
    None = 0,
    NoOutputPass,         // no frame.pass WRITES the `@output` import — a (flattened) frame must produce its output endpoint
    DuplicateName,        // two ops in the SAME category (resource {declare+import} / draw_list / pass) share a `name`
    PingPongNeedsBothWays, // a lifetime=history (ping-pong) resource is only read OR only written — the pair would never rotate
    ResourceNeverWritten,  // a graph-owned TRANSIENT resource.declare no pass produces (persistent/history/import are exempt)
    // ── the REN-38-B2 SHAPE block (frame-cook: needs the FgImageKind enum) — a TransientImage only. ──
    CubeNeedsSquare,  // a cube / cube_array image whose width != height (the hardware has no non-square cube face)
    VolumeNeedsDepth, // a `dimension = "3d"` image with `depth == 0` (a volume the author believes is 3-D but reads slice 0)
    BadMipCount,      // `mips == 0` (not "full chain"), or more levels than the extent can halve down to
    LayersOutOfRange,     // a TransientImage with `layers == 0` or `layers > kFgMaxImageLayers`
    StructuredNeedsStride, // a structured/counter buffer with `stride == 0` (its elements have no size)
    StrideNotAligned,      // … or a structured/counter `stride` that is not a multiple of 4 (both APIs require it)
    AccelIsExternal,          // an AccelerationStructure (import) given a size/format — the graph never ALLOCATES one (host-built)
    ExternalTextureIsReadOnly, // a frame.pass WRITES an ExternalTexture (import) — the application owns its contents
    // ── NEW-IN-CEIR consistency checks: `ceir.frame` is a STRICT SUPERSET of FrameGraphDesc, so these have NO desc-side
    // oracle (the desc has one `kind` field + a parse-validated dimension; the ceir.frame can desync them). ──
    KindLifetimeMismatch, // a resource.declare whose `frame_kind` and `lifetime` disagree (kind_lifetime(frame_kind) != lifetime)
    UnknownDimension,     // a resource.declare `dimension` attr whose int is not a valid FgImageKind (2d/3d/cube/cube_array)
    BadResourceSize,      // a TransientImage with neither absolute size nor scale, or a TransientBuffer with no size_bytes
    PersistentNeedsSize,  // a persistent/history image sized ONLY by scale (its key must be stable across frames; unless resizable)
    // ── CEIR-15c-1d: the executor PROGRAM-CONTRACT layer. The ~18 per-executor contract violations (MissingShader/
    // MissingDrawList/Present*/RayTrace*/… ) come from the shared `pass_contract_diag`; the SPECIFIC FrameCookError rides
    // `FrameSemanticDiag::contract` (Option 2 — carrying the verdict beats mirroring 18 enum values, whose mirror IS the
    // drift surface). A check that ALREADY has its own kind (ExternalTextureIsReadOnly) maps back to that kind. ──
    ProgramContract,
    // ── CEIR-15c-1d-6: a closed-vocab PARAM carries an out-of-range enum int. NEW-IN-CEIR (no oracle: the desc-side parse
    // rejects bad enum STRINGS, and emit NORMALIZES a bad int to a default, so validate_frame_graph never sees one). The
    // specific FrameCookError (UnknownBlend/UnknownCompare/UnknownSamplerFilter/…) rides `contract`. ──
    UnknownEnumParam,
    // ── CEIR-15d-2: the pass-DAG has a DEPENDENCY CYCLE (a Kahn topo-sort failure over the REN-41 dependency graph). A
    // GRAPH-LEVEL error (op == nullptr — no single offending pass, like NoOutputPass); `contract` carries DependencyCycle.
    // Shares the ONE `dependency_cycle_diag` with the desc-side cook pipeline, so the differential oracle holds. ──
    DependencyCycle,
};
[[nodiscard]] crd::containers::StringView frame_semantic_kind_name(FrameSemanticKind k) noexcept;

// The pointing result: the FIRST semantic violation (pre-order), the offending op (the SECOND collider for DuplicateName;
// null for NoOutputPass — the whole graph is the subject). `kind == None` ⇒ the frame is semantically well-formed.
struct FrameSemanticDiag
{
    const crd::ceir::Operation* op   = nullptr;
    FrameSemanticKind           kind = FrameSemanticKind::None;
    // The specific verdict — MEANINGFUL for `kind == ProgramContract` (the per-executor contract), `kind == UnknownEnumParam`
    // (the closed-vocab that was violated), and the ExternalTextureIsReadOnly row (which carries both its kind AND this).
    // `FrameCookError::Ok` otherwise.
    FrameCookError contract = FrameCookError::Ok;
};
// ⛔ CEIR-15c-1d: `alloc` materializes a FrameGraphDesc (from_ceir_frame) so the shared pass_contract_diag can run — the
// program-contract layer's inputs (the executor + the full param bag) live in that desc.
[[nodiscard]] FrameSemanticDiag validate_ceir_frame(const crd::ceir::Context& ctx, const crd::ceir::Module& m,
                                                    crd::memory::IAllocator* alloc);
} // namespace crd::framecook
