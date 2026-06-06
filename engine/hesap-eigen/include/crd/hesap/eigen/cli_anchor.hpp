#pragma once

namespace crd::hesap::eigen
{
// Anchor: cli_register_eigen.cpp registers hesap.eigen.* via a static-init hook; a consumer references this to
// force the .obj to link (MSVC drops static-only .objs from a .lib). See feedback_static_lib_anchor_symbol.
void register_eigen_cli_anchor() noexcept;

} // namespace crd::hesap::eigen
