#include <crd/ceir/gpu/coopvec_mlp.hpp>

#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

#include <crd/math/float_convert.hpp> // f32_to_f16_bits

namespace crd::ceir::gpu
{
namespace
{
[[nodiscard]] TypeId shape_of(const Context& ctx, TypeId t) noexcept
{
    const Type tt = ctx.type_of(t);
    return tt.members.size() >= 2U ? tt.members[1] : TypeId{};
}
// The static extent of tensor `t`'s dim `axis`, or 0 if absent / dynamic.
[[nodiscard]] crd::u32 dim_ext(const Context& ctx, TypeId t, usize axis) noexcept
{
    const Type sh = ctx.type_of(shape_of(ctx, t));
    if (axis >= sh.members.size()) { return 0U; }
    const Type d = ctx.type_of(sh.members[axis]);
    return static_cast<DimKind>(d.cols) == DimKind::Static ? d.count : 0U;
}
} // namespace

kir::neural::CoopVecMlpConfig coopvec_config_from_mlp(const Context& ctx, const Operation* op)
{
    kir::neural::CoopVecMlpConfig cfg;
    if (op == nullptr || op->num_operands() < 3U || op->num_results() == 0U) { return cfg; }
    const u32 nw = op->num_operands() - 1U; // weight matrices; matmuls = nw = hidden_layers + 1
    // in_dim = W_1.dim0; hidden = W_1.dim1; out_dim = W_n.dim1; hidden_layers = nw - 1.
    cfg.in_dim        = static_cast<int>(dim_ext(ctx, op->operand(1U)->type(), 0U));
    cfg.hidden        = static_cast<int>(dim_ext(ctx, op->operand(1U)->type(), 1U));
    cfg.out_dim       = static_cast<int>(dim_ext(ctx, op->operand(nw)->type(), 1U));
    cfg.hidden_layers = static_cast<int>(nw) - 1;
    return cfg;
}

bool coopvec_weights_from_mlp(const kir::neural::CoopVecMlpConfig& cfg, const float* const* weights, crd::u16* w_out)
{
    if (weights == nullptr || w_out == nullptr || !cfg.valid()) { return false; }
    int woff = 0;
    for (int l = 0; l < cfg.layers(); ++l)
    {
        int rows = 0; // coopvec output dim of this layer
        int cols = 0; // coopvec input dim of this layer
        kir::neural::coopvec_layer_dims(cfg, l, rows, cols);
        const float* ml_w = weights[l]; // ml.mlp W_{l+1} : ROW-MAJOR [in=cols, out=rows] (x·W), so ml_w[k*rows + r]
        if (ml_w == nullptr) { return false; }
        for (int r = 0; r < rows; ++r)
        {
            for (int k = 0; k < cols; ++k)
            {
                // coopvec W[out=r, in=k] (RowMajor [rows, cols]) = the TRANSPOSE of ml.mlp W[in=k, out=r].
                w_out[woff + r * cols + k] = crd::math::f32_to_f16_bits(ml_w[k * rows + r]);
            }
        }
        woff += rows * cols;
    }
    return true;
}
} // namespace crd::ceir::gpu
