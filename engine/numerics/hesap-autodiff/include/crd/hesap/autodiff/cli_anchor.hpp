#pragma once

// cli_anchor.hpp — v15-z: the anchor symbol that forces the hesap.ad.* CLI registration TU
// (src/cli_register_autodiff.cpp) to survive the static-library link. A consumer that wants the forward-AD CLI
// commands calls `register_autodiff_cli_anchor()` once (e.g. `const bool k = (register_autodiff_cli_anchor(), true);`).
// ADR-0097.

namespace crd::hesap::autodiff
{
void register_autodiff_cli_anchor() noexcept;
} // namespace crd::hesap::autodiff
