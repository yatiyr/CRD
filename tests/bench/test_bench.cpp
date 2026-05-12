#include <crd/containers/containers.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/log/log.hpp>
#include <crd/math/math.hpp>
#include <crd/memory/memory.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace crd;
using namespace crd::containers;
using namespace crd::log;
using namespace crd::math;
using namespace crd::geometry::primitives;

CRD_DEFINE_LOG_CHANNEL(g_log_bench, "Bench", crd::log::LogLevel::Trace)

namespace
{
struct BenchLoggerScope
{
    explicit BenchLoggerScope(LoggerConfig cfg = {})
    {
        if (is_initialized())
        {
            shutdown();
        }
        clear_sinks();
        init(cfg);
        add_sink(std::make_unique<NullSink>());
        g_log_bench.runtime_level = LogLevel::Trace;
    }

    ~BenchLoggerScope() { shutdown(); }
};

// Calls CRD_LOG_INFO with the channel's runtime_level set above Error, so the
// call compiles in all configurations but is filtered at runtime. Measures the
// cost of the runtime level check + early-out, not compile-time elimination.
CRD_FORCEINLINE void runtime_disabled_info_call() noexcept
{
    CRD_LOG_INFO(g_log_bench, "runtime-disabled info {}", 42);
}

volatile f32 g_vec3f_bench_bias = 0.0F;
volatile f64 g_vec3d_bench_bias = 0.0;
volatile f32 g_mat4f_bench_bias = 0.0F;
volatile f64 g_mat4d_bench_bias = 0.0;
volatile f32 g_quatf_bench_bias = 0.0F;
volatile f32 g_transformf_bench_bias = 0.0F;
} // namespace

TEST_CASE("Runtime-disabled log call cost", "[bench][log]")
{
    BenchLoggerScope scope{};
    g_log_bench.runtime_level = LogLevel::Error; // gate below Error at runtime

    BENCHMARK("runtime-disabled INFO call")
    {
        runtime_disabled_info_call();
        return 0;
    };
}

TEST_CASE("Async producer push cost", "[bench][log]")
{
    LoggerConfig cfg;
    cfg.async = true;
    cfg.async_queue_capacity = 1 << 16;
    cfg.drop_on_overflow = false;
    BenchLoggerScope scope{cfg};

    BENCHMARK("async log push")
    {
        CRD_LOG_INFO(g_log_bench, "bench message {}", 7);
        return 0;
    };

    flush();
}

TEST_CASE("Array push_back amortised 1k", "[bench][containers]")
{
    BENCHMARK("Array<u32>::push_back 1k")
    {
        Array<u32> values;
        for (u32 i = 0; i < 1024; ++i)
        {
            values.push_back(i);
        }
        return values.size();
    };
}

TEST_CASE("HashMap integer workloads", "[bench][containers]")
{
    constexpr u32 kCount = 1U << 20;

    BENCHMARK("HashMap<u32,u32> insert 1M")
    {
        HashMap<u32, u32> map;
        for (u32 i = 0; i < kCount; ++i)
        {
            (void)map.insert(i, i + 1);
        }
        return map.size();
    };

    HashMap<u32, u32> seeded;
    seeded.reserve(kCount);
    for (u32 i = 0; i < kCount; ++i)
    {
        (void)seeded.insert(i, i + 1);
    }

    BENCHMARK("HashMap<u32,u32> find 1M")
    {
        u64 sum = 0;
        for (u32 i = 0; i < kCount; ++i)
        {
            const u32* value = seeded.find(i);
            sum += value ? *value : 0U;
        }
        return sum;
    };

    BENCHMARK("HashMap<u32,u32> erase 1M")
    {
        HashMap<u32, u32> map = seeded;
        for (u32 i = 0; i < kCount; ++i)
        {
            (void)map.erase(i);
        }
        return map.size();
    };
}

TEST_CASE("String SSO vs heap workloads", "[bench][containers]")
{
    BENCHMARK("String SSO construct+assign")
    {
        String s("cerid");
        s.append(StringView{"-bench"});
        return s.size();
    };

    BENCHMARK("String heap construct+assign")
    {
        String s("this-string-is-definitely-longer-than-the-sso-boundary");
        s.append(StringView{"-and-it-grows-even-more"});
        return s.size();
    };
}

TEST_CASE("Vec3 float workloads", "[bench][math]")
{
    const Vec3f a(1.0F, 2.0F, 3.0F);
    const Vec3f b(4.0F, 5.0F, 6.0F);

    BENCHMARK("Vec3f add")
    {
        return a + b;
    };

    BENCHMARK("Vec3f dot")
    {
        return dot(a, b);
    };

    BENCHMARK("Vec3f normalize")
    {
        const Vec3f v(3.0F + g_vec3f_bench_bias, 4.0F, 5.0F);
        const Vec3f n = normalized(v);
        return n.x + n.y + n.z;
    };
}

TEST_CASE("Vec3 double workloads", "[bench][math]")
{
    const Vec3d a(1.0, 2.0, 3.0);
    const Vec3d b(4.0, 5.0, 6.0);

    BENCHMARK("Vec3d dot")
    {
        return dot(a, b);
    };

    BENCHMARK("Vec3d normalize")
    {
        const Vec3d v(3.0 + g_vec3d_bench_bias, 4.0, 5.0);
        const Vec3d n = normalized(v);
        return n.x + n.y + n.z;
    };
}

