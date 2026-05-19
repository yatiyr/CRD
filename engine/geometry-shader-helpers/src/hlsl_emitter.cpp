// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — HLSL backend implementation.
// Phase 3.1.7 v9e-c (2026-05-19).
//
// Mirror of `glsl_emitter.cpp`. Same IR walker, HLSL 6.0 syntax. Single
// source of structural truth (the IR) and proof-by-induction equivalence
// with the GLSL backend (whose math is line-for-line identical).
// ---------------------------------------------------------------------------

#include <crd/geometry/shader_helpers/hlsl_emitter.hpp>

#include <crd/core/assert.hpp>

#include <cstdio>

namespace crd::geometry::shader_helpers
{

// ===========================================================================
// The HLSL helpers prelude — verbatim line-for-line translations of the GLSL
// helpers in glsl_emitter.cpp. Only syntax differs: vec3 → float3, vec2 →
// float2, floatBitsToUint → asuint, uintBitsToFloat → asfloat. Math is
// identical, so by induction over v9e-b's GLSL conformance proof, HLSL ≡ C++.
// ===========================================================================
namespace
{
constexpr const char* kPrelude = R"HLSL(
// --- helpers --------------------------------------------------------------
float crd_max3(float a, float b, float c) { return max(a, max(b, c)); }
float3 crd_vmax0(float3 v) { return max(v, float3(0.0, 0.0, 0.0)); }

// --- 3D primitives --------------------------------------------------------
float sd_sphere(float3 p, float r) { return length(p) - r; }

float sd_box(float3 p, float3 b) {
    float3 q = abs(p) - b;
    return length(crd_vmax0(q)) + min(crd_max3(q.x, q.y, q.z), 0.0);
}

float sd_round_box(float3 p, float3 b, float r) {
    float3 q = abs(p) - b + float3(r, r, r);
    return length(crd_vmax0(q)) + min(crd_max3(q.x, q.y, q.z), 0.0) - r;
}

float sd_box_frame(float3 p_in, float3 b, float e) {
    float3 p = abs(p_in) - b;
    float3 q = abs(p + float3(e, e, e)) - float3(e, e, e);
    float d0 = length(crd_vmax0(float3(p.x, q.y, q.z))) + min(crd_max3(p.x, q.y, q.z), 0.0);
    float d1 = length(crd_vmax0(float3(q.x, p.y, q.z))) + min(crd_max3(q.x, p.y, q.z), 0.0);
    float d2 = length(crd_vmax0(float3(q.x, q.y, p.z))) + min(crd_max3(q.x, q.y, p.z), 0.0);
    return min(min(d0, d1), d2);
}

float sd_plane(float3 p, float3 n, float h) { return dot(p, n) + h; }

float sd_capsule(float3 p, float3 a, float3 b, float r) {
    float3 pa = p - a;
    float3 ba = b - a;
    float denom = dot(ba, ba);
    float h = (denom > 0.0) ? clamp(dot(pa, ba) / denom, 0.0, 1.0) : 0.0;
    return length(pa - ba * h) - r;
}

float sd_cylinder(float3 p, float3 a, float3 b, float r) {
    float3 ba = b - a;
    float3 pa = p - a;
    float baba = dot(ba, ba);
    float paba = dot(pa, ba);
    float x  = length(pa * baba - ba * paba) - r * baba;
    float y  = abs(paba - baba * 0.5) - baba * 0.5;
    float x2 = x * x;
    float y2 = y * y * baba;
    float d  = (max(x, y) < 0.0)
               ? -min(x2, y2)
               : ((x > 0.0 ? x2 : 0.0) + (y > 0.0 ? y2 : 0.0));
    return sign(d) * sqrt(abs(d)) / baba;
}

float sd_cone(float3 p, float2 c, float h) {
    float2 q = float2(h * c.x / c.y, -h);
    float2 w = float2(sqrt(p.x * p.x + p.z * p.z), p.y);
    float t0 = clamp(dot(w, q) / dot(q, q), 0.0, 1.0);
    float2 a = w - q * t0;
    float2 b = float2(w.x - q.x * clamp(w.x / q.x, 0.0, 1.0), w.y - q.y);
    float k = sign(q.y);
    float d = min(dot(a, a), dot(b, b));
    float s = max(k * (w.x * q.y - w.y * q.x), k * (w.y - q.y));
    return sqrt(d) * sign(s);
}

float sd_torus(float3 p, float2 t) {
    float qx = sqrt(p.x * p.x + p.z * p.z) - t.x;
    return sqrt(qx * qx + p.y * p.y) - t.y;
}

float crd_dot2(float3 v) { return dot(v, v); }

float sd_triangle(float3 p, float3 a, float3 b, float3 c) {
    float3 ba = b - a; float3 pa = p - a;
    float3 cb = c - b; float3 pb = p - b;
    float3 ac = a - c; float3 pc = p - c;
    float3 nor = cross(ba, ac);
    float sign_sum = sign(dot(cross(ba, nor), pa))
                   + sign(dot(cross(cb, nor), pb))
                   + sign(dot(cross(ac, nor), pc));
    if (sign_sum < 2.0) {
        float e0 = crd_dot2(ba * clamp(dot(ba, pa) / crd_dot2(ba), 0.0, 1.0) - pa);
        float e1 = crd_dot2(cb * clamp(dot(cb, pb) / crd_dot2(cb), 0.0, 1.0) - pb);
        float e2 = crd_dot2(ac * clamp(dot(ac, pc) / crd_dot2(ac), 0.0, 1.0) - pc);
        return sqrt(min(min(e0, e1), e2));
    }
    float dn = dot(nor, pa);
    return sqrt(dn * dn / crd_dot2(nor));
}

// --- crd::math::deterministic ports (HLSL twin of GLSL) -------------------
static const float kCrdK4OverPi = 1.27323954473516;
static const float kCrdDp1      = 0.78515625;
static const float kCrdDp2      = 2.4187564849853515625e-4;
static const float kCrdDp3      = 3.77489497744594108e-8;
static const float kCrdSincofP0 = -1.9515295891e-4;
static const float kCrdSincofP1 =  8.3321608736e-3;
static const float kCrdSincofP2 = -1.6666654611e-1;
static const float kCrdCoscofP0 =  2.443315711809948e-5;
static const float kCrdCoscofP1 = -1.388731625493765e-3;
static const float kCrdCoscofP2 =  4.166664568298827e-2;
static const float kCrdLog2E    = 1.44269504088896341;
static const float kCrdLn2Hi    = 0.693359375;
static const float kCrdLn2Lo    = -2.12194440e-4;
static const float kCrdInvLn2   = 1.44269504088896341;
static const float kCrdExpcofP0 = 1.9875691500e-4;
static const float kCrdExpcofP1 = 1.3981999507e-3;
static const float kCrdExpcofP2 = 8.3334519073e-3;
static const float kCrdExpcofP3 = 4.1665795894e-2;
static const float kCrdExpcofP4 = 1.6666665459e-1;
static const float kCrdExpcofP5 = 5.0000001201e-1;
static const float kCrdLogcofP0 =  7.0376836292e-2;
static const float kCrdLogcofP1 = -1.1514610310e-1;
static const float kCrdLogcofP2 =  1.1676998740e-1;
static const float kCrdLogcofP3 = -1.2420140846e-1;
static const float kCrdLogcofP4 =  1.4249322787e-1;
static const float kCrdLogcofP5 = -1.6668057665e-1;
static const float kCrdLogcofP6 =  2.0000714765e-1;
static const float kCrdLogcofP7 = -2.4999993993e-1;
static const float kCrdLogcofP8 =  3.3333331174e-1;
static const float kCrdLogQ1    = -2.12194440e-4;
static const float kCrdLogQ2    =  0.693359375;
static const float kCrdExpMax   =  88.722832;
static const float kCrdExpMin   = -87.336544;

uint crd_sign_bit(float x) { return asuint(x) & 0x80000000u; }
float crd_apply_sign(float y, uint sign_xor) {
    return asfloat(asuint(y) ^ sign_xor);
}
float crd_fast_abs(float x) {
    return asfloat(asuint(x) & 0x7FFFFFFFu);
}
float crd_round_half_away(float x) {
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}
float crd_ldexp_int_pow2(float mantissa, int k) {
    if (k > 127)  { return mantissa * asfloat(0x7F800000u); }
    if (k < -126) { return 0.0; }
    return mantissa * asfloat(uint(k + 127) << 23);
}

float crd_det_sin(float xx) {
    uint sgn = crd_sign_bit(xx);
    float x  = crd_fast_abs(xx);
    float j  = trunc(x * kCrdK4OverPi);
    uint  jq = uint(j);
    if ((jq & 1u) != 0u) { j += 1.0; jq += 1u; }
    jq &= 7u;
    uint sign_out = sgn;
    if (jq > 3u) { sign_out ^= 0x80000000u; jq -= 4u; }
    float z  = ((x - j * kCrdDp1) - j * kCrdDp2) - j * kCrdDp3;
    float zz = z * z;
    float y;
    if (jq == 1u || jq == 2u) {
        y = ((kCrdCoscofP0 * zz + kCrdCoscofP1) * zz + kCrdCoscofP2) * zz * zz
          - 0.5 * zz + 1.0;
    } else {
        y = ((kCrdSincofP0 * zz + kCrdSincofP1) * zz + kCrdSincofP2) * zz * z + z;
    }
    return crd_apply_sign(y, sign_out);
}

float crd_det_cos(float xx) {
    float x = crd_fast_abs(xx);
    float j  = trunc(x * kCrdK4OverPi);
    uint  jq = uint(j);
    if ((jq & 1u) != 0u) { j += 1.0; jq += 1u; }
    jq &= 7u;
    uint sign_out = 0u;
    if (jq > 3u) { sign_out = 0x80000000u; jq -= 4u; }
    if (jq > 1u) { sign_out ^= 0x80000000u; }
    float z  = ((x - j * kCrdDp1) - j * kCrdDp2) - j * kCrdDp3;
    float zz = z * z;
    float y;
    if (jq == 1u || jq == 2u) {
        y = ((kCrdSincofP0 * zz + kCrdSincofP1) * zz + kCrdSincofP2) * zz * z + z;
    } else {
        y = ((kCrdCoscofP0 * zz + kCrdCoscofP1) * zz + kCrdCoscofP2) * zz * zz
          - 0.5 * zz + 1.0;
    }
    return crd_apply_sign(y, sign_out);
}

float crd_det_exp(float x) {
    if (x > kCrdExpMax) { return asfloat(0x7F800000u); }
    if (x < kCrdExpMin) { return 0.0; }
    float fk = crd_round_half_away(x * kCrdLog2E);
    int   k  = int(fk);
    float r  = (x - fk * kCrdLn2Hi) - fk * kCrdLn2Lo;
    float z  = r * r;
    float p  = ((((kCrdExpcofP0 * r + kCrdExpcofP1) * r + kCrdExpcofP2) * r
                + kCrdExpcofP3) * r + kCrdExpcofP4) * r + kCrdExpcofP5;
    float y  = 1.0 + r + z * p;
    return crd_ldexp_int_pow2(y, k);
}

float crd_det_exp2(float x) {
    return crd_det_exp(x * 0.69314718055994530942);
}

float crd_frexp_extract(float x, out int exp_out) {
    uint b       = asuint(x);
    int  raw_exp = int((b >> 23u) & 0xFFu);
    exp_out      = raw_exp - 126;
    uint mant    = (b & 0x807FFFFFu) | 0x3F000000u;
    return asfloat(mant);
}

float crd_det_log(float x) {
    if (x <  0.0) { return asfloat(0x7FC00000u); }
    if (x == 0.0) { return -asfloat(0x7F800000u); }
    int   exp_int = 0;
    float m       = crd_frexp_extract(x, exp_int);
    if (m < 0.707106781187) { exp_int -= 1; m += m; }
    m -= 1.0;
    float z  = m * m;
    float fe = float(exp_int);
    float y  = ((((((((kCrdLogcofP0 * m + kCrdLogcofP1) * m + kCrdLogcofP2) * m
                    + kCrdLogcofP3) * m + kCrdLogcofP4) * m + kCrdLogcofP5) * m
                + kCrdLogcofP6) * m + kCrdLogcofP7) * m + kCrdLogcofP8) * m * z;
    y += fe * kCrdLogQ1;
    y -= 0.5 * z;
    y += m;
    y += fe * kCrdLogQ2;
    return y;
}

float crd_det_log2(float x) {
    return crd_det_log(x) * kCrdInvLn2;
}

// --- Value-domain operators ----------------------------------------------
static const float kCrdFltMin = 1.17549435e-38;
float smin_poly(float a, float b, float k) {
    float kk = k * 4.0;
    float diff = (a < b) ? (b - a) : (a - b);
    float h = max(kk - diff, 0.0) / (kk + kCrdFltMin);
    return min(a, b) - h * h * kk * 0.25;
}

float smin_cubic(float a, float b, float k) {
    float kk = k * 6.0;
    float diff = (a < b) ? (b - a) : (a - b);
    float h = max(kk - diff, 0.0) / (kk + kCrdFltMin);
    return min(a, b) - h * h * h * kk * (1.0 / 6.0);
}

float smin_exp(float a, float b, float k) {
    float r = crd_det_exp2(-a / k) + crd_det_exp2(-b / k);
    return -k * crd_det_log2(r);
}

float smax_poly(float a, float b, float k) {
    float kk = k * 4.0;
    float diff = (a < b) ? (b - a) : (a - b);
    float h = max(kk - diff, 0.0) / (kk + kCrdFltMin);
    return max(a, b) + h * h * kk * 0.25;
}

float op_round(float d, float r)  { return d - r; }
float op_onion(float d, float t)  { return abs(d) - t; }

// --- Position-domain operators -------------------------------------------
float3 domain_repeat(float3 p, float3 c) {
    return p - c * floor(p / c + float3(0.5, 0.5, 0.5));
}

float domain_mirror_axis(float p, float period) {
    float two_period = 2.0 * period;
    float q = p - two_period * floor(p / two_period);
    if (q > period) { q = two_period - q; }
    return q - 0.5 * period;
}
float3 domain_mirror(float3 p, float3 c) {
    return float3(domain_mirror_axis(p.x, c.x),
                  domain_mirror_axis(p.y, c.y),
                  domain_mirror_axis(p.z, c.z));
}

float3 domain_elongate(float3 p, float3 h) {
    return p - clamp(p, -h, h);
}

float3 domain_twist(float3 p, float k) {
    float angle = k * p.y;
    float c = crd_det_cos(angle);
    float s = crd_det_sin(angle);
    return float3(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
}

float3 domain_bend(float3 p, float k) {
    float angle = k * p.x;
    float c = crd_det_cos(angle);
    float s = crd_det_sin(angle);
    return float3(c * p.x - s * p.y, s * p.x + c * p.y, p.z);
}

)HLSL";
} // namespace

crd::containers::StringView hlsl_helpers_prelude() noexcept
{
    return crd::containers::StringView(kPrelude);
}

// ===========================================================================
// IR walker — same SSA structure as GLSL, different syntax. Helpers all
// take `float3`/`float2` instead of `vec3`/`vec2`.
// ===========================================================================
namespace
{

void append_f32(crd::containers::String& out, float v) noexcept
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    out.append(buf);
}

void append_uint(crd::containers::String& out, crd::u32 v) noexcept
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", v);
    out.append(buf);
}

