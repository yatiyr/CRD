#pragma once

namespace crd::hesap::ordering
{
// CLI register anchor (ADR-0081 §7; see hesap-sparse cli_anchor for rationale).
// Reference this from a downstream TU to force the linker to keep
// cli_register_ordering.cpp's static-init registration block.
void register_ordering_cli_anchor() noexcept;
} // namespace crd::hesap::ordering
