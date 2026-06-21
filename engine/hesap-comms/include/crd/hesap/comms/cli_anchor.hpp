#pragma once

// cli_anchor.hpp — v11-z: pull this from any TU that needs the hesap.comms.* CLI registration block to survive the
// static-lib link (the v6/v7/v9/v10 pattern).

namespace crd::hesap::comms
{
void register_comms_cli_anchor() noexcept;
} // namespace crd::hesap::comms
