#pragma once

// cli_anchor.hpp — v11-z: pull this from any TU that needs the hesap.dsp.* CLI registration block to survive the
// static-lib link (the v6/v7/v9/v10 hesap.{eigen,opt,ode,fft} pattern).

namespace crd::hesap::dsp
{
void register_dsp_cli_anchor() noexcept;
} // namespace crd::hesap::dsp
