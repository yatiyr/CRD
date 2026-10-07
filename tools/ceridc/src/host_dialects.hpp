#pragma once

// host_dialects.hpp — the CEIR dialects ceridc registers wherever it loads an authored program: the inspect verb's
// executor and the diag service's program.provenance command. One set, so both answer for the same ops.

namespace crd::ceir
{
class Context;
} // namespace crd::ceir

namespace crd::ceridc
{
// The dialects the CEIR host executors run (the compiled plan's scalar host subset). A Registrar: `user` is unused.
void register_host_dialects(crd::ceir::Context& ctx, void* user);
} // namespace crd::ceridc
