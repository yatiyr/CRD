#pragma once

namespace crd::hesap::iterative
{
// Anchor: the cli_register_iterative.cpp TU registers hesap.iterative.* via a
// static-init hook; a consumer references this to force the .obj to link
// (MSVC drops static-only .objs from a .lib). See feedback_static_lib_anchor_symbol.
void register_iterative_cli_anchor() noexcept;

} // namespace crd::hesap::iterative
