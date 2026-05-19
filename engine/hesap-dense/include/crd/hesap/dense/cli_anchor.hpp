#pragma once

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// CLI register anchor — call from a downstream TU (test / smoke / runtime)
// to force the linker to pull in `cli_register.cpp.obj`, whose static-init
// block registers the BLAS L1 CLI commands.
//
// Why this is needed: MSVC's linker drops object files from a static
// library when none of their exported symbols are referenced. Static-init
// blocks alone do NOT count as referenced — the .obj is dropped, and the
// static-init never runs. The anchor symbol is a one-liner that gives
// downstream code something concrete to reference. ADR-0081 §7 — every
// module that ships CLI registrations exports such an anchor.
// -----------------------------------------------------------------------
void register_blas1_cli_anchor() noexcept;
void register_blas2_cli_anchor() noexcept;
void register_blas3_cli_anchor() noexcept;
} // namespace crd::hesap::dense
