#pragma once

// crd-ceir — CEIR (Cerid Execution IR), ADR-0109. The host-only execution/orchestration IR: typed SSA + graph/CFG/
// structured regions (mission §2/§12/§13). This umbrella pulls the CEIR-1a core (identity + the IR graph + the
// Context factory). Later slices add the dialect registry (1d), the schema generator (CEIR-2), the type system
// (CEIR-3), effects (CEIR-4), and so on — each behind its own header.
//
// ⛔ crd-ceir is HOST-ONLY (ADR-0109 §4): it links crd-core/log/memory/containers/units and NOTHING else — no
// GPU, no kir, no render-graph, no jobs (those are reached through the dependency-inversion provider bridges).

#include <crd/ceir/attr.hpp>
#include <crd/ceir/binary.hpp>
#include <crd/ceir/builder.hpp>
#include <crd/ceir/context.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/id.hpp>
#include <crd/ceir/interface.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/parse.hpp>
#include <crd/ceir/print.hpp>
#include <crd/ceir/symbol_table.hpp>
