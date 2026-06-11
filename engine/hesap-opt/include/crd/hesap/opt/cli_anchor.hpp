#pragma once

// cli_anchor.hpp — v7-z: pull this from any TU that needs the hesap.opt.* CLI registration block to survive
// the static-lib link (the v6 hesap.eigen pattern).

namespace crd::hesap::opt
{
void register_opt_cli_anchor() noexcept;
} // namespace crd::hesap::opt