void emit_n(crd::containers::String& out, crd::u32 idx) noexcept
{
    out.append("n_");
    append_uint(out, idx);
}

void emit_p(crd::containers::String& out, const char* base) noexcept
{
    out.append(base);
}

void emit_float3(crd::containers::String& out, crd::containers::ConstSpan<float> params, crd::usize offset) noexcept
{
    out.append("float3(");
    append_f32(out, params[offset + 0U]); out.append(", ");
    append_f32(out, params[offset + 1U]); out.append(", ");
    append_f32(out, params[offset + 2U]); out.append(")");
}

void emit_float2(crd::containers::String& out, crd::containers::ConstSpan<float> params, crd::usize offset) noexcept
{
    out.append("float2(");
    append_f32(out, params[offset + 0U]); out.append(", ");
    append_f32(out, params[offset + 1U]); out.append(")");
}

void emit_primitive_call(crd::containers::String& out,
                          IrPrimKind kind,
                          crd::containers::ConstSpan<float> params,
                          const char* p_var) noexcept
{
    switch (kind)
    {
        case IrPrimKind::Sphere:
            out.append("sd_sphere(");   emit_p(out, p_var); out.append(", ");
            append_f32(out, params[0]); out.append(")");
            break;
        case IrPrimKind::Box:
            out.append("sd_box(");      emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U); out.append(")");
            break;
        case IrPrimKind::RoundBox:
            out.append("sd_round_box("); emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U);  out.append(", ");
            append_f32(out, params[3]);  out.append(")");
            break;
        case IrPrimKind::BoxFrame:
            out.append("sd_box_frame("); emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U);  out.append(", ");
            append_f32(out, params[3]);  out.append(")");
            break;
        case IrPrimKind::Plane:
            out.append("sd_plane(");     emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U);  out.append(", ");
            append_f32(out, params[3]);  out.append(")");
            break;
        case IrPrimKind::Capsule:
            out.append("sd_capsule(");   emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U);  out.append(", ");
            emit_float3(out, params, 3U);  out.append(", ");
            append_f32(out, params[6]);  out.append(")");
            break;
        case IrPrimKind::Cylinder:
            out.append("sd_cylinder(");  emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U);  out.append(", ");
            emit_float3(out, params, 3U);  out.append(", ");
            append_f32(out, params[6]);  out.append(")");
            break;
        case IrPrimKind::Cone:
            out.append("sd_cone(");      emit_p(out, p_var); out.append(", ");
            emit_float2(out, params, 0U);  out.append(", ");
            append_f32(out, params[2]);  out.append(")");
            break;
        case IrPrimKind::Torus:
            out.append("sd_torus(");     emit_p(out, p_var); out.append(", ");
            emit_float2(out, params, 0U);  out.append(")");
            break;
        case IrPrimKind::Triangle3D:
            out.append("sd_triangle(");  emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U);  out.append(", ");
            emit_float3(out, params, 3U);  out.append(", ");
            emit_float3(out, params, 6U);  out.append(")");
            break;
        case IrPrimKind::Count_:
            CRD_ASSERT_MSG(false, "emit_primitive_call: invalid IrPrimKind");
            break;
    }
}

