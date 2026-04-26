#include <crd/containers/containers.hpp>
#include <crd/core/core.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/memory.hpp>

#include <chrono>
#include <iostream>
#include <thread>

// Demo channels: imagine these belonging to future engine subsystems.
// In a real setup, each subsystem owns its own channel via CRD_DEFINE_LOG_CHANNEL
// inside its own .cpp file. We co-locate them here for the smoke test.
CRD_DEFINE_LOG_CHANNEL(g_log_engine, "Engine", crd::log::LogLevel::Trace)
CRD_DEFINE_LOG_CHANNEL(g_log_renderer, "Renderer", crd::log::LogLevel::Info)
CRD_DEFINE_LOG_CHANNEL(g_log_physics, "Physics", crd::log::LogLevel::Debug)
CRD_DEFINE_LOG_CHANNEL(g_log_audio, "Audio", crd::log::LogLevel::Warn)

int main()
{
    using namespace std::chrono_literals;

    const crd::u32 version_major = CRD_VERSION_MAJOR;
    const crd::u32 version_minor = CRD_VERSION_MINOR;
    const crd::u32 version_patch = CRD_VERSION_PATCH;

    // ---- Bring up the logger ------------------------------------------
    crd::log::LoggerConfig cfg;
    cfg.async = true;
    cfg.async_queue_capacity = 1024;
    cfg.flush_on_critical = true;
    crd::log::init(cfg);

    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());
    crd::log::add_sink(
        std::make_unique<crd::log::FileSink>("engine.log", /*max_bytes*/ 1ull * 1024ull * 1024ull, /*max_files*/ 3));

    // ---- Smoke test ---------------------------------------------------
    CRD_LOG_INFO(g_log_engine, "CRD Engine v{}.{}.{} starting up", version_major, version_minor, version_patch);
    CRD_LOG_INFO(g_log_engine, "platform={} compiler={} arch={}", crd::platform_name(), crd::compiler_name(),
                 crd::arch_name());

    CRD_LOG_TRACE(g_log_engine, "trace: this is the chattiest level");
    CRD_LOG_DEBUG(g_log_engine, "debug: useful while building things");

    CRD_LOG_INFO(g_log_renderer, "Renderer initialised, backend={}", "Vulkan");
    CRD_LOG_DEBUG(g_log_physics, "physics tick budget = {} ms", 4);
    CRD_LOG_WARN(g_log_audio, "no audio device found, falling back to silence");

    // ---- Memory subsystem smoke test --------------------------------
    {
        using namespace crd::memory;

        // Default heap allocator -> grab/release a small struct.
        auto* heap = default_allocator();
        auto* p = construct<crd::u32>(*heap, 0xDEADBEEFu);
        CRD_LOG_INFO(g_log_engine, "default_allocator handed out {:p} (= 0x{:08X})", static_cast<const void*>(p), *p);
        destroy(*heap, p);

        // Frame-scoped scratch with LinearAllocator.
        LinearAllocator frame_scratch(64 * 1024, heap, "FrameScratch");
        {
            LinearScope scope(frame_scratch);
            auto* tmp = allocate_array<crd::f32>(frame_scratch, 256);
            CRD_LOG_INFO(g_log_engine, "frame scratch: 256 floats at {:p}, used={} bytes",
                         static_cast<const void*>(tmp), frame_scratch.used());
        }
        CRD_LOG_INFO(g_log_engine, "frame scratch after scope: used={} bytes", frame_scratch.used());

        // ECS-style fixed-size pool.
        struct Particle
        {
            crd::f32 x, y, z, life;
        };
        PoolAllocator particles(sizeof(Particle), 64, alignof(Particle), heap, "ParticlePool");
        Particle* a = static_cast<Particle*>(particles.allocate(sizeof(Particle), alignof(Particle)));
        Particle* b = static_cast<Particle*>(particles.allocate(sizeof(Particle), alignof(Particle)));
        CRD_LOG_INFO(g_log_engine, "particle pool: {}/{} slots in use", particles.slots_in_use(),
                     particles.slot_count());
        particles.deallocate(a);
        particles.deallocate(b);

        const auto stats = heap->stats().snapshot();
        CRD_LOG_INFO(g_log_engine, "heap stats: alloc={} dealloc={} bytes_in_use={} peak={}", stats.alloc_count,
                     stats.dealloc_count, stats.bytes_in_use, stats.peak_bytes);
    }

    // ---- Container subsystem smoke test ----------------------------
    {
        using namespace crd::containers;

        // Heap-backed Array growing past initial capacity.
        Array<crd::u32> ids;
        for (crd::u32 i = 0; i < 10; ++i)
        {
            ids.push_back(i * i);
        }
        CRD_LOG_INFO(g_log_engine, "Array<u32>: size={} capacity={} front={} back={}", ids.size(), ids.capacity(),
                     ids.front(), ids.back());

        // FixedArray, stack-only, no allocator.
        FixedArray<const char*, 4> tags;
        tags.push_back("renderer");
        tags.push_back("physics");
        tags.push_back("audio");
        CRD_LOG_INFO(g_log_engine, "FixedArray<const char*, 4>: size={}/{} full?={}", tags.size(), tags.capacity(),
                     tags.full());

        // Linear-allocator-backed Array using try_push_back to detect exhaustion.
        crd::memory::LinearAllocator scratch(256, crd::memory::default_allocator(), "ContainerScratch");
        Array<crd::u32> scratch_array(&scratch);
        int pushed = 0;
        while (scratch_array.try_push_back(static_cast<crd::u32>(pushed)))
        {
            ++pushed;
        }
        CRD_LOG_INFO(g_log_engine, "Linear-backed Array: pushed {} u32s before exhaustion (scratch used={}/{})", pushed,
                     scratch.used(), scratch.capacity());

        // Hash defaults at work.
        CRD_LOG_INFO(g_log_engine, "hash_u64(42) = 0x{:016X}, fnv1a('cerid') = 0x{:016X}", hash_u64(42),
                     hash_string(std::string_view{"cerid"}));

        // ---- v1b: String + RingBuffer ------------------------------
        // Small string stays in SSO; appending past 23 bytes promotes to heap.
        String greeting("hello, ");
        greeting.append(StringView{"cerid"});
        CRD_LOG_INFO(g_log_engine, "String '{}' size={} sso?={}", greeting.c_str(), greeting.size(),
                     greeting.is_small());

        // Heterogeneous hash sanity: String and StringView hash equally.
        CRD_LOG_INFO(g_log_engine, "hash(String)={:016X} hash(StringView)={:016X}", DefaultHash<String>{}(greeting),
                     DefaultHash<StringView>{}(StringView{greeting}));

        // RingBuffer: power-of-two capacity, FIFO, refuses overflow.
        RingBuffer<crd::u32> events(8);
        for (crd::u32 i = 0; i < 12; ++i)
        {
            (void)events.try_push(i);
        }
        Array<crd::u32> events_snap;
        events.snapshot(events_snap);
        CRD_LOG_INFO(g_log_engine, "RingBuffer<u32>(cap=8): size={} full?={} accepted=8/12, snapshot.front={} back={}",
                     events.size(), events.full(), events_snap.front(), events_snap.back());

        // ---- v1c: HashMap + HashSet --------------------------------
        // String-keyed asset version table; lookup by StringView (no temp).
        HashMap<String, crd::u32> asset_versions;
        asset_versions.insert(String("mesh.obj"), 3);
        asset_versions.insert(String("shader.frag"), 7);
        asset_versions.insert(String("texture.png"), 11);
        const auto* mesh_v = asset_versions.find(StringView{"mesh.obj"});
        CRD_LOG_INFO(g_log_engine, "HashMap<String,u32>: size={} cap={} load_factor={:.3f} mesh.obj v={}",
                     asset_versions.size(), asset_versions.capacity(), asset_versions.load_factor(),
                     mesh_v ? *mesh_v : 0u);

        // HashSet of seen tags.
        HashSet<crd::u32> seen_tags;
        for (crd::u32 i = 0; i < 100; ++i)
        {
            (void)seen_tags.insert(i % 17);
        }
        CRD_LOG_INFO(g_log_engine, "HashSet<u32>: size={} (after 100 inserts mod 17)", seen_tags.size());
    }

    // Filtered: Audio's runtime level is Warn, so this Info is dropped at producer.
    CRD_LOG_INFO(g_log_audio, "(this should NOT appear)");

    // Cross-thread: pretend a couple of worker threads emit logs.
    std::thread t1(
        []
        {
            for (int i = 0; i < 3; ++i)
            {
                CRD_LOG_INFO(g_log_renderer, "frame {} submitted", i);
                std::this_thread::sleep_for(2ms);
            }
        });
    std::thread t2(
        []
        {
            for (int i = 0; i < 3; ++i)
            {
                CRD_LOG_DEBUG(g_log_physics, "stepped {} bodies", 32 + i);
                std::this_thread::sleep_for(2ms);
            }
        });
    t1.join();
    t2.join();

    CRD_LOG_ERROR(g_log_renderer, "shader compile failed: '{}'", "tonemap.frag");
    CRD_LOG_CRITICAL(g_log_engine, "fatal subsystem failure simulated -- shutting down");

    crd::log::flush();
    crd::log::shutdown();

    std::cout << "(runtime exiting cleanly)\n";
    return 0;
}
