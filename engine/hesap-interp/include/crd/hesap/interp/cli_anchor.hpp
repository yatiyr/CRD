#pragma once

// cli_anchor.hpp — v13-z: pull this from any TU that needs the hesap.interp.* CLI registration block to survive the
// static-lib link (the hesap.fft / hesap.ode / hesap.opt CLI pattern). ADR-0095.

namespace crd::hesap::interp
{
void register_interp_cli_anchor() noexcept;
} // namespace crd::hesap::interp