void emit_domain_warp_call(crd::containers::String& out,
                            IrOpKind kind,
                            crd::containers::ConstSpan<float> params,
                            const char* p_var) noexcept
{
    switch (kind)
    {
        case IrOpKind::DomainRepeat:
            out.append("domain_repeat("); emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U);   out.append(")");
            break;
        case IrOpKind::DomainMirror:
            out.append("domain_mirror("); emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U);   out.append(")");
            break;
        case IrOpKind::DomainElongate:
            out.append("domain_elongate("); emit_p(out, p_var); out.append(", ");
            emit_float3(out, params, 0U);     out.append(")");
            break;
        case IrOpKind::DomainTwist:
            out.append("domain_twist(");  emit_p(out, p_var); out.append(", ");
            append_f32(out, params[0]);   out.append(")");
            break;
        case IrOpKind::DomainBend:
            out.append("domain_bend(");   emit_p(out, p_var); out.append(", ");
            append_f32(out, params[0]);   out.append(")");
            break;
        default:
            CRD_ASSERT_MSG(false, "emit_domain_warp_call: not a position-domain op");
            break;
    }
}

[[nodiscard]] bool is_position_domain_op(IrOpKind k) noexcept
{
    return k == IrOpKind::DomainRepeat || k == IrOpKind::DomainMirror
        || k == IrOpKind::DomainElongate || k == IrOpKind::DomainTwist
        || k == IrOpKind::DomainBend;
}

