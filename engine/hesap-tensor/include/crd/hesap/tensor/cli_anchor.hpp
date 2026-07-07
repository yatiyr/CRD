#pragma once

// cli_anchor.hpp — v14-z: pull this from any TU that needs the hesap.tensor.* CLI registration block to survive the
// static-lib link (the hesap.fft / hesap.ode / hesap.interp CLI pattern). ADR-0095 / ADR-0096.
//
// NOTE (link contract): the registration TU compiled into crd-hesap-tensor consumes the header-only einsum-exec /
// batched / decomp / tt / nn / io surfaces, so a consumer that pulls THIS anchor must link crd-hesap,
// crd-hesap-dense and crd-platform (exactly like the einsum/decomp/tt/io/nn test executables). Consumers that do
// NOT pull the anchor never drag those edges — the smoke_hesap_tensor link-isolation gate stays intact.

namespace crd::hesap::tensor
{
void register_tensor_cli_anchor() noexcept;
} // namespace crd::hesap::tensor
