#pragma once

// crd-ceir — the deterministic canonical PRINTER (CEIR-1e, §10/§166). IR → text. The text is the canonical SEMANTIC
// model (ops / values / regions / symbols); graph layout (coords/edges) is UI and lives elsewhere — the printer emits
// no layout. Output is DETERMINISTIC: SSA values are numbered by a fixed pre-order walk, attributes are emitted in
// name order, so the same semantic graph always prints byte-identical text. Round-trips with `parse` (parse.hpp):
// print(parse(print(x))) == print(x). MLIR-flavored:
//
//     module {
//     ^bb0(%0 : !t1):
//       %1 = test.add(%0, %0) {tag = 7} : !t1
//       func.func() {sym_name = "f"} {
//       ^bb0:
//         func.return()
//       }
//     }

#include <crd/ceir/context.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir
{
// Append `module`'s canonical text to `out`.
void print(Context& ctx, const Module& module, containers::String& out);

// Print `module` to a fresh String allocated from `alloc`.
[[nodiscard]] containers::String print(Context& ctx, const Module& module, memory::IAllocator* alloc);
} // namespace crd::ceir