void emit_value_op_call(crd::containers::String& out,
                         IrOpKind kind,
                         crd::containers::ConstSpan<float> params,
                         crd::containers::ConstSpan<crd::u32> children) noexcept
{
    switch (kind)
    {
        case IrOpKind::SminPoly:
            out.append("smin_poly(");
            emit_n(out, children[0]); out.append(", ");
            emit_n(out, children[1]); out.append(", ");
            append_f32(out, params[0]); out.append(")");
            break;
        case IrOpKind::SminCubic:
            out.append("smin_cubic(");
            emit_n(out, children[0]); out.append(", ");
            emit_n(out, children[1]); out.append(", ");
            append_f32(out, params[0]); out.append(")");
            break;
        case IrOpKind::SminExp:
            out.append("smin_exp(");
            emit_n(out, children[0]); out.append(", ");
            emit_n(out, children[1]); out.append(", ");
            append_f32(out, params[0]); out.append(")");
            break;
        case IrOpKind::SmaxPoly:
            out.append("smax_poly(");
            emit_n(out, children[0]); out.append(", ");
            emit_n(out, children[1]); out.append(", ");
            append_f32(out, params[0]); out.append(")");
            break;
        case IrOpKind::OpRound:
            out.append("op_round(");
            emit_n(out, children[0]); out.append(", ");
            append_f32(out, params[0]); out.append(")");
            break;
        case IrOpKind::OpOnion:
            out.append("op_onion(");
            emit_n(out, children[0]); out.append(", ");
            append_f32(out, params[0]); out.append(")");
            break;
        default:
            CRD_ASSERT_MSG(false, "emit_value_op_call: not a value-domain op");
            break;
    }
}

