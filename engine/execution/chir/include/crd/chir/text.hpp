// text.hpp — the CHIR TEXT projection of the CHIR source model (CEIR-32c, ADR-0128 D5).
//
// TEXT and the CR-D007 GRAPH (schema.hpp) are the two PROJECTIONS of the ONE `SourceModel` (ADR-0128 D5). This is the
// human-authored LANGUAGE surface: `print_chir` renders the model as CHIR text, `parse_chir` reads it back. Unlike the
// graph schema (a machine-canonical DATA document that carries stable ids + layout + spans verbatim), the text form is
// SEMANTICS ONLY — it is span-BLIND and layout-BLIND by construction (a language does not print line numbers or node
// coordinates). That is exactly what makes it the parity anchor: `semantic_hash` IGNORES span + layout (§180 #10), so
//   print_chir(oracle) == committed .chir            (anti-drift through the printer)
//   print_chir(parse_chir(t)) == t                   (text FIXED-POINT for a canonical t)
//   semantic_hash(parse_chir(.chir)) == semantic_hash(read_schema(.chirgraph))   (the two projections AGREE — 32d's
//                                                                                  precondition, cross-projection tooth)
// hold BYTE-exactly with no normalization. `parse_chir` re-derives ids (pre-order construction reproduces the oracle's
// node indices) and computes a real `SourceLoc` for every node from its text position (contract item 6, spans survive).
//
// ⛔ NOT the graph schema (that is schema.hpp). ⛔ host-only, no std containers. The GRAMMAR lives in docs/systems/chir.md.

#pragma once

#include <crd/chir/node.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::chir
{
// The outcome of `parse_chir` — the `fail`-latches-the-first-error mold (crd::ceir::ParseResult), with the byte offset
// resolved to a 1-based line:col so a LANGUAGE diagnostic reads `line:col: message` (contract item 6). `msg` is a static
// string literal (never allocated), valid for the program lifetime.
struct ChirParseResult
{
    bool                        ok       = false;
    crd::u32                    err_off  = 0; // byte offset of the first error (0 when ok)
    crd::u32                    err_line = 0; // 1-based line of the first error (0 when ok)
    crd::u32                    err_col  = 0; // 1-based column of the first error (0 when ok)
    crd::containers::StringView msg;          // a static message (empty when ok)
};

// Render `m` as a canonical CHIR text document (appended to `out`), LF-only, 2-space indent. SEMANTICS ONLY (no ids, no
// layout, no spans). Requires derive_ids() only for edge endpoints' node/pin NAMES to be present — the ids themselves
// are not emitted. Deterministic: the same model prints the same bytes.
void print_chir(const SourceModel& m, crd::containers::Array<char>& out);

// Parse a CHIR text document into `out` (which MUST be empty) with `file_id` stamped into every node's SourceLoc. On
// success the model is fully built (nodes in pre-order, edges resolved by name + canonically inserted, ids derived). On
// a malformed document returns {ok=false, ...} and leaves a PARTIAL model (callers discard it — graceful reject, never a
// merged/partial commit; the read_schema convention).
[[nodiscard]] ChirParseResult parse_chir(crd::containers::StringView text, crd::u32 file_id, SourceModel& out);

} // namespace crd::chir
