#pragma once

// cli_anchor.hpp — v9-z: pull this from any TU that needs the hesap.ode.* CLI registration block to survive
// the static-lib link (the v6/v7 hesap.eigen / hesap.opt pattern). ADR-0091.

namespace crd::hesap::ode
{
void register_ode_cli_anchor() noexcept;
} // namespace crd::hesap::ode
