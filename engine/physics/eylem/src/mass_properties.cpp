// Mass-properties derivation. Phase 3.1 v1a-material-d (ADR-0069 §3 + §8 + §11).

#include <crd/eylem/mass_properties.hpp>

#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>

namespace crd::eylem
{
namespace
{
// ---- Volume formulas -------------------------------------------------------

constexpr crd::f32 kPi    = 3.14159265358979323846F;
constexpr crd::f32 kFourThirdsPi = 4.0F * kPi / 3.0F;

[[nodiscard]] crd::f32 volume_of(const Collider& c) noexcept
{
    switch (c.shape)
    {
    case ColliderShape::Sphere:
    {
        const crd::f32 r = c.sphere.radius;
        return kFourThirdsPi * r * r * r;
    }
    case ColliderShape::Box:
    {
        const auto& he = c.box.half_extents;
        return 8.0F * he.x * he.y * he.z;
    }
    case ColliderShape::Capsule:
    {
        const crd::f32 r = c.capsule.radius;
        const crd::f32 h = c.capsule.half_height; // half of cylinder body length
        const crd::f32 v_cyl = kPi * r * r * (2.0F * h);
        const crd::f32 v_sph = kFourThirdsPi * r * r * r;
        return v_cyl + v_sph;
    }
    // v1a: convex / plane / mesh / heightfield / sdf carry no analytic
    // volume. v1d-mesh + v1d-hf + v1d-sdf (and the cooker, v1k) supply
    // pre-computed mass properties for these via a side channel.
    case ColliderShape::ConvexHull:
    case ColliderShape::Plane:
    case ColliderShape::TriangleMesh:
    case ColliderShape::Heightfield:
    case ColliderShape::Sdf:
    default:
        return 0.0F;
    }
}

// ---- Inertia tensor (about collider centroid, in collider local frame) ----
//
// Returns a Vec3f representing the diagonal of the inertia tensor for a
// canonical-orientation collider. Off-diagonal terms are zero by symmetry
// (sphere = isotropic, box = axis-aligned, capsule = axis-symmetric about Y).
[[nodiscard]] crd::math::Vec3f inertia_diagonal_local(const Collider& c, crd::f32 mass) noexcept
{
    if (mass <= 0.0F)
    {
        return crd::math::Vec3f{0.0F, 0.0F, 0.0F};
    }

    switch (c.shape)
    {
    case ColliderShape::Sphere:
    {
        const crd::f32 r = c.sphere.radius;
        const crd::f32 v = (2.0F / 5.0F) * mass * r * r;
        return crd::math::Vec3f{v, v, v};
    }
    case ColliderShape::Box:
    {
        const auto& he = c.box.half_extents; // half-extents
        const crd::f32 hx2 = he.x * he.x;
        const crd::f32 hy2 = he.y * he.y;
        const crd::f32 hz2 = he.z * he.z;
        const crd::f32 third = mass / 3.0F;
        return crd::math::Vec3f{
            third * (hy2 + hz2),
            third * (hx2 + hz2),
            third * (hx2 + hy2)};
    }
    case ColliderShape::Capsule:
    {
        // Capsule = cylinder body (length L = 2h, axis = Y) + 2 hemisphere caps.
        // Mass split by volume share at uniform density.
        const crd::f32 r  = c.capsule.radius;
        const crd::f32 h  = c.capsule.half_height;
        const crd::f32 r2 = r * r;
        const crd::f32 v_cyl = kPi * r2 * (2.0F * h);
        const crd::f32 v_sph = kFourThirdsPi * r2 * r;
        const crd::f32 v_tot = v_cyl + v_sph;
        if (v_tot <= 0.0F)
        {
            return crd::math::Vec3f{0.0F, 0.0F, 0.0F};
        }
        const crd::f32 m_cyl = mass * (v_cyl / v_tot);
        const crd::f32 m_sph = mass * (v_sph / v_tot);
        const crd::f32 d_hs  = h + 3.0F * r / 8.0F; // capsule centre → hemisphere centroid
        const crd::f32 i_y   = 0.5F  * m_cyl * r2 + (2.0F / 5.0F) * m_sph * r2;
        const crd::f32 i_xz  = (1.0F / 12.0F) * m_cyl * (3.0F * r2 + 4.0F * h * h)
                             + (83.0F / 320.0F) * m_sph * r2
                             + m_sph * d_hs * d_hs;
        return crd::math::Vec3f{i_xz, i_y, i_xz};
    }
    // ConvexHull / Plane / TriangleMesh / Heightfield / Sdf — caller
    // supplies mass properties out-of-band at v1d+ / cooker (v1k).
    default:
        return crd::math::Vec3f{0.0F, 0.0F, 0.0F};
    }
}

// Build a 3x3 diagonal inertia matrix from the collider-local diagonal.
[[nodiscard]] crd::math::Mat3f make_diag(crd::math::Vec3f d) noexcept
{
    return crd::math::Mat3f{
        crd::math::Vec3f{d.x, 0.0F, 0.0F},
        crd::math::Vec3f{0.0F, d.y, 0.0F},
        crd::math::Vec3f{0.0F, 0.0F, d.z}};
}

// I' = R · I · R^T  — rotate a 3x3 inertia tensor from collider local frame
// to body local frame using the collider's local_rotation. Result remains
// symmetric.
[[nodiscard]] crd::math::Mat3f rotate_inertia(const crd::math::Mat3f& i,
                                              const crd::math::Mat3f& r) noexcept
{
    // r is column-major (c0 / c1 / c2 are columns). r * i:
    auto col_mul = [](const crd::math::Mat3f& a, const crd::math::Vec3f& b) noexcept
    {
        return crd::math::Vec3f{
            a.c0.x * b.x + a.c1.x * b.y + a.c2.x * b.z,
            a.c0.y * b.x + a.c1.y * b.y + a.c2.y * b.z,
            a.c0.z * b.x + a.c1.z * b.y + a.c2.z * b.z};
    };
    const crd::math::Mat3f ri{col_mul(r, i.c0), col_mul(r, i.c1), col_mul(r, i.c2)};
    // r^T has rows = r's columns. (ri * r^T)_{kc} = Σ ri_{ka} * r^T_{ac}
    //                                              = Σ ri_{ka} * r_{ca}.
    auto row = [](const crd::math::Mat3f& m, crd::usize k) noexcept
    {
        // Column-major: row k = (c0[k], c1[k], c2[k]).
        return crd::math::Vec3f{m.c0[k], m.c1[k], m.c2[k]};
    };
    crd::math::Mat3f out{};
    for (crd::usize c = 0; c < 3; ++c)
    {
        const crd::math::Vec3f rc = row(r, c); // c-th row of r
        for (crd::usize k = 0; k < 3; ++k)
        {
            const crd::math::Vec3f rk = row(ri, k); // k-th row of ri
            out[c][k] = rk.x * rc.x + rk.y * rc.y + rk.z * rc.z;
        }
    }
    return out;
}

// Parallel-axis theorem: shift inertia tensor from one point to another,
// for a body of mass `m` with displacement `d` between the two points.
// I_new = I_old + m * (||d||² · I_3 - d · d^T)
[[nodiscard]] crd::math::Mat3f parallel_axis_shift(const crd::math::Mat3f& i,
                                                   crd::f32                 m,
                                                   crd::math::Vec3f         d) noexcept
{
    const crd::f32 d2 = d.x * d.x + d.y * d.y + d.z * d.z;
    crd::math::Mat3f out = i;
    // Add m * d² * I_3
    out.c0.x += m * d2;
    out.c1.y += m * d2;
    out.c2.z += m * d2;
    // Subtract m * d · d^T (column-major: column j has entries m*d_j*d_i)
    out.c0.x -= m * d.x * d.x;
    out.c0.y -= m * d.x * d.y;
    out.c0.z -= m * d.x * d.z;
    out.c1.x -= m * d.y * d.x;
    out.c1.y -= m * d.y * d.y;
    out.c1.z -= m * d.y * d.z;
    out.c2.x -= m * d.z * d.x;
    out.c2.y -= m * d.z * d.y;
    out.c2.z -= m * d.z * d.z;
    return out;
}

} // namespace

DerivedMassProperties derive_mass_properties(
    crd::containers::ConstSpan<Collider> colliders,
    MaterialAccessor                     accessor,
    void*                                user_data) noexcept
{
    DerivedMassProperties out{};
    if (colliders.empty() || accessor == nullptr)
    {
        return out;
    }

    // -------- Pass 1: mass + COM (in ascending ColliderId order) ---------
    crd::f32         total_mass = 0.0F;
    crd::math::Vec3f com_accum{0.0F, 0.0F, 0.0F};
    for (const Collider& c : colliders)
    {
        const crd::f32 v = volume_of(c);
        if (v <= 0.0F)
        {
            continue;
        }
        const Material& mat = accessor(user_data, c.material);
        const crd::f32  m_i = v * mat.density;
        total_mass    += m_i;
        com_accum.x   += m_i * c.local_position.x;
        com_accum.y   += m_i * c.local_position.y;
        com_accum.z   += m_i * c.local_position.z;
    }

    if (total_mass <= 0.0F)
    {
        // All zero-volume. Body is structurally static — return zeros so
        // the caller can treat as RigidBody::inv_mass = 0 (infinite).
        return out;
    }

    out.mass = total_mass;
    out.com_local = crd::math::Vec3f{
        com_accum.x / total_mass,
        com_accum.y / total_mass,
        com_accum.z / total_mass};

    // -------- Pass 2: inertia tensor about body COM (same iteration order) ----
    crd::math::Mat3f i_total{
        crd::math::Vec3f{0.0F, 0.0F, 0.0F},
        crd::math::Vec3f{0.0F, 0.0F, 0.0F},
        crd::math::Vec3f{0.0F, 0.0F, 0.0F}};
    for (const Collider& c : colliders)
    {
        const crd::f32 v = volume_of(c);
        if (v <= 0.0F)
        {
            continue;
        }
        const Material&  mat = accessor(user_data, c.material);
        const crd::f32   m_i = v * mat.density;
        const crd::math::Vec3f i_diag_local = inertia_diagonal_local(c, m_i);
        const crd::math::Mat3f i_local      = make_diag(i_diag_local);
        const crd::math::Mat3f r            = crd::math::to_mat3(c.local_rotation);
        const crd::math::Mat3f i_in_body    = rotate_inertia(i_local, r);
        const crd::math::Vec3f d{
            c.local_position.x - out.com_local.x,
            c.local_position.y - out.com_local.y,
            c.local_position.z - out.com_local.z};
        const crd::math::Mat3f i_shifted    = parallel_axis_shift(i_in_body, m_i, d);
        // Sum
        i_total.c0.x += i_shifted.c0.x;
        i_total.c0.y += i_shifted.c0.y;
        i_total.c0.z += i_shifted.c0.z;
        i_total.c1.x += i_shifted.c1.x;
        i_total.c1.y += i_shifted.c1.y;
        i_total.c1.z += i_shifted.c1.z;
        i_total.c2.x += i_shifted.c2.x;
        i_total.c2.y += i_shifted.c2.y;
        i_total.c2.z += i_shifted.c2.z;
    }
    // v1a returns diagonal only; ADR reserves off-diagonal for v1c+v1f.
    out.inertia_diagonal = crd::math::Vec3f{i_total.c0.x, i_total.c1.y, i_total.c2.z};

    return out;
}

} // namespace crd::eylem
