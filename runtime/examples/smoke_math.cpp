#include <crd/math/math.hpp>

#include <iostream>

int main()
{
    using namespace crd::math;

    const Vec3f right(1.0f, 0.0f, 0.0f);
    const Vec3f up(0.0f, 1.0f, 0.0f);
    const Vec3f forward = cross(right, up);
    const Vec3f velocity(3.0f, 4.0f, 0.0f);
    const Vec3f dir = normalized(velocity);
    const Mat3f basis(Vec3f(1.0f, 0.0f, 0.0f), Vec3f(0.0f, 2.0f, 0.0f), Vec3f(0.0f, 0.0f, 3.0f));
    const Vec3f scaled = basis * Vec3f(1.0f, 2.0f, 3.0f);
    const Quatf quarter_turn = from_axis_angle(Vec3f(0.0f, 0.0f, 1.0f), k_half_pi_f);
    const Vec3f rotated = rotate_vector(quarter_turn, Vec3f(1.0f, 0.0f, 0.0f));
    const Transformf pose(Vec3f(10.0f, 0.0f, 0.0f), quarter_turn);
    const Vec3f transformed = transform_point(pose, Vec3f(1.0f, 0.0f, 0.0f));

    std::cout << "pi<float>=" << k_pi<crd::f32> << "\n";
    std::cout << "pi<float> alias=" << k_pi_f << "\n";
    std::cout << "pi<double> alias=" << k_pi_d << "\n";
    std::cout << "forward = (" << forward.x << ", " << forward.y << ", " << forward.z << ")\n";
    std::cout << "normalized velocity = (" << dir.x << ", " << dir.y << ", " << dir.z << ")\n";
    std::cout << "basis * (1,2,3) = (" << scaled.x << ", " << scaled.y << ", " << scaled.z << ")\n";
    std::cout << "rotated x by +90deg around z = (" << rotated.x << ", " << rotated.y << ", " << rotated.z << ")\n";
    std::cout << "transform point -> (" << transformed.x << ", " << transformed.y << ", " << transformed.z << ")\n";
    std::cout << "90 deg in radians = " << deg_to_rad(90.0f) << "\n";
    return 0;
}
