#pragma once

namespace crd::hesap::direct
{
// CLI register anchor (ADR-0081 §7; see hesap-sparse cli_anchor for rationale).
// Reference this from a downstream TU to force the linker to keep
// cli_register_direct.cpp's static-init registration block (a static lib drops
// .obj files whose only contents are static-init side effects otherwise).
void register_direct_cli_anchor() noexcept;
} // namespace crd::hesap::direct