void emit_node(const FormulaIr& ir,
                crd::u32 idx,
                const char* p_var,
                crd::containers::String& out) noexcept
{
    const IrNode& node = ir.nodes()[idx];
    const auto params   = ir.params_of(node);
    const auto children = ir.children_of(node);

    if (node.kind == IrNode::Kind::Primitive)
    {
        out.append("    float ");
        emit_n(out, idx);
        out.append(" = ");
        emit_primitive_call(out, node.prim, params, p_var);
        out.append(";\n");
        return;
    }

    if (is_position_domain_op(node.op))
    {
        char child_p_buf[16];
        std::snprintf(child_p_buf, sizeof(child_p_buf), "p_%u", idx);

        out.append("    float3 ");
        out.append(child_p_buf);
        out.append(" = ");
        emit_domain_warp_call(out, node.op, params, p_var);
        out.append(";\n");

        emit_node(ir, children[0], child_p_buf, out);

        out.append("    float ");
        emit_n(out, idx);
        out.append(" = ");
        emit_n(out, children[0]);
        out.append(";\n");
        return;
    }

    for (crd::u32 c : children)
    {
        emit_node(ir, c, p_var, out);
    }
    out.append("    float ");
    emit_n(out, idx);
    out.append(" = ");
    emit_value_op_call(out, node.op, params, children);
    out.append(";\n");
}

} // namespace

