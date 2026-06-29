#pragma once

// cli_anchor.hpp — v12-z: pull this from any TU that needs the hesap.special.* CLI registration block to survive the
// static-lib link (the v6/v7/v9/v10/v11 hesap.* pattern).

namespace crd::hesap::special
{
void register_special_cli_anchor() noexcept;
} // namespace crd::hesap::special
