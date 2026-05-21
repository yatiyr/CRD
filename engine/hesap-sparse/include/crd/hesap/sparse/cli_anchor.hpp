#pragma once

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// CLI register anchor (ADR-0081 §7). Call from a downstream TU (test /
// smoke / runtime) to force the linker to pull in cli_register_sparse.cpp's
// object, whose static-init block registers the v1a-2 sparse CLI commands.
// MSVC drops static-only .obj files from a .lib unless an exported symbol is
// referenced; this one-liner is that reference.
// -----------------------------------------------------------------------
void register_sparse_cli_anchor() noexcept;
} // namespace crd::hesap::sparse
