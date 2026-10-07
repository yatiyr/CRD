// schema.hpp — the CR-D007 GRAPH-SCHEMA projection of the CHIR source model (CEIR-32b, ADR-0128 D5).
//
// The graph schema is the DATA form of the visual projection: nodes + typed pins + edges + a layout side-table, keyed by
// stable id. It is a canonical, deterministic, LF-only text serialization of `SourceModel` — a SERIALIZE FIXED-POINT
// (print(read(x)) == x) so the anti-drift-through-the-printer gate holds (the committed-asset mold). ⛔ This is NOT the
// CHIR text SYNTAX (that is 32c's parser, which owns print+parse of the LANGUAGE); it is a structured document, the way
// a `.ceir` binary is distinct from `.ceir` text. CEIR-33/D7E RENDERS this schema; both projections lower through the
// SAME CHIR->CEIR path (§180 #13, ADR-0128 D5). Host-only, no std containers.

#pragma once

#include <crd/chir/node.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::chir
{
// The outcome of `read_schema`, in the `ChirParseResult` mold: the first refusal names the document record responsible
// for it. The document is line-oriented (one record per line), so `err_line` is the 1-based line of the offending
// record and `err_col` the 1-based column of its offending token: the malformed field, the column where a missing field
// was expected, or the record keyword for a check that needs the whole graph (an edge's pins, single-writer, an
// orphaned layout row). `node` is the index of the CHIR node the record declares or names (the endpoint whose pin is
// wrong, the consumer of a doubled in-pin), or kInvalidNode when the record names none that exists (the header, an
// unknown record, an out-of-range reference, an orphaned layout row). A node record refused past its index names the
// node it declares, which is not in the model. `id` is the stable id of that node (zero when the refused field is the
// id itself), or the id an orphaned layout row could not resolve; read it only when `node` is valid or `orphan` is set.
// A refusal of a non-empty model has no record (line 0). `msg` is a static string literal, empty when ok.
struct SchemaReadResult
{
    bool                        ok       = false;
    crd::u32                    err_line = 0;
    crd::u32                    err_col  = 0;
    crd::u32                    node     = kInvalidNode;
    StableId                    id{};
    bool                        orphan = false; // the refusal is a layout row whose `id` names no node
    crd::containers::StringView msg;
};

// Print `m` to `out` (appended) as the canonical graph-schema document. Deterministic + LF-only. Requires derive_ids().
void print_schema(const SourceModel& m, crd::containers::Array<char>& out);

// Parse a graph-schema document into `out` (which must be empty). A malformed document is refused with the record that
// caused it (graceful reject: callers discard the partial model, the parse_chir convention). Every record's fields sit
// on the record's own line; a field on a later line is a missing field, and a token after the last field is refused.
// The inverse of print_schema: print_schema(read_schema(text)) == text for a canonical input.
[[nodiscard]] SchemaReadResult read_schema(crd::containers::StringView text, SourceModel& out);

} // namespace crd::chir
