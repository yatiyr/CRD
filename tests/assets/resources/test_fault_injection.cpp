// DIAG.9b -- seeded fault injection across the resource manager's load and reload paths. crd-perf's FaultInjector
// fails the real seams (the MESH loader's payload try_allocate through a FaultAllocator; every pack read through the
// I/O fault seam) and these cases check the recovery each path promises:
//   - a reload interrupted by an allocation failure, a failed read or a short read keeps the last good payload, its
//     generation and its bytes, and the next clean reload swaps in the new content;
//   - a streamed load whose completion is short, failed or cancelled ends Failed without the loader ever seeing the
//     bytes; a delayed completion still loads.
// A shipping build cannot arm a plan; there the cases assert the refusal and that every path runs plain.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include "scoped_test_file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <crd/core/fault_seam.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/load_state.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <new>

namespace
{
namespace fs   = crd::platform::fs;
namespace cp   = crd::perf;
namespace cont = crd::containers;
using namespace crd::resources;

alignas(crd::memory::GrowableTlsfAllocator) unsigned char s_fi_alloc_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
// Never destroyed: static-destruction-order safe.
crd::memory::GrowableTlsfAllocator& s_fi_alloc = // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    *::new (s_fi_alloc_buf) crd::memory::GrowableTlsfAllocator();

constexpr cp::DiagAuthoritySet kInject = cp::authority_bit(cp::DiagAuthority::Inject);

cp::FaultPlan plan_of(const char* text)
{
    cp::FaultPlan plan;
    cont::String  reason;
    const bool    parsed = cp::parse_fault_plan(cont::StringView{text}, plan, reason);
    INFO(reason.c_str());
    REQUIRE(parsed);
    return plan;
}

fs::Path temp_pack_path(const char* stem, ResourceId id)
{
    cont::String name(stem, &s_fi_alloc);
    name.append(id.to_string(&s_fi_alloc));
    name.append(".crdr");
    return fs::temp_directory() / cont::StringView{name.data(), name.size()};
}

// One artifact in a PACK: the manifest, then the artifact bytes.
void write_pack(const fs::Path& path, ResourceId id, crd::u32 fourcc, const cont::Array<crd::u8>& artifact)
{
    cont::Array<crd::u8> pool(&s_fi_alloc);
    for (const char c : cont::StringView{"fault.art"})
    {
        pool.push_back(static_cast<crd::u8>(c));
    }
    pool.push_back(0U);

    cont::Array<ManifestEntry> entries(&s_fi_alloc);
    ManifestEntry              e;
    e.id            = id;
    e.type_fourcc   = fourcc;
    e.flags         = 0U;
    e.blob_offset   = 0U;
    e.blob_size     = static_cast<crd::u64>(artifact.size());
    e.name_strp_idx = 0U;
    entries.push_back(e);

    const ResourceId pack_id = ResourceId::mint_random();
    {
        CrdrWriter measure(&s_fi_alloc, pack_id, kFourCC_PACK);
        manifest_write(measure, cont::as_const_span(entries), cont::as_const_span(pool));
        entries[0].blob_offset = static_cast<crd::u64>(measure.finish().size());
    }
    CrdrWriter writer(&s_fi_alloc, pack_id, kFourCC_PACK);
    manifest_write(writer, cont::as_const_span(entries), cont::as_const_span(pool));
    cont::Array<crd::u8> bytes = writer.finish();
    for (const crd::u8 b : artifact)
    {
        bytes.push_back(b);
    }
    REQUIRE(fs::write_file_binary(path, cont::as_const_span(bytes)));
}

// A one-triangle MESH artifact whose first vertex's x is `x` (so a reload's new content is visible).
cont::Array<crd::u8> mesh_artifact(ResourceId id, crd::f32 x)
{
    constexpr crd::u32 stride = 48U;
    crd::u8            vertices[3U * stride] = {};
    for (crd::u32 v = 0U; v < 3U; ++v)
    {
        const crd::f32 pos[3] = {v == 0U ? x : static_cast<crd::f32>(v), 0.0F, 0.0F};
        std::memcpy(vertices + v * stride, pos, sizeof(pos));
    }
    const crd::u32 indices[3] = {0U, 1U, 2U};
    crd::u8        prim[4U + 32U] = {};
    const crd::u32 prim_header[5] = {1U, 3U, 3U, 0U, 0U}; // count; vertex count, index count, both offsets 0
    std::memcpy(prim, prim_header, sizeof(prim_header));

    CrdrWriter writer(&s_fi_alloc, id, kFourCC_MESH);
    writer.add_chunk(kFourCC_VERT, cont::ConstSpan<crd::u8>{vertices, sizeof(vertices)});
    writer.add_chunk(kFourCC_INDX,
                     cont::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(indices), sizeof(indices)});
    writer.add_chunk(kFourCC_PRIM, cont::ConstSpan<crd::u8>{prim, sizeof(prim)});
    return writer.finish();
}

