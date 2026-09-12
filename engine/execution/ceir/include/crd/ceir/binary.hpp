#pragma once

// crd-ceir — the BINARY serial form of the IR (CEIR-1f, §104/§123). The compact, versioned, forward-compatible
// sibling of the textual form (print.hpp/parse.hpp). Magic 'CEIR' + a version word + FourCC/length-prefixed chunks
// in the CRDR mould (ADR-0038): a reader iterates chunks and SKIPS any FourCC it does not know by the stored length,
// so a newer producer's extra chunks never break an older consumer. Every record is FIELD-BY-FIELD little-endian
// (⛔ NEVER a raw struct blast — the struct-padding-in-content-hash scar); the string / attribute / source-file
// POOLS are built from the MODULE WALK (first-use order, only what the module references), so the blob is a PURE
// FUNCTION OF MODULE CONTENT — identical graphs serialize byte-equal regardless of Context history (content-hash-safe).
//
// Chunks (v1): 'STRP' string pool · 'SRCM' source-file map · 'ATTR' attribute-value pool · 'BODY' the region graph.
// Unlike the text form, the binary form CARRIES `Region::kind` (Graph vs SsaCfg). It round-trips byte-exact with
// itself (serialize∘deserialize∘serialize == serialize) and agrees with the text form (print∘deserialize∘serialize
// == print). TypeId is an opaque u32 here (there is no type intern table until CEIR-3, which will add a type chunk
// and bump the version).

#include <crd/ceir/context.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/parse.hpp> // ParseResult — the shared load-result shape (module | ok | byte offset)
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir
{
// The container magic 'CEIR' (little-endian on disk) and the format version. A version bump cleanly rejects older
// blobs (recook). v2 (CEIR-3a) adds the 'TYPE' chunk — an interned structural-type pool (child-first, so every record
// references strictly-lower indices) — and makes result/block-arg types per-element type-pool refs (1-based; 0 = none).
inline constexpr u32 kBinaryMagic =
    static_cast<u32>('C') | (static_cast<u32>('E') << 8U) | (static_cast<u32>('I') << 16U) | (static_cast<u32>('R') << 24U);
inline constexpr u32 kBinaryVersion = 2U;

// v1 decode cap for a count that costs ZERO stream bytes (a block's arg count, an op's result count) — a hostile blob
// could otherwise declare 4e9 and force a multi-GB allocation before any byte is consumed. Counts whose elements DO
// cost stream bytes (operands, attrs, regions, blocks) are bounded by the chunk length instead. A real CEIR op stays
// far below this; a blob that exceeds it is rejected as malformed.
inline constexpr u32 kMaxDecodeCount = 1U << 20U;

// Serialize `module` to a fresh little-endian byte blob allocated from `alloc`.
[[nodiscard]] containers::Array<u8> serialize(Context& ctx, const Module& module, memory::IAllocator* alloc);

// A DETERMINISTIC content hash of a module: FNV-1a over its canonical binary form (serialized into `scratch`). Because
// the blob is a pure function of module content (CEIR-1f), equal-content modules hash EQUAL regardless of Context
// history, and the hash is STABLE across runs/processes — the foundation later bands build cook-cache keys / variant
// dedup on (the CKIR-serialize precedent). NOTE: like the blob, it is attribute-INSERTION-order sensitive (two
// semantically-equal modules whose attrs were added in a different order hash differently) and version-sensitive (a
// format bump changes the hash — a recook, by design); a canonical-order pass is a later-band consideration.
[[nodiscard]] u64 stable_hash(Context& ctx, const Module& module, memory::IAllocator* scratch);

// Deserialize a blob into a fresh Module owned by `ctx`. Never throws; on any malformed/incompatible input returns
// {ok=false, module=nullptr, error_offset=<byte>}. Symbol-defining ops re-enter the module's SymbolTable (a duplicate
// name is a load error), so the loaded module resolves like the original.
[[nodiscard]] ParseResult deserialize(Context& ctx, containers::ConstSpan<u8> bytes);
} // namespace crd::ceir
