#pragma once

// cli_anchor.hpp — v10-z: pull this from any TU that needs the hesap.fft.* CLI registration block to survive the
// static-lib link (the v6/v7/v9 hesap.eigen / hesap.opt / hesap.ode pattern). ADR for the FFT cluster (v10).

namespace crd::hesap::fft
{
void register_fft_cli_anchor() noexcept;
} // namespace crd::hesap::fft