crd::f32 first_x(const MeshResource& mesh)
{
    crd::f32 x = 0.0F;
    std::memcpy(&x, mesh.vertices.data(), sizeof(x));
    return x;
}

// A BLOB loader that counts every call, so a case can prove the loader never saw a faulted read's bytes.
struct CountedBlob
{
    cont::Array<crd::u8> bytes;
    explicit CountedBlob(crd::memory::IAllocator* a) : bytes(a) {}
};

struct CountingBlobLoader final : public ILoader
{
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_alloc{&m_inner};
    std::atomic<crd::u32>*             m_calls;

    explicit CountingBlobLoader(std::atomic<crd::u32>* calls) : m_calls(calls) {}

    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_BLOB; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }

    [[nodiscard]] void* load(const LoadContext& ctx) override
    {
        m_calls->fetch_add(1U, std::memory_order_relaxed);
        CrdrFile file(&m_alloc);
        if (crdr_read(ctx.bytes, file, &m_alloc) != CrdrError::Ok)
        {
            return nullptr;
        }
        const CrdrChunk* chunk = crdr_find_chunk(file, kFourCC_BLOB);
        if (chunk == nullptr)
        {
            return nullptr;
        }
        void* raw = m_alloc.allocate(sizeof(CountedBlob), alignof(CountedBlob));
        auto* res = new (raw) CountedBlob(&m_alloc);
        res->bytes.resize(chunk->payload.size());
        std::memcpy(res->bytes.data(), chunk->payload.data(), chunk->payload.size());
        return res;
    }

    void unload(void* payload) noexcept override
    {
        if (payload == nullptr)
        {
            return;
        }
        auto* res = static_cast<CountedBlob*>(payload);
        res->~CountedBlob();
        m_alloc.deallocate(res);
    }
};

cont::Array<crd::u8> blob_artifact(ResourceId id)
{
    crd::u8 payload[64];
    for (crd::u32 i = 0U; i < sizeof(payload); ++i)
    {
        payload[i] = static_cast<crd::u8>(i * 3U + 1U);
    }
    CrdrWriter writer(&s_fi_alloc, id, kFourCC_BLOB);
    writer.add_chunk(kFourCC_BLOB, cont::ConstSpan<crd::u8>{payload, sizeof(payload)});
    return writer.finish();
}

// Installs the injector on the I/O fault seam for one scope.
struct ScopedIoHook
{
    bool installed = false;

    explicit ScopedIoHook(cp::FaultInjector& injector)
        : installed(crd::install_io_fault_hook(&cp::FaultInjector::io_hook, &injector))
    {
    }
    ~ScopedIoHook()
    {
        if (installed)
        {
            crd::uninstall_io_fault_hook();
        }
    }
    ScopedIoHook(const ScopedIoHook&)            = delete;
    ScopedIoHook& operator=(const ScopedIoHook&) = delete;
    ScopedIoHook(ScopedIoHook&&)                 = delete;
    ScopedIoHook& operator=(ScopedIoHook&&)      = delete;
};
} // namespace

