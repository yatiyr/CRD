#include <crd/core/core.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/memory.hpp>

CRD_DEFINE_LOG_CHANNEL(g_log_runtime_memory, "RuntimeMemory", crd::log::LogLevel::Trace)

int main()
{
    crd::log::init();
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    using namespace crd::memory;

    auto* heap = default_allocator();
    auto* p = construct<crd::u32>(*heap, 0xDEADBEEFU);
    CRD_LOG_INFO(g_log_runtime_memory, "default_allocator handed out {:p} (= 0x{:08X})", static_cast<const void*>(p),
                 *p);
    destroy(*heap, p);

    LinearAllocator frame_scratch(64 * 1024, heap, "FrameScratch");
    {
        LinearScope scope(frame_scratch);
        auto* tmp = allocate_array<crd::f32>(frame_scratch, 256);
        CRD_LOG_INFO(g_log_runtime_memory, "frame scratch: 256 floats at {:p}, used={} bytes",
                     static_cast<const void*>(tmp), frame_scratch.used());
    }
    CRD_LOG_INFO(g_log_runtime_memory, "frame scratch after scope: used={} bytes", frame_scratch.used());

    struct Particle
    {
        crd::f32 x, y, z, life;
    };

    PoolAllocator particles(sizeof(Particle), 64, alignof(Particle), heap, "ParticlePool");
    Particle* a = static_cast<Particle*>(particles.allocate(sizeof(Particle), alignof(Particle)));
    Particle* b = static_cast<Particle*>(particles.allocate(sizeof(Particle), alignof(Particle)));
    CRD_LOG_INFO(g_log_runtime_memory, "particle pool: {}/{} slots in use", particles.slots_in_use(),
                 particles.slot_count());
    particles.deallocate(a);
    particles.deallocate(b);

    const auto stats = heap->stats().snapshot();
    CRD_LOG_INFO(g_log_runtime_memory, "heap stats: alloc={} dealloc={} bytes_in_use={} peak={}", stats.alloc_count,
                 stats.dealloc_count, stats.bytes_in_use, stats.peak_bytes);

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
