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

namespace crd::chir
{
// Print `m` to `out` (appended) as the canonical graph-schema document. Deterministic + LF-only. Requires derive_ids().
void print_schema(const SourceModel& m, crd::containers::Array<char>& out);

// Parse a graph-schema document into `out` (which must be empty). Returns false on a malformed document (graceful reject
// — never a partial model). The inverse of print_schema: print_schema(read_schema(text)) == text for a canonical input.
[[nodiscard]] bool read_schema(crd::containers::StringView text, SourceModel& out);

} // namespace crd::chir
