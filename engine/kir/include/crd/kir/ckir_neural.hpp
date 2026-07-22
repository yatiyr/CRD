#pragma once

// ckir_neural.hpp — B10: the CKIR NEURAL-SHADING moat. A PER-INVOCATION small-MLP evaluator on `VK_NV_cooperative_vector` — each
// thread/pixel runs its OWN multi-layer perceptron inline on the tensor units (the RTX Neural Shading substrate: neural materials,
// neural textures, learned BRDFs evaluated per pixel). This is the counterpart to the coopmat fused MLP (`ckir_mlp.hpp`): coopmat
// is a WORKGROUP-scoped GEMM (one big matrix multiply the whole workgroup cooperates on — batched inference); coopvec is
// PER-INVOCATION matrix×vector (every invocation independent), which is exactly what a per-pixel neural shader needs.
//
// Like the coopmat GEMM (`emit_contract_coopmat2_glsl`) and the fused MLP, the cooperative-vector matmul is ONE opaque typed op,
// so the kernel is emitted as a config-keyed whole-kernel GLSL template (dims baked as literals ⇒ the driver specializes), not
// built from scalar KStmts. fp16 weights + fp16 activations + fp16 result — the ONLY fp16-input matmul combination the hardware
// supports (`vkGetPhysicalDeviceCooperativeVectorPropertiesNV`; the others are int8/fp8 quantized). ReLU hidden activations,
// linear output. Matched-accuracy vs the CPU reference (NOT bit-exact — tensor cores reorder the accumulation; the FP32-precise
// CKIR tier owns bit-exactness). C6 (`cooperative_vector()`) is the device prerequisite.

#include <crd/kir/ckir_glsl.hpp>       // GlslKernel carrier + glsl_detail::app_uint (int → GLSL literal)
#include <crd/math/cmath.hpp>          // crd::math::sin/cos for the shared uv encoding
#include <crd/math/float_convert.hpp> // f32<->f16 bit conversion for the reference

namespace crd::kir::neural
{

namespace neural_detail
{
inline void u(crd::containers::String& s, int v) { glsl_detail::app_uint(s, v); }
} // namespace neural_detail

// A uniform-hidden MLP: in_dim → (hidden ×hidden_layers) → out_dim, ReLU on the hidden layers, linear output. All fp16. Dims are
// arbitrary (up to the device's max cooperative-vector components, 1024 on Ada) — no tile constraint like coopmat's 16-multiple.
struct CoopVecMlpConfig
{
    int in_dim        = 16;
    int hidden        = 16;
    int out_dim       = 16;
    int hidden_layers = 2; // number of hidden layers; total matmuls = hidden_layers + 1 (the linear output layer)

