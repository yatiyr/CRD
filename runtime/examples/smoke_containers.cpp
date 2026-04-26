#include <crd/containers/containers.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/memory.hpp>

CRD_DEFINE_LOG_CHANNEL(g_log_runtime_containers, "RuntimeContainers", crd::log::LogLevel::Trace)

int main()
{
    crd::log::init();
    crd::log::add_sink(std::make_unique<crd::log::ConsoleSink>());

    using namespace crd::containers;

    Array<crd::u32> ids;
    for (crd::u32 i = 0; i < 10; ++i)
    {
        ids.push_back(i * i);
    }
    CRD_LOG_INFO(g_log_runtime_containers, "Array<u32>: size={} capacity={} front={} back={}", ids.size(),
                 ids.capacity(), ids.front(), ids.back());

    FixedArray<const char*, 4> tags;
    tags.push_back("renderer");
    tags.push_back("physics");
    tags.push_back("audio");
    CRD_LOG_INFO(g_log_runtime_containers, "FixedArray<const char*, 4>: size={}/{} full?={}", tags.size(),
                 tags.capacity(), tags.full());

    crd::memory::LinearAllocator scratch(256, crd::memory::default_allocator(), "ContainerScratch");
    Array<crd::u32> scratch_array(&scratch);
    int pushed = 0;
    while (scratch_array.try_push_back(static_cast<crd::u32>(pushed)))
    {
        ++pushed;
    }
    CRD_LOG_INFO(g_log_runtime_containers, "Linear-backed Array: pushed {} u32s before exhaustion (scratch used={}/{})",
                 pushed, scratch.used(), scratch.capacity());

    CRD_LOG_INFO(g_log_runtime_containers, "hash_u64(42) = 0x{:016X}, fnv1a('cerid') = 0x{:016X}", hash_u64(42),
                 hash_string(std::string_view{"cerid"}));

    String greeting("hello, ");
    greeting.append(StringView{"cerid"});
    CRD_LOG_INFO(g_log_runtime_containers, "String '{}' size={} sso?={}", greeting.c_str(), greeting.size(),
                 greeting.is_small());

    CRD_LOG_INFO(g_log_runtime_containers, "hash(String)={:016X} hash(StringView)={:016X}",
                 DefaultHash<String>{}(greeting), DefaultHash<StringView>{}(StringView{greeting}));

    RingBuffer<crd::u32> events(8);
    for (crd::u32 i = 0; i < 12; ++i)
    {
        (void)events.try_push(i);
    }
    Array<crd::u32> events_snap;
    events.snapshot(events_snap);
    CRD_LOG_INFO(g_log_runtime_containers,
                 "RingBuffer<u32>(cap=8): size={} full?={} accepted=8/12, snapshot.front={} back={}", events.size(),
                 events.full(), events_snap.front(), events_snap.back());

    HashMap<String, crd::u32> asset_versions;
    asset_versions.insert(String("mesh.obj"), 3);
    asset_versions.insert(String("shader.frag"), 7);
    asset_versions.insert(String("texture.png"), 11);
    const auto* mesh_v = asset_versions.find(StringView{"mesh.obj"});
    CRD_LOG_INFO(g_log_runtime_containers, "HashMap<String,u32>: size={} cap={} load_factor={:.3f} mesh.obj v={}",
                 asset_versions.size(), asset_versions.capacity(), asset_versions.load_factor(), mesh_v ? *mesh_v : 0u);

    HashSet<crd::u32> seen_tags;
    for (crd::u32 i = 0; i < 100; ++i)
    {
        (void)seen_tags.insert(i % 17);
    }
    CRD_LOG_INFO(g_log_runtime_containers, "HashSet<u32>: size={} (after 100 inserts mod 17)", seen_tags.size());

    crd::log::flush();
    crd::log::shutdown();
    return 0;
}
