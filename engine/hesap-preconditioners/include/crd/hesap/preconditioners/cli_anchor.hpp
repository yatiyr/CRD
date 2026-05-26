#pragma once

namespace crd::hesap::preconditioners
{
// Anchor: cli_register_preconditioners.cpp registers hesap.precond.* +
// hesap.iterative.pcg.* via a static-init hook; a consumer references this to
// force the .obj to link. See feedback_static_lib_anchor_symbol.
void register_preconditioners_cli_anchor() noexcept;

} // namespace crd::hesap::preconditioners