    [[nodiscard]] bool valid() const noexcept
    {
        return in_dim > 0 && hidden > 0 && out_dim > 0 && hidden_layers >= 1 && in_dim <= 1024 && hidden <= 1024 && out_dim <= 1024;
    }
    [[nodiscard]] int layers() const noexcept { return hidden_layers + 1; }
    // concatenated weights (fp16 elements): W0[hidden×in_dim] · W1..W(hl-1)[hidden×hidden] · Wout[out_dim×hidden]
    [[nodiscard]] int weight_count() const noexcept
    {
        int c = hidden * in_dim;
        for (int l = 1; l < hidden_layers; ++l) { c += hidden * hidden; }
        return c + out_dim * hidden;
    }
    [[nodiscard]] int bias_count() const noexcept { return hidden * hidden_layers + out_dim; }
};

// (rows, cols) = (output dim, input dim) of the RowMajor weight matrix for matmul `l` (0-based).
inline void coopvec_layer_dims(const CoopVecMlpConfig& c, int l, int& rows, int& cols) noexcept
{
    if (l == 0) { rows = c.hidden; cols = c.in_dim; }
    else if (l < c.hidden_layers) { rows = c.hidden; cols = c.hidden; }
    else { rows = c.out_dim; cols = c.hidden; } // the linear output layer
}

// Emit the per-invocation coopvec MLP as GLSL (`GL_NV_cooperative_vector`). Bindings: 0=In(N×in_dim fp16), 1=W(concat fp16),
// 2=B(concat fp16), 3=Out(N×out_dim fp16), 4=Cfg(uint[0]=N). One thread per sample. Byte offsets: coopvec load/store + matrix
// offset/stride are in BYTES (fp16 = 2). Returns false on an invalid config.
inline bool emit_coopvec_mlp_glsl(const CoopVecMlpConfig& c, GlslKernel& out)
{
    if (!c.valid()) { return false; }
    using neural_detail::u;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 460\n");
    s.append("#extension GL_NV_cooperative_vector : require\n");
    s.append("#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n");
    s.append("#extension GL_EXT_shader_explicit_arithmetic_types : require\n");
    s.append("layout(local_size_x = 64) in;\n");
    s.append("layout(std430, binding = 0) readonly  buffer BIn  { float16_t Inb[]; };\n");
    s.append("layout(std430, binding = 1) readonly  buffer BW   { float16_t Wb[]; };\n");
    s.append("layout(std430, binding = 2) readonly  buffer BB   { float16_t Bb[]; };\n");
    s.append("layout(std430, binding = 3) writeonly buffer BOut { float16_t Outb[]; };\n");
    s.append("layout(std430, binding = 4) readonly  buffer BCfg { uint Cfg[]; };\n");
    s.append("void main() {\n");
    s.append("  uint tid = gl_GlobalInvocationID.x;\n");
    s.append("  if (tid >= Cfg[0]) { return; }\n");
    s.append("  const int F16 = gl_ComponentTypeFloat16NV;\n");
    s.append("  const int RM = gl_CooperativeVectorMatrixLayoutRowMajorNV;\n");
    s.append("  coopvecNV<float16_t, ");
    u(s, c.in_dim);
    s.append("> a0;\n  coopVecLoadNV(a0, Inb, tid * ");
    u(s, c.in_dim);
    s.append("u * 2u);\n");

    int woff = 0; // fp16-element offsets into the concatenated weight / bias buffers
    int boff = 0;
    for (int l = 0; l < c.layers(); ++l)
    {
        int rows = 0;
        int cols = 0;
        coopvec_layer_dims(c, l, rows, cols);
        s.append("  coopvecNV<float16_t, ");
        u(s, rows);
        s.append("> a");
        u(s, l + 1);
        s.append(";\n  coopVecMatMulAddNV(a");
        u(s, l + 1);
        s.append(", a");
        u(s, l);
        s.append(", F16, Wb, ");
        u(s, woff * 2);
        s.append("u, F16, Bb, ");
        u(s, boff * 2);
        s.append("u, F16, ");
        u(s, rows);
        s.append("u, ");
        u(s, cols);
        s.append("u, RM, false, ");
        u(s, cols * 2);
        s.append("u);\n");
        if (l + 1 < c.layers()) // ReLU on the hidden layers (component-wise max on the cooperative vector); output stays linear
        {
            s.append("  a");
            u(s, l + 1);
            s.append(" = max(a");
            u(s, l + 1);
            s.append(", coopvecNV<float16_t, ");
            u(s, rows);
            s.append(">(float16_t(0.0)));\n");
        }
        woff += rows * cols;
        boff += rows;
    }
    s.append("  coopVecStoreNV(a");
    u(s, c.layers());
    s.append(", Outb, tid * ");
    u(s, c.out_dim);
    s.append("u * 2u);\n}\n");
    return true;
}

// The SCALAR-FMA BASELINE — the SAME MLP written the way a shader author would by hand: plain per-invocation loops, fp16 storage
// read into fp32 registers, register-accumulated MACs, no cooperative-vector / tensor units. Same 5 bindings + layout as the
// coopvec kernel, so a benchmark can dispatch both over the same data and compare timings (the tensor-core speedup) AND outputs
// (self-verifying — both compute the identical MLP). Two ping-pong local arrays sized to the widest layer. Returns false if invalid.
inline bool emit_scalar_mlp_glsl(const CoopVecMlpConfig& c, GlslKernel& out)
{
    if (!c.valid()) { return false; }
    using neural_detail::u;
    int maxd = c.in_dim;
    if (c.hidden > maxd) { maxd = c.hidden; }
    if (c.out_dim > maxd) { maxd = c.out_dim; }
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 460\n");
    s.append("#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n");
    s.append("layout(local_size_x = 64) in;\n");
    s.append("layout(std430, binding = 0) readonly  buffer BIn  { float16_t Inb[]; };\n");
    s.append("layout(std430, binding = 1) readonly  buffer BW   { float16_t Wb[]; };\n");
    s.append("layout(std430, binding = 2) readonly  buffer BB   { float16_t Bb[]; };\n");
    s.append("layout(std430, binding = 3) writeonly buffer BOut { float16_t Outb[]; };\n");
    s.append("layout(std430, binding = 4) readonly  buffer BCfg { uint Cfg[]; };\n");
    s.append("void main() {\n");
    s.append("  uint tid = gl_GlobalInvocationID.x;\n");
    s.append("  if (tid >= Cfg[0]) { return; }\n");
    s.append("  float a[");
    u(s, maxd);
    s.append("]; float b[");
    u(s, maxd);
    s.append("];\n");
    s.append("  for (int k = 0; k < ");
    u(s, c.in_dim);
    s.append("; ++k) { a[k] = float(Inb[tid * ");
    u(s, c.in_dim);
    s.append("u + uint(k)]); }\n");
    int woff = 0;
    int boff = 0;
    for (int l = 0; l < c.layers(); ++l)
    {
        int rows = 0;
        int cols = 0;
        coopvec_layer_dims(c, l, rows, cols);
        s.append("  for (int r = 0; r < ");
        u(s, rows);
        s.append("; ++r) {\n    float acc = float(Bb[");
        u(s, boff);
        s.append("u + uint(r)]);\n    for (int k = 0; k < ");
        u(s, cols);
        s.append("; ++k) { acc += float(Wb[");
        u(s, woff);
        s.append("u + uint(r) * ");
        u(s, cols);
        s.append("u + uint(k)]) * a[k]; }\n");
        // round through fp16 to match the coopvec kernel's fp16 activation store, then ReLU on the hidden layers.
        s.append("    float v = float(float16_t(acc));\n");
        if (l + 1 < c.layers()) { s.append("    if (v < 0.0) { v = 0.0; }\n"); }
        s.append("    b[r] = v;\n  }\n");
        s.append("  for (int i = 0; i < ");
        u(s, rows);
        s.append("; ++i) { a[i] = b[i]; }\n");
        woff += rows * cols;
        boff += rows;
    }
    s.append("  for (int o = 0; o < ");
    u(s, c.out_dim);
    s.append("; ++o) { Outb[tid * ");
    u(s, c.out_dim);
    s.append("u + uint(o)] = float16_t(a[o]); }\n}\n");
    return true;
}

// The CPU REFERENCE — matched-accuracy fp16 model of the device MLP. Weights/biases/inputs are fp16 bits (u16). Per layer:
// accumulate in fp32 (bias first, then the RowMajor matmul over the f16-rounded operands), ROUND the layer output to fp16 (the
// device stores fp16 activations between layers), then ReLU on the hidden layers. `out` receives N×out_dim fp16 bits. Dims ≤ 1024.
inline void eval_coopvec_mlp_cpu(const CoopVecMlpConfig& c, const crd::u16* w, const crd::u16* b, const crd::u16* in, int n, crd::u16* out) noexcept
{
    float buf_a[1024];
    float buf_b[1024];
    for (int sample = 0; sample < n; ++sample)
    {
        float* cur = &buf_a[0];
        float* nxt = &buf_b[0];
        for (int i = 0; i < c.in_dim; ++i) { cur[i] = crd::math::f16_bits_to_f32(in[sample * c.in_dim + i]); }
        int woff = 0;
        int boff = 0;
        for (int l = 0; l < c.layers(); ++l)
        {
            int rows = 0;
            int cols = 0;
            coopvec_layer_dims(c, l, rows, cols);
            for (int r = 0; r < rows; ++r)
            {
                float acc = crd::math::f16_bits_to_f32(b[boff + r]);
                for (int k = 0; k < cols; ++k) { acc += crd::math::f16_bits_to_f32(w[woff + r * cols + k]) * cur[k]; }
                float v = crd::math::f16_bits_to_f32(crd::math::f32_to_f16_bits(acc)); // fp16 activation store
                if (l + 1 < c.layers() && v < 0.0F) { v = 0.0F; }                      // ReLU (hidden only)
                nxt[r] = v;
            }
            woff += rows * cols;
            boff += rows;
            float* tmp = cur;
            cur        = nxt;
            nxt        = tmp;
        }
        for (int o = 0; o < c.out_dim; ++o) { out[sample * c.out_dim + o] = crd::math::f32_to_f16_bits(cur[o]); }
    }
}

// ── ON-DEVICE DIFFERENTIABLE TRAINING (the moat's defining claim) ──────────────────────────────────────────────────────────
// Emit a TRAINING-STEP kernel for a single `dim`×`dim` linear layer y = W·x + b: one invocation per sample runs the forward on
// the tensor units, forms the loss gradient δ = 2(y − t), and accumulates the WEIGHT gradient (δ ⊗ x) and BIAS gradient (Σδ) into
// device buffers using the HARDWARE cooperative-vector training ops — `coopVecOuterProductAccumulateNV` (the weight-gradient outer
// product; its output MUST be a TrainingOptimal-layout matrix) + `coopVecReduceSumAccumulateNV` (the bias gradient). The whole
// backprop — the expensive part — runs on the GPU tensor path. Bindings: 0=X(N×dim in), 1=T(N×dim target), 2=W(dim×dim RowMajor),
// 3=B(dim bias), 4=GW(TrainingOptimal weight-grad, accumulated), 5=GB(bias-grad, accumulated), 6=Y(N×dim forward out, for the
// loss), 7=Cfg(uint[0]=N). The host converts the TrainingOptimal GW → RowMajor (`vkConvertCooperativeVectorMatrixNV`) and applies
// SGD; a multi-layer MLP composes this per layer with the transposed-matmul deltas. `dim` ≤ 1024.
inline bool emit_coopvec_linear_train_glsl(int dim, GlslKernel& out)
{
    if (dim <= 0 || dim > 1024) { return false; }
    using neural_detail::u;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 460\n");
    s.append("#extension GL_NV_cooperative_vector : require\n");
    s.append("#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n");
    s.append("#extension GL_EXT_shader_explicit_arithmetic_types : require\n");
    s.append("layout(local_size_x = 64) in;\n");
    s.append("layout(std430, binding = 0) readonly  buffer BX   { float16_t Xb[]; };\n");
    s.append("layout(std430, binding = 1) readonly  buffer BT   { float16_t Tb[]; };\n");
    s.append("layout(std430, binding = 2) readonly  buffer BW   { float16_t Wb[]; };\n");
    s.append("layout(std430, binding = 3) readonly  buffer BB   { float16_t Bb[]; };\n");
    s.append("layout(std430, binding = 4)          buffer BGW  { float16_t GWb[]; };\n");
    s.append("layout(std430, binding = 5)          buffer BGB  { float16_t GBb[]; };\n");
    s.append("layout(std430, binding = 6) writeonly buffer BY   { float16_t Yb[]; };\n");
    s.append("layout(std430, binding = 7) readonly  buffer BCfg { uint Cfg[]; };\n");
    s.append("void main() {\n");
    s.append("  uint tid = gl_GlobalInvocationID.x;\n");
    s.append("  if (tid >= Cfg[0]) { return; }\n");
    s.append("  const int F16 = gl_ComponentTypeFloat16NV;\n");
    s.append("  const int RM = gl_CooperativeVectorMatrixLayoutRowMajorNV;\n");
    s.append("  const int TO = gl_CooperativeVectorMatrixLayoutTrainingOptimalNV;\n");
    s.append("  const uint D = ");
    u(s, dim);
    s.append("u;\n");
    s.append("  coopvecNV<float16_t, ");
    u(s, dim);
    s.append("> x; coopVecLoadNV(x, Xb, tid * D * 2u);\n");
    s.append("  coopvecNV<float16_t, ");
    u(s, dim);
    s.append("> y; coopVecMatMulAddNV(y, x, F16, Wb, 0u, F16, Bb, 0u, F16, D, D, RM, false, D * 2u);\n");
    s.append("  coopVecStoreNV(y, Yb, tid * D * 2u);\n");
    s.append("  coopvecNV<float16_t, ");
    u(s, dim);
    s.append("> t; coopVecLoadNV(t, Tb, tid * D * 2u);\n");
    s.append("  coopvecNV<float16_t, ");
    u(s, dim);
    s.append("> d = (y - t) * coopvecNV<float16_t, ");
    u(s, dim);
    s.append(">(float16_t(2.0));\n");
    s.append("  coopVecOuterProductAccumulateNV(d, x, GWb, 0u, 0u, TO, F16);\n"); // dW += d (x) x
    s.append("  coopVecReduceSumAccumulateNV(d, GBb, 0u);\n");                    // db += d
    s.append("}\n");
    return true;
}

// ── NEURAL MATERIAL / 2-D NEURAL FIELD (a learned texture) ─────────────────────────────────────────────────────────────────
// A frequency (positional) encoding of a uv coordinate into `dim` features — the standard fix for the MLP's spectral bias so a
// small net can represent detail (NeRF/Instant-NGP style). `dim` must be a multiple of 4: band k (k=0..dim/4-1, frequency 2^k·π)
// contributes [sin(f·u), cos(f·u), sin(f·v), cos(f·v)]. Shared by the CPU trainer and the GLSL render kernel so they agree.
inline void neural_uv_encode(float u, float v, int dim, float* feat) noexcept
{
    const int bands = dim / 4;
    for (int k = 0; k < bands; ++k)
    {
        const float f    = static_cast<float>(1 << k) * 3.14159265358979324F;
        feat[4 * k + 0] = crd::math::sin(f * u);
        feat[4 * k + 1] = crd::math::cos(f * u);
        feat[4 * k + 2] = crd::math::sin(f * v);
        feat[4 * k + 3] = crd::math::cos(f * v);
    }
}

// Emit the NEURAL MATERIAL RENDER kernel: one invocation per pixel computes its uv → the frequency encoding (inline, into the
// input cooperative vector via component assignment) → the MLP (coopvec) → clamps the first 3 outputs to an RGBA8 pixel. A single
// fused pass on the tensor units (no feature round-trip through memory). Bindings: 0=W(concat fp16), 1=B(concat fp16), 2=Out(uint
// RGBA8, W×H), 3=Cfg(uint[0]=W, [1]=H). Requires in_dim % 4 == 0 and out_dim ≥ 3. 8×8 workgroup tiles.
inline bool emit_neural_material_render_glsl(const CoopVecMlpConfig& c, GlslKernel& out)
{
    if (!c.valid() || (c.in_dim % 4) != 0 || c.out_dim < 3) { return false; }
    using neural_detail::u;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 460\n");
    s.append("#extension GL_NV_cooperative_vector : require\n");
    s.append("#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n");
    s.append("#extension GL_EXT_shader_explicit_arithmetic_types : require\n");
    s.append("layout(local_size_x = 8, local_size_y = 8) in;\n");
    s.append("layout(std430, binding = 0) readonly  buffer BW   { float16_t Wb[]; };\n");
    s.append("layout(std430, binding = 1) readonly  buffer BB   { float16_t Bb[]; };\n");
    s.append("layout(std430, binding = 2) writeonly buffer BOut { uint Outb[]; };\n");
    s.append("layout(std430, binding = 3) readonly  buffer BCfg { uint Cfg[]; };\n");
    s.append("void main() {\n");
    s.append("  uint px = gl_GlobalInvocationID.x; uint py = gl_GlobalInvocationID.y;\n");
    s.append("  uint W = Cfg[0]; uint H = Cfg[1];\n");
    s.append("  if (px >= W || py >= H) { return; }\n");
    s.append("  const int F16 = gl_ComponentTypeFloat16NV;\n");
    s.append("  const int RM = gl_CooperativeVectorMatrixLayoutRowMajorNV;\n");
    s.append("  float uu = (float(px) + 0.5) / float(W);\n  float vv = (float(py) + 0.5) / float(H);\n");
    s.append("  coopvecNV<float16_t, ");
    u(s, c.in_dim);
    s.append("> a0;\n  for (int k = 0; k < ");
    u(s, c.in_dim / 4);
    s.append("; ++k) {\n    float f = float(1 << k) * 3.14159265;\n");
    s.append("    a0[4*k+0] = float16_t(sin(f*uu)); a0[4*k+1] = float16_t(cos(f*uu));\n");
    s.append("    a0[4*k+2] = float16_t(sin(f*vv)); a0[4*k+3] = float16_t(cos(f*vv));\n  }\n");
    int woff = 0;
    int boff = 0;
    for (int l = 0; l < c.layers(); ++l)
    {
        int rows = 0;
        int cols = 0;
        coopvec_layer_dims(c, l, rows, cols);
        s.append("  coopvecNV<float16_t, ");
        u(s, rows);
        s.append("> a");
        u(s, l + 1);
        s.append(";\n  coopVecMatMulAddNV(a");
        u(s, l + 1);
        s.append(", a");
        u(s, l);
        s.append(", F16, Wb, ");
        u(s, woff * 2);
        s.append("u, F16, Bb, ");
        u(s, boff * 2);
        s.append("u, F16, ");
        u(s, rows);
        s.append("u, ");
        u(s, cols);
        s.append("u, RM, false, ");
        u(s, cols * 2);
        s.append("u);\n");
        if (l + 1 < c.layers())
        {
            s.append("  a");
            u(s, l + 1);
            s.append(" = max(a");
            u(s, l + 1);
            s.append(", coopvecNV<float16_t, ");
            u(s, rows);
            s.append(">(float16_t(0.0)));\n");
        }
        woff += rows * cols;
        boff += rows;
    }
    s.append("  vec3 col = clamp(vec3(float(a");
    u(s, c.layers());
    s.append("[0]), float(a");
    u(s, c.layers());
    s.append("[1]), float(a");
    u(s, c.layers());
    s.append("[2])), 0.0, 1.0);\n");
    s.append("  Outb[py * W + px] = packUnorm4x8(vec4(col, 1.0));\n}\n");
    return true;
}

} // namespace crd::kir::neural
