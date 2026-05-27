#pragma once

namespace crd::hesap::amg
{
// Anchor: cli_register_amg.cpp registers hesap.amg.* via a static-init hook; a
// consumer references this to force the .obj to link (MSVC drops static-only
// .objs from a .lib). See feedback_static_lib_anchor_symbol.
void register_amg_cli_anchor() noexcept;

} // namespace crd::hesap::amg
