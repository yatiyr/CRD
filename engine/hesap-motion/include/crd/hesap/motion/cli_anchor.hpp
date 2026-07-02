#pragma once

// cli_anchor.hpp — v13-z: pull this from any TU that needs the hesap.motion.* CLI registration block to survive the
// static-lib link (the hesap.fft / hesap.ode CLI pattern). ADR-0095.

namespace crd::hesap::motion
{
void register_motion_cli_anchor() noexcept;
} // namespace crd::hesap::motion
