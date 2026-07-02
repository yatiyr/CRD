#pragma once

// cli_anchor.hpp — v13-z: pull this from any TU that needs the hesap.diff.* CLI registration block to survive the
// static-lib link (the hesap.fft / hesap.ode CLI pattern). ADR-0095.

namespace crd::hesap::diff
{
void register_diff_cli_anchor() noexcept;
} // namespace crd::hesap::diff
