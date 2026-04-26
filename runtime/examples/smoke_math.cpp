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

    std::cout << "pi<float>=" << k_pi<crd::f32> << "\n";
    std::cout << "forward = (" << forward.x << ", " << forward.y << ", " << forward.z << ")\n";
    std::cout << "normalized velocity = (" << dir.x << ", " << dir.y << ", " << dir.z << ")\n";
    std::cout << "90 deg in radians = " << deg_to_rad(90.0f) << "\n";
    return 0;
}