TEST_CASE("fault injection: a reload interrupted by an injected fault keeps the last good mesh",
          "[resources][diag9b][fault]")
{
    const ResourceId     id = ResourceId::mint_random();
    const ScopedTestFile pack(temp_pack_path("fault_reload_", id));
    write_pack(pack.path(), id, kFourCC_MESH, mesh_artifact(id, 1.0F));

    cp::FaultInjector            injector;
    crd::memory::GrowableTlsfAllocator payload_heap;
    crd::memory::ThreadSafeAllocator   payload_safe(&payload_heap);
    cp::FaultAllocator           payload(&payload_safe, &injector, "fault-mesh-payload");
    cp::FlightRecorder           recorder;
    cp::FlightPolicy             policy;
    policy.capacity    = 64U;
    policy.pre_events  = 32U;
    policy.post_events = 1U;
    REQUIRE(recorder.init(policy) == cp::FlightPolicyStatus::Committed);
    injector.attach_flight_recorder(&recorder);

    ResourceManager rm(&s_fi_alloc);
    register_mesh_loader(&rm, &payload);
    const MountId mount = rm.mount_manifest(pack.path().generic());
    REQUIRE(mount.is_valid());

    auto handle = rm.load_sync<MeshResource>(id);
    REQUIRE(handle.is_ready());
    const MeshResource* const good       = handle.get();
    const crd::u32            generation = handle.generation();
    REQUIRE(good != nullptr);
    CHECK(first_x(*good) == 1.0F);

    // The pack on disk now holds new content; every interrupted reload must leave the old payload in place.
    write_pack(pack.path(), id, kFourCC_MESH, mesh_artifact(id, 7.0F));

    if (!crd::fault_seams_compiled())
    {
        CHECK(injector.arm(plan_of("seed=1;allocation-failure@0+1%1000000"), kInject) ==
              cp::FaultArmStatus::Unavailable);
        const ScopedIoHook hook(injector);
        CHECK_FALSE(hook.installed);
        CHECK(rm.reload_mount_now(mount) == 1U);
        CHECK(first_x(*handle.get()) == 7.0F);
        recorder.shutdown();
        return;
    }

    const auto check_last_good = [&](const char* what)
    {
        INFO(what);
        CHECK(handle.state() == LoadState::Ready);
        const MeshResource* const now = handle.get();
        CHECK(now == good);
        CHECK(handle.generation() == generation);
        REQUIRE(now != nullptr);
        CHECK(first_x(*now) == 1.0F);
        CHECK(now->vertices.size() == 3U * 48U);
    };

    // An allocation failure in the real MESH loader (its payload try_allocate).
    REQUIRE(injector.arm(plan_of("seed=1;allocation-failure@0+1%1000000"), kInject) == cp::FaultArmStatus::Armed);
    CHECK(rm.reload_mount_now(mount) == 0U);
    CHECK(injector.fired(cp::FaultSite::AllocationFailure) == 1U);
    CHECK(payload.injected() == 1U);
    check_last_good("allocation failure");
    recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"after the reload"}); // closes the post window
    cp::FlightCapture capture;
    REQUIRE(recorder.take_capture(capture));
    CHECK(capture.trigger.kind == cp::FlightTrigger::InjectedFault);
    CHECK(cont::StringView{capture.replay.data(), capture.replay.size()}.find(
              "fired=allocation-failure#0 minimal=seed=1;allocation-failure@0+1%1000000") != cont::StringView::npos);

    {
        const ScopedIoHook hook(injector);
        REQUIRE(hook.installed);

        // The reload's pack read fails outright.
        REQUIRE(injector.arm(plan_of("seed=2;failed-read@0+1%1000000"), kInject) == cp::FaultArmStatus::Armed);
        CHECK(rm.reload_mount_now(mount) == 0U);
        CHECK(injector.fired(cp::FaultSite::FailedRead) == 1U);
        check_last_good("failed read");

        // The reload's pack read stops after 16 bytes.
        REQUIRE(injector.arm(plan_of("seed=3;partial-read@0+1%1000000=16"), kInject) == cp::FaultArmStatus::Armed);
        CHECK(rm.reload_mount_now(mount) == 0U);
        CHECK(injector.fired(cp::FaultSite::PartialRead) == 1U);
        check_last_good("partial read");
    }

    // Disarmed and unhooked, the same reload swaps the new content in.
    injector.disarm();
    CHECK(rm.reload_mount_now(mount) == 1U);
    CHECK(handle.state() == LoadState::Ready);
    CHECK(handle.generation() == generation + 1U);
    CHECK(first_x(*handle.get()) == 7.0F);
    recorder.shutdown();
}