crd::containers::String emit_hlsl_sdf_function(const FormulaIr&            ir,
                                                 crd::containers::StringView function_name,
                                                 crd::memory::IAllocator*    alloc) noexcept
{
    CRD_ASSERT_MSG(!ir.is_empty(), "emit_hlsl_sdf_function: empty IR");
    CRD_ASSERT_MSG(ir.root() < ir.nodes().size(), "emit_hlsl_sdf_function: root out of bounds");

    crd::containers::String out(alloc);
    out.append("float ");
    out.append(function_name);
    out.append("(float3 p) {\n");

    emit_node(ir, ir.root(), "p", out);

    out.append("    return ");
    emit_n(out, ir.root());
    out.append(";\n}\n");
    return out;
}

crd::containers::String emit_hlsl_conformance_shader(const FormulaIr&         ir,
                                                       crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::String out(alloc);

    // HLSL 6.0 compute shader header. Uses Vulkan-flavour push constants
    // (`[[vk::push_constant]]`) so the same shader can dxc-target SPIR-V
    // for the future v9e-c-dxc-spirv-dispatch path. For pure D3D12 the
    // attribute is silently ignored.
    out.append("// Generated by crd::geometry::shader_helpers::emit_hlsl_conformance_shader\n");
    out.append("// HLSL 6.0 — dxc -T cs_6_0 -E cs_main  (or `-spirv` for Vulkan dispatch)\n\n");
    out.append("RWStructuredBuffer<float> out_buf : register(u0);\n\n");
    out.append("struct PushConstants {\n");
    out.append("    float3 grid_origin;\n");
    out.append("    float  pad0;\n");
    out.append("    float3 grid_step;\n");
    out.append("    uint   grid_resolution;\n");
    out.append("};\n");
    out.append("[[vk::push_constant]] ConstantBuffer<PushConstants> pc;\n\n");
    out.append(hlsl_helpers_prelude());
    out.append("\n");

    const auto sdf_body = emit_hlsl_sdf_function(ir, crd::containers::StringView("sdf"), alloc);
    out.append(sdf_body);
    out.append("\n");

    out.append("[numthreads(4, 4, 4)]\n");
    out.append("void cs_main(uint3 idx : SV_DispatchThreadID) {\n");
    out.append("    if (idx.x >= pc.grid_resolution || idx.y >= pc.grid_resolution || idx.z >= pc.grid_resolution) return;\n");
    out.append("    uint flat_idx = (idx.z * pc.grid_resolution + idx.y) * pc.grid_resolution + idx.x;\n");
    out.append("    float3 p = pc.grid_origin + float3(idx) * pc.grid_step;\n");
    out.append("    out_buf[flat_idx] = sdf(p);\n");
    out.append("}\n");

    return out;
}

} // namespace crd::geometry::shader_helpers
