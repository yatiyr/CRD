#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/math.hpp>

#include <iostream>

int main()
{
    using namespace crd::math;
    using namespace crd::geometry::primitives;

    const Vec3f right(1.0F, 0.0F, 0.0F);
    const Vec3f up(0.0F, 1.0F, 0.0F);
    const Vec3f forward = cross(right, up);
    const Vec3f velocity(3.0F, 4.0F, 0.0F);
    const Vec3f dir = normalized(velocity);
    const Mat3f basis(Vec3f(1.0F, 0.0F, 0.0F), Vec3f(0.0F, 2.0F, 0.0F), Vec3f(0.0F, 0.0F, 3.0F));
    const Vec3f scaled = basis * Vec3f(1.0F, 2.0F, 3.0F);
    const Quatf quarter_turn = from_axis_angle(Vec3f(0.0F, 0.0F, 1.0F), k_half_pi_f);
    const Vec3f rotated = rotate_vector(quarter_turn, Vec3f(1.0F, 0.0F, 0.0F));
    const Transformf pose(Vec3f(10.0F, 0.0F, 0.0F), quarter_turn);
    const Vec3f transformed = transform_point(pose, Vec3f(1.0F, 0.0F, 0.0F));
    const Planef ground = plane_from_point_normal(Vec3f(0.0F, 0.0F, 0.0F), Vec3f(0.0F, 1.0F, 0.0F));
    const Ray3f drop_ray(Vec3f(0.0F, 10.0F, 0.0F), Vec3f(0.0F, -1.0F, 0.0F));
    const Triangle3f tri(Vec3f(-1.0F, -1.0F, 0.0F), Vec3f(1.0F, -1.0F, 0.0F), Vec3f(0.0F, 1.0F, 0.0F));
    const Frustumf frustum = frustum_from_view_projection(Mat4f::identity());
    const AABB3f bounds(Vec3f(-0.5F, -0.5F, -0.5F), Vec3f(0.5F, 0.5F, 0.5F));
    float ray_t = 0.0F;
    Vec3f bary{};
    const bool ray_hit_ground = intersect_ray_plane(drop_ray, ground, ray_t);
    const bool ray_hit_tri =
        intersect_ray_triangle(Ray3f(Vec3f(0.0F, 0.0F, -5.0F), Vec3f(0.0F, 0.0F, 1.0F)), tri, ray_t, bary);
    const bool frustum_hits_bounds = intersects(frustum, bounds);

    std::cout << "pi<float>=" << k_pi<crd::f32> << "\n";
    std::cout << "pi<float> alias=" << k_pi_f << "\n";
    std::cout << "pi<double> alias=" << k_pi_d << "\n";
    std::cout << "forward = (" << forward.x << ", " << forward.y << ", " << forward.z << ")\n";
    std::cout << "normalized velocity = (" << dir.x << ", " << dir.y << ", " << dir.z << ")\n";
    std::cout << "basis * (1,2,3) = (" << scaled.x << ", " << scaled.y << ", " << scaled.z << ")\n";
    std::cout << "rotated x by +90deg around z = (" << rotated.x << ", " << rotated.y << ", " << rotated.z << ")\n";
    std::cout << "transform point -> (" << transformed.x << ", " << transformed.y << ", " << transformed.z << ")\n";
    std::cout << "ray hits ground? " << ray_hit_ground << " at t=" << ray_t << "\n";
    std::cout << "ray hits triangle? " << ray_hit_tri << " bary=(" << bary.x << ", " << bary.y << ", " << bary.z
              << ")\n";
    std::cout << "identity frustum intersects unit-ish bounds? " << frustum_hits_bounds << "\n";
    std::cout << "90 deg in radians = " << deg_to_rad(90.0F) << "\n";
    return 0;
}