TEST_CASE("fault injection: a streamed load whose completion is short, failed or cancelled never reaches the loader",
          "[resources][diag9b][fault]")
{
    // crd::jobs is initialised by the ResourcesJobsListener in test_resource_manager.cpp (same binary).
    const ResourceId     id = ResourceId::mint_random();
    const ScopedTestFile pack(temp_pack_path("fault_stream_", id));
    write_pack(pack.path(), id, kFourCC_BLOB, blob_artifact(id));

    // Mount, arm `plan`, hook the injector, stream the artifact; a Ready load must hold the artifact's bytes.
    const auto load_once = [&](cp::FaultInjector& injector, const char* plan, std::atomic<crd::u32>& calls)
    {
        ResourceManager rm(&s_fi_alloc);
        rm.register_loader(std::make_unique<CountingBlobLoader>(&calls));
        REQUIRE(rm.mount_manifest(pack.path().generic()).is_valid()); // the mount's own read happens unfaulted
        REQUIRE(injector.arm(plan_of(plan), kInject) == cp::FaultArmStatus::Armed);
        const ScopedIoHook hook(injector);
        REQUIRE(hook.installed);
        auto               handle = rm.load_streamed<CountedBlob>(id);
        const LoadState    state  = handle.wait_ready();
        if (state == LoadState::Ready)
        {
            REQUIRE(handle.get() != nullptr);
            REQUIRE(handle.get()->bytes.size() == 64U);
            CHECK(handle.get()->bytes[63] == static_cast<crd::u8>(63U * 3U + 1U));
        }
        return state;
    };

    cp::FaultInjector injector;
    if (!crd::fault_seams_compiled())
    {
        CHECK(injector.arm(plan_of("seed=1;failed-read@0+1%1000000"), kInject) == cp::FaultArmStatus::Unavailable);
        std::atomic<crd::u32> calls{0U};
        ResourceManager       rm(&s_fi_alloc);
        rm.register_loader(std::make_unique<CountingBlobLoader>(&calls));
        REQUIRE(rm.mount_manifest(pack.path().generic()).is_valid());
        auto handle = rm.load_streamed<CountedBlob>(id);
        CHECK(handle.wait_ready() == LoadState::Ready);
        CHECK(calls.load() == 1U);
        return;
    }

    {
        std::atomic<crd::u32> calls{0U};
        // Control: the plan covers a later read only, so this one load is clean and the loader runs once.
        CHECK(load_once(injector, "seed=1;failed-read@5+1%1000000", calls) == LoadState::Ready);
        CHECK(calls.load() == 1U);
    }

    const char* const faulted[] = {
        "seed=4;partial-read@0+1%1000000=8",
        "seed=5;failed-read@0+1%1000000",
        "seed=6;cancelled-completion@0+1%1000000",
    };
    for (const char* plan : faulted)
    {
        INFO(plan);
        std::atomic<crd::u32> calls{0U};
        CHECK(load_once(injector, plan, calls) == LoadState::Failed);
        CHECK(calls.load() == 0U);
        CHECK(injector.hits(cp::FaultSite::FailedRead) == 1U); // exactly one read: the streamed completion
    }

    {
        std::atomic<crd::u32> calls{0U};
        const auto            t0    = std::chrono::steady_clock::now();
        const LoadState       state = load_once(injector, "seed=7;delayed-completion@0+1%1000000=20000000", calls);
        CHECK(state == LoadState::Ready);
        CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(20));
        CHECK(calls.load() == 1U);
        CHECK(injector.fired(cp::FaultSite::DelayedCompletion) == 1U);
    }
    injector.disarm();
}