TEST_CASE("Mat4 float workloads", "[bench][math]")
{
    BENCHMARK("Mat4f * Vec4f")
    {
        const Mat4f a(Vec4f(1.0F + g_mat4f_bench_bias, 2.0F, 3.0F, 4.0F), Vec4f(5.0F, 6.0F, 7.0F, 8.0F),
                      Vec4f(9.0F, 10.0F, 11.0F, 12.0F), Vec4f(13.0F, 14.0F, 15.0F, 16.0F));
        const Vec4f v(1.0F, 2.0F, 3.0F, 1.0F);
        return a * v;
    };

    BENCHMARK("Mat4f * Mat4f")
    {
        const Mat4f a(Vec4f(1.0F + g_mat4f_bench_bias, 2.0F, 3.0F, 4.0F), Vec4f(5.0F, 6.0F, 7.0F, 8.0F),
                      Vec4f(9.0F, 10.0F, 11.0F, 12.0F), Vec4f(13.0F, 14.0F, 15.0F, 16.0F));
        const Mat4f b(Vec4f(0.5F, 1.5F, 2.5F, 3.5F), Vec4f(4.5F, 5.5F, 6.5F, 7.5F), Vec4f(8.5F, 9.5F, 10.5F, 11.5F),
                      Vec4f(12.5F, 13.5F, 14.5F, 15.5F));
        return a * b;
    };
}

TEST_CASE("Mat4 double workloads", "[bench][math]")
{
    BENCHMARK("Mat4d * Vec4d")
    {
        const Mat4d a(Vec4d(1.0 + g_mat4d_bench_bias, 2.0, 3.0, 4.0), Vec4d(5.0, 6.0, 7.0, 8.0),
                      Vec4d(9.0, 10.0, 11.0, 12.0), Vec4d(13.0, 14.0, 15.0, 16.0));
        const Vec4d v(1.0, 2.0, 3.0, 1.0);
        return a * v;
    };

    BENCHMARK("Mat4d * Mat4d")
    {
        const Mat4d a(Vec4d(1.0 + g_mat4d_bench_bias, 2.0, 3.0, 4.0), Vec4d(5.0, 6.0, 7.0, 8.0),
                      Vec4d(9.0, 10.0, 11.0, 12.0), Vec4d(13.0, 14.0, 15.0, 16.0));
        const Mat4d b(Vec4d(0.5, 1.5, 2.5, 3.5), Vec4d(4.5, 5.5, 6.5, 7.5), Vec4d(8.5, 9.5, 10.5, 11.5),
                      Vec4d(12.5, 13.5, 14.5, 15.5));
        return a * b;
    };
}

TEST_CASE("Quaternion workloads", "[bench][math]")
{
    BENCHMARK("Quatf multiply")
    {
        const Quatf a = from_axis_angle(Vec3f(0.0F, 0.0F, 1.0F), k_half_pi_f + g_quatf_bench_bias);
        const Quatf b = from_axis_angle(Vec3f(0.0F, 1.0F, 0.0F), deg_to_rad(30.0F));
        return a * b;
    };

    BENCHMARK("Quatf rotate Vec3f")
    {
        const Quatf a = from_axis_angle(Vec3f(0.0F, 0.0F, 1.0F), k_half_pi_f + g_quatf_bench_bias);
        const Vec3f v(1.0F, 2.0F, 3.0F);
        return rotate_vector(a, v);
    };
}

TEST_CASE("Transform workloads", "[bench][math]")
{
    BENCHMARK("Transformf compose")
    {
        const Transformf a(Vec3f(1.0F + g_transformf_bench_bias, 2.0F, 3.0F),
                           from_axis_angle(Vec3f(0.0F, 0.0F, 1.0F), k_half_pi_f));
        const Transformf b(Vec3f(-2.0F, 1.0F, 0.5F), from_axis_angle(Vec3f(0.0F, 1.0F, 0.0F), deg_to_rad(30.0F)));
        return a * b;
    };

    BENCHMARK("Transformf point")
    {
        const Transformf a(Vec3f(1.0F + g_transformf_bench_bias, 2.0F, 3.0F),
                           from_axis_angle(Vec3f(0.0F, 0.0F, 1.0F), k_half_pi_f));
        const Vec3f p(0.25F, -0.5F, 0.75F);
        return transform_point(a, p);
    };
}

TEST_CASE("Primitive geometry workloads", "[bench][math]")
{
    const Ray3f ray(Vec3f(0.0F, 0.0F, -5.0F), Vec3f(0.0F, 0.0F, 1.0F));
    const Planef plane = plane_from_point_normal(Vec3f(0.0F, 0.0F, 0.0F), Vec3f(0.0F, 0.0F, 1.0F));
    const Triangle3f tri(Vec3f(-1.0F, -1.0F, 0.0F), Vec3f(1.0F, -1.0F, 0.0F), Vec3f(0.0F, 1.0F, 0.0F));
    const Frustumf frustum = frustum_from_view_projection(Mat4f::identity());
    const AABB3f bounds(Vec3f(-0.5F, -0.5F, -0.5F), Vec3f(0.5F, 0.5F, 0.5F));

    BENCHMARK("Ray3f plane intersection")
    {
        f32 t = 0.0F;
        return intersect_ray_plane(ray, plane, t) ? t : -1.0F;
    };

    BENCHMARK("Ray3f triangle intersection")
    {
        f32 t = 0.0F;
        Vec3f bary{};
        return intersect_ray_triangle(ray, tri, t, bary) ? t + bary.x : -1.0F;
    };

    BENCHMARK("Frustumf AABB3 test")
    {
        return intersects(frustum, bounds);
    };
}
