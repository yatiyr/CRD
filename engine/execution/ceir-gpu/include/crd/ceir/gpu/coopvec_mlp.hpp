#pragma once

// crd-ceir-gpu — the COOPVEC native MLP provider's CLAIM CONVERSION (CEIR-24c). When the §69 partitioner assigns an ml.mlp to the
// coopvec provider (coopvec_can_claim_mlp), THIS turns the CEIR op into the cooperative-vector kernel's inputs: (1) the config
// (dims) from the op's operand/result TYPES; (2) the concatenated fp16 weight buffer from the caller's f32 weights — TRANSPOSED
// (ml.mlp is x·W with W[in,out]; the coopvec matmul is W·x with W[out,in] — see eval_coopvec_mlp_cpu) + f32→f16; the bias is ZERO
// (the ml.mlp dialect has no bias — a zero bias is contract-clean, `b + Σ` = `Σ`). ⛔ DEVICE-FREE: pure shape/array math (no
// gpu-context) — the device dispatch (emit_coopvec_mlp_glsl) is 24c-2b. The whole conversion is proven vs the f32 MLP oracle
// through eval_coopvec_mlp_cpu (24c-2a), so the on-device leg only has to match the CPU coopvec reference.

#include <crd/ceir/context.hpp>
#include <crd/ceir/id.hpp>

#include <crd/kir/ckir_neural.hpp> // CoopVecMlpConfig / eval_coopvec_mlp_cpu

namespace crd::ceir::gpu
{
// Extract the coopvec MLP config (in_dim / hidden / out_dim / hidden_layers) from an ml.mlp op's operand + result TYPES. ⛔ the op
// MUST satisfy coopvec_can_claim_mlp (>=2 weights, uniform hidden, static dims) — else the returned config may be `!valid()`
// (the caller checks). Device-free.
[[nodiscard]] kir::neural::CoopVecMlpConfig coopvec_config_from_mlp(const Context& ctx, const Operation* op);

// Convert ml.mlp's per-layer ROW-MAJOR f32 weights (`weights[l]` = W_{l+1}[D_l, D_{l+1}] in the x·W layout) into the coopvec
// CONCATENATED fp16 weight buffer (each layer TRANSPOSED to RowMajor [out, in] = the W·x layout, f32→f16). `weights` has
// `cfg.layers()` pointers; `w_out` receives `cfg.weight_count()` fp16 elements. Returns false on nullptr / invalid cfg. Device-free.
[[nodiscard]] bool coopvec_weights_from_mlp(const kir::neural::CoopVecMlpConfig& cfg, const float* const* weights, crd::u16* w_out);
} // namespace crd::ceir::gpu
