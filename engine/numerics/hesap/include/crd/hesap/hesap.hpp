#pragma once

// Umbrella header for crd-hesap substrate (Phase 3.1.6 v0a).
//
// Includes every public type from the substrate module:
//   - Complex<T> + Complex32 / Complex64
//   - LinearOp<T>
//   - MatrixId / VectorId
//   - 'HDV0' CRDR FourCC pin
//   - CLI protocol scaffolding (CommandSchema / CommandResult /
//     CommandRegistry + JSON writer + MCP-descriptor emit)

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/cli/command_schema.hpp>
#include <crd/hesap/cli/json_writer.hpp>
#include <crd/hesap/cli/mcp_descriptor.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/crdr_format.hpp>
#include <crd/hesap/handles.hpp>
#include <crd/hesap/linear_op.hpp>
