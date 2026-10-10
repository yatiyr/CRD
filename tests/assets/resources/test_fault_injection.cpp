// DIAG.9b -- seeded fault injection across the resource manager's load and reload paths. crd-perf's FaultInjector
// fails the real seams (the MESH loader's payload try_allocate through a FaultAllocator; every pack read through the
// I/O fault seam) and these cases check the recovery each path promises:
//   - a reload interrupted by an allocation failure, a failed read or a short read keeps the last good payload, its
//     generation and its bytes, and the next clean reload swaps in the new content;
//   - a streamed load whose completion is short, failed or cancelled ends Failed without the loader ever seeing the
//     bytes; a delayed completion still loads.
//   - a cook (the asset processor, crd-cooker's cmd_cook) interrupted by a failed or short read of an asset's source
//     or .meta, or of an input that exists, publishes nothing: the previous pack, cache artifact, cook database and
//     asset identity are kept byte for byte, the flight capture names the file, and the next clean cook publishes the
//     new content under the same id; a fault in the up-to-date check recooks the same bytes, never ships stale ones.
//   - an install (mounting a newly cooked pack over a mounted one) interrupted by a failed or short read, or offered a
//     pack torn on disk, mounts nothing: the mounted pack keeps serving and its loaded payload is kept;
//   - a teardown after a run full of fired faults performs no faultable read or allocation, returns every allocation
//     the run made, drains a delayed completion still in flight, and the recorder delivers its open window truncated.
//   - (DIAG.9c) a seeded read plan that fails a cook after the cook recovered from earlier faults is minimized
//     against the real cooker to a seed-free plan of single reads, each needed (checked by cooking without it).
// A shipping build cannot arm a plan; there the cases assert the refusal and that every path runs plain.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b, DIAG.9c); ADR-0133.

#include "scoped_test_file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <crd/cooker/cook_command.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/minimize.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/fault_minimize.hpp>
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
#include <cstdio>
#include <cstring>
#include <initializer_list>
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

// ---- the cook stage ---------------------------------------------------------------------------------------------

namespace
{
namespace ck = crd::cooker;

// A cook tree under the temp directory: <root>/src is the cook root, <root>/pack.crdr the pack (cook.log.toml lands
// beside it). The whole tree is removed when the case ends, however it ends.
struct CookTree
{
    fs::Path root;
    fs::Path src;
    fs::Path pack;

    CookTree()
    {
        cont::String leaf("crd_fault_cook_", &s_fi_alloc);
        leaf.append(ResourceId::mint_random().to_string(&s_fi_alloc));
        root = fs::temp_directory() / cont::StringView{leaf.data(), leaf.size()};
        src  = root / "src";
        pack = root / "pack.crdr";
        REQUIRE(fs::create_directories(src));
    }
    ~CookTree() { (void)fs::remove_all(root); }
    CookTree(const CookTree&)            = delete;
    CookTree& operator=(const CookTree&) = delete;
    CookTree(CookTree&&)                 = delete;
    CookTree& operator=(CookTree&&)      = delete;

    [[nodiscard]] int cook() const { return ck::cmd_cook(src.generic().data(), pack.generic().data()); }
    [[nodiscard]] fs::Path cache(const char* name) const { return src / ".cook_cache" / name; }
};

void write_source(const fs::Path& path, std::initializer_list<crd::u8> bytes)
{
    cont::Array<crd::u8> data(&s_fi_alloc);
    for (const crd::u8 b : bytes)
    {
        data.push_back(b);
    }
    REQUIRE(fs::write_file_binary(path, cont::as_const_span(data)));
}

// The file's bytes, read with no hook installed (an empty array when it cannot be read).
cont::Array<crd::u8> bytes_of(const fs::Path& path)
{
    cont::Array<crd::u8> out(&s_fi_alloc);
    if (!fs::read_file_binary(path, out))
    {
        out.clear();
    }
    return out;
}

bool same_bytes(const cont::Array<crd::u8>& a, const cont::Array<crd::u8>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// The id an asset's .meta holds.
ResourceId meta_id(const fs::Path& meta)
{
    const cont::Array<crd::u8> text = bytes_of(meta);
    const cont::StringView     view{reinterpret_cast<const char*>(text.data()), text.size()};
    const crd::usize           at = view.find("uuid = \"");
    REQUIRE(at != cont::StringView::npos);
    const crd::usize begin = at + 8U;
    const crd::usize end   = view.find('"', begin);
    REQUIRE(end != cont::StringView::npos);
    return ResourceId::parse(view.substr(begin, end - begin));
}

// The ids a pack's manifest lists, in pack order.
cont::Array<ResourceId> pack_ids(const fs::Path& pack)
{
    const cont::Array<crd::u8> bytes = bytes_of(pack);
    CrdrFile                   file(&s_fi_alloc);
    REQUIRE(crdr_read(cont::as_const_span(bytes), file, &s_fi_alloc) == CrdrError::Ok);
    const CrdrChunk* mfst = crdr_find_chunk(file, kFourCC_MFST);
    REQUIRE(mfst != nullptr);
    cont::Array<ManifestEntry> entries(&s_fi_alloc);
    REQUIRE(manifest_read_entries(mfst->payload, entries, &s_fi_alloc));
    cont::Array<ResourceId> ids(&s_fi_alloc);
    for (const ManifestEntry& e : entries)
    {
        ids.push_back(e.id);
    }
    return ids;
}

// Everything a failed cook must leave as it was: the pack, the asset's .meta, the cook database and the cached
// artifact of the asset's id.
struct CookSnapshot
{
    cont::Array<crd::u8> pack{&s_fi_alloc};
    cont::Array<crd::u8> meta{&s_fi_alloc};
    cont::Array<crd::u8> db{&s_fi_alloc};
    cont::Array<crd::u8> artifact{&s_fi_alloc};
};

CookSnapshot snapshot(const CookTree& tree, const fs::Path& meta)
{
    CookSnapshot s;
    s.pack = bytes_of(tree.pack);
    s.meta = bytes_of(meta);
    s.db   = bytes_of(tree.cache("cookdb.toml"));
    cont::String artifact(&s_fi_alloc);
    artifact.append(meta_id(meta).to_string(&s_fi_alloc));
    artifact.append(".crdr");
    s.artifact = bytes_of(tree.cache(artifact.c_str()));
    REQUIRE_FALSE(s.pack.empty());
    REQUIRE_FALSE(s.db.empty());
    REQUIRE_FALSE(s.artifact.empty());
    return s;
}

void check_unchanged(const CookSnapshot& before, const CookSnapshot& now)
{
    CHECK(same_bytes(before.pack, now.pack));
    CHECK(same_bytes(before.meta, now.meta));
    CHECK(same_bytes(before.db, now.db));
    CHECK(same_bytes(before.artifact, now.artifact));
}

// The flight event message of the one fault a capture holds, and the one-hit plan its recipe names.
struct FaultEvidence
{
    cont::String message{&s_fi_alloc};
    cont::String minimal{&s_fi_alloc};
};

FaultEvidence take_fault_evidence(cp::FlightRecorder& recorder)
{
    recorder.record(0x0042U, cp::Severity::Info, cont::StringView{"after the cook"}); // closes the post window
    cp::FlightCapture capture;
    REQUIRE(recorder.take_capture(capture));
    CHECK(capture.trigger.kind == cp::FlightTrigger::InjectedFault);
    // The ring keeps earlier runs' faults in the pre window; this run's fault is the event recorded just before the
    // trigger, and no fault follows it.
    FaultEvidence           out;
    const cp::FlightEvent*  fault = nullptr;
    crd::u32                after = 0U;
    for (const cp::FlightEvent& e : capture.events)
    {
        if (e.code != cp::kFlightCodeFaultInjected)
        {
            continue;
        }
        if (e.sequence < capture.trigger.sequence)
        {
            fault = &e;
        }
        else
        {
            ++after;
        }
    }
    REQUIRE(fault != nullptr);
    CHECK(fault->sequence + 1U == capture.trigger.sequence);
    CHECK(after == 0U);
    out.message.append(fault->message_view());
    const cont::StringView recipe{capture.replay.data(), capture.replay.size()};
    const crd::usize       at = recipe.find("minimal=");
    REQUIRE(at != cont::StringView::npos);
    const cont::StringView rest = recipe.substr(at + 8U);
    const crd::usize       end  = rest.find(' ');
    out.minimal.append(rest.substr(0U, end == cont::StringView::npos ? rest.size() : end));
    return out;
}

bool ends_with(const cont::String& text, const char* tail)
{
    return cont::StringView{text.data(), text.size()}.ends_with(cont::StringView{tail});
}

// A handler whose input is optional: the product is the source's bytes followed by "extra.dat"'s when that file is
// there. Recording absence is right for it, so only the processor's unreadable rule tells a failed read of an
// existing extra.dat from no extra.dat at all.
ck::CookResult optional_input_handler(const ck::CookContext& ctx)
{
    ck::CookResult       result(ctx.allocator);
    cont::Array<crd::u8> bytes(ctx.allocator);
    if (!ctx.io->read_source(bytes))
    {
        return result;
    }
    cont::Array<crd::u8> extra(ctx.allocator);
    if (ctx.io->read_input(cont::StringView{"extra.dat"}, extra))
    {
        for (const crd::u8 b : extra)
        {
            bytes.push_back(b);
        }
    }
    CrdrWriter writer(ctx.allocator, ctx.id, kFourCC_BLOB);
    writer.add_chunk(kFourCC_BLOB, cont::as_const_span(bytes));
    result.type_fourcc     = kFourCC_BLOB;
    result.cooked_bytes    = writer.finish();
    result.handler_version = 1U;
    result.ok              = true;
    return result;
}

// The BLOB chunk of the cached artifact of `id`.
cont::Array<crd::u8> cached_blob(const CookTree& tree, ResourceId id)
{
    cont::String name(&s_fi_alloc);
    name.append(id.to_string(&s_fi_alloc));
    name.append(".crdr");
    const cont::Array<crd::u8> bytes = bytes_of(tree.cache(name.c_str()));
    CrdrFile                   file(&s_fi_alloc);
    REQUIRE(crdr_read(cont::as_const_span(bytes), file, &s_fi_alloc) == CrdrError::Ok);
    const CrdrChunk* chunk = crdr_find_chunk(file, kFourCC_BLOB);
    REQUIRE(chunk != nullptr);
    cont::Array<crd::u8> out(&s_fi_alloc);
    for (const crd::u8 b : chunk->payload)
    {
        out.push_back(b);
    }
    return out;
}
} // namespace

TEST_CASE("fault injection: a cook interrupted by a failed or short read publishes nothing and keeps the asset's id",
          "[resources][diag9b][fault][cook]")
{
    ck::register_builtin_handlers();
    const CookTree tree;
    const fs::Path source = tree.src / "a.bin";
    const fs::Path meta   = tree.src / "a.bin.meta";
    write_source(source, {1U, 2U, 3U});
    REQUIRE(tree.cook() == 0);
    const ResourceId   id   = meta_id(meta);
    const CookSnapshot good = snapshot(tree, meta);
    write_source(source, {9U, 9U, 9U, 9U}); // the next cook has new content to publish

    cp::FaultInjector injector;
    if (!crd::fault_seams_compiled())
    {
        CHECK(injector.arm(plan_of("seed=1;failed-read@4+1%1000000"), kInject) == cp::FaultArmStatus::Unavailable);
        const ScopedIoHook hook(injector);
        CHECK_FALSE(hook.installed);
        CHECK(tree.cook() == 0);
        CHECK(meta_id(meta) == id);
        CHECK_FALSE(same_bytes(bytes_of(tree.pack), good.pack));
        return;
    }

    cp::FlightRecorder recorder;
    cp::FlightPolicy   policy;
    policy.capacity    = 64U;
    policy.pre_events  = 32U;
    policy.post_events = 1U;
    REQUIRE(recorder.init(policy) == cp::FlightPolicyStatus::Committed);
    injector.attach_flight_recorder(&recorder);

    // The edited run reads, in order: the cook database (0) and journal (1), the .meta for the asset's id (2), the
    // source in the up-to-date check (3, changed, so the check stops), then the cook's own source (4) and .meta (5).
    // A cook that stops after starting the asset's job leaves its journal `begin` dangling, which makes the next run
    // distrust the job and skip the up-to-date check; each faulted run below starts from the same state (no journal),
    // so the recipe it leaves replays the same sequence of reads.
    struct Fault
    {
        const char* plan;
        const char* file;    // the file the flight event must name
        bool        started; // whether the job had begun (its journal `begin` is left dangling)
    };
    const Fault faults[] = {
        {"seed=11;failed-read@4+1%1000000", "a.bin", true},       // the source the cook reads
        {"seed=12;partial-read@4+1%1000000=2", "a.bin", true},    // the same read, cut short
        {"seed=13;failed-read@2+1%1000000", "a.bin.meta", false}, // the .meta that holds the asset's id
        {"seed=14;failed-read@5+1%1000000", "a.bin.meta", true},  // the .meta as a recorded cook input
    };
    const fs::Path journal   = tree.cache("journal.log");
    const auto     run_fault = [&](const Fault& f, const char* plan)
    {
        REQUIRE(injector.arm(plan_of(plan), kInject) == cp::FaultArmStatus::Armed);
        int rc = 0;
        {
            const ScopedIoHook hook(injector);
            REQUIRE(hook.installed);
            rc = tree.cook();
        }
        CHECK(rc != 0);
        CHECK(injector.fired(cp::FaultSite::FailedRead) + injector.fired(cp::FaultSite::PartialRead) == 1U);
        check_unchanged(good, snapshot(tree, meta));
        CHECK(meta_id(meta) == id);
        const cont::Array<crd::u8> log = bytes_of(journal);
        CHECK(cont::StringView{reinterpret_cast<const char*>(log.data()), log.size()} ==
              cont::StringView{f.started ? "begin a.bin\n" : ""});
        (void)fs::remove_file(journal);
        const FaultEvidence evidence = take_fault_evidence(recorder);
        cont::String        tail(" on ", &s_fi_alloc);
        tail.append(f.file);
        CHECK(ends_with(evidence.message, tail.c_str()));
        return evidence;
    };
    for (const Fault& f : faults)
    {
        INFO(f.plan);
        const FaultEvidence evidence = run_fault(f, f.plan);
        // The recipe's one-hit plan reproduces the same interrupted cook on its own.
        INFO(evidence.minimal.c_str());
        (void)run_fault(f, evidence.minimal.c_str());
    }

    // A failed cook leaves the dangling `begin`; the next clean cook recooks the distrusted job and publishes the new
    // content under the same id.
    REQUIRE(injector.arm(plan_of("seed=15;failed-read@4+1%1000000"), kInject) == cp::FaultArmStatus::Armed);
    {
        const ScopedIoHook hook(injector);
        REQUIRE(hook.installed);
        CHECK(tree.cook() != 0);
    }
    CHECK(fs::exists(journal));
    (void)take_fault_evidence(recorder);
    injector.disarm();
    REQUIRE(tree.cook() == 0);
    CHECK_FALSE(fs::exists(journal));
    CHECK(meta_id(meta) == id);
    const cont::Array<ResourceId> ids = pack_ids(tree.pack);
    REQUIRE(ids.size() == 1U);
    CHECK(ids[0] == id);
    CHECK_FALSE(same_bytes(bytes_of(tree.pack), good.pack));
    recorder.shutdown();
}

TEST_CASE("fault injection: a read fault in the cook's up-to-date check recooks the same product, never a stale one",
          "[resources][diag9b][fault][cook]")
{
    ck::register_builtin_handlers();
    const CookTree tree;
    const fs::Path meta = tree.src / "a.bin.meta";
    write_source(tree.src / "a.bin", {4U, 5U, 6U});
    REQUIRE(tree.cook() == 0);
    const CookSnapshot good = snapshot(tree, meta);

    cp::FaultInjector injector;
    if (!crd::fault_seams_compiled())
    {
        CHECK(injector.arm(plan_of("seed=1;failed-read@3+1%1000000"), kInject) == cp::FaultArmStatus::Unavailable);
        CHECK(tree.cook() == 0);
        check_unchanged(good, snapshot(tree, meta));
        return;
    }

    // An unchanged run reads the database (0), journal (1) and .meta (2), checks the source (3) and .meta (4), checks
    // the cached artifact (5) and, when every check holds, reads that artifact into the pack (6).
    const char* const recook[] = {
        "seed=21;failed-read@3+1%1000000", // the source looks absent: the job recooks
        "seed=22;failed-read@5+1%1000000", // the cached artifact looks torn: the job recooks
    };
    for (const char* plan : recook)
    {
        INFO(plan);
        REQUIRE(injector.arm(plan_of(plan), kInject) == cp::FaultArmStatus::Armed);
        int rc = 1;
        {
            const ScopedIoHook hook(injector);
            REQUIRE(hook.installed);
            rc = tree.cook();
        }
        CHECK(rc == 0);
        CHECK(injector.fired(cp::FaultSite::FailedRead) == 1U);
        check_unchanged(good, snapshot(tree, meta)); // the recooked pack is byte-identical
        const cont::Array<crd::u8> log = bytes_of(tree.root / "cook.log.toml");
        CHECK(cont::StringView{reinterpret_cast<const char*>(log.data()), log.size()}.find("status = \"cooked\"") !=
              cont::StringView::npos);
    }

    // The artifact read that would put the cached product into the pack fails: nothing is published.
    REQUIRE(injector.arm(plan_of("seed=23;failed-read@6+1%1000000"), kInject) == cp::FaultArmStatus::Armed);
    int rc = 0;
    {
        const ScopedIoHook hook(injector);
        REQUIRE(hook.installed);
        rc = tree.cook();
    }
    CHECK(rc != 0);
    CHECK(injector.fired(cp::FaultSite::FailedRead) == 1U);
    check_unchanged(good, snapshot(tree, meta));
    injector.disarm();
    REQUIRE(tree.cook() == 0);
    check_unchanged(good, snapshot(tree, meta));
}

TEST_CASE("fault injection: an input that exists but cannot be read fails the cook instead of being cooked as absent",
          "[resources][diag9b][fault][cook]")
{
    ck::register_builtin_handlers();
    ck::register_cook_handler(cont::StringView{".optin"}, &optional_input_handler, 1U);
    const CookTree tree;
    const fs::Path source = tree.src / "m.optin";
    const fs::Path meta   = tree.src / "m.optin.meta";
    write_source(source, {1U});
    write_source(tree.src / "extra.dat", {0xE0U, 0xE1U});
    REQUIRE(tree.cook() == 0);
    const ResourceId id = meta_id(meta);
    {
        const cont::Array<crd::u8> blob = cached_blob(tree, id);
        REQUIRE(blob.size() == 3U);
        CHECK(blob[2] == 0xE1U);
    }
    const CookSnapshot good = snapshot(tree, meta);
    write_source(source, {2U});

    cp::FaultInjector injector;
    if (!crd::fault_seams_compiled())
    {
        CHECK(injector.arm(plan_of("seed=1;failed-read@6+1%1000000"), kInject) == cp::FaultArmStatus::Unavailable);
        CHECK(tree.cook() == 0);
        CHECK(cached_blob(tree, id).size() == 3U);
        return;
    }

    // The edited run reads the database (0), journal (1), .meta (2), the changed source (3), the cook's source (4)
    // and .meta (5), then the handler's optional extra.dat (6), which is there.
    REQUIRE(injector.arm(plan_of("seed=31;failed-read@6+1%1000000"), kInject) == cp::FaultArmStatus::Armed);
    int rc = 0;
    {
        const ScopedIoHook hook(injector);
        REQUIRE(hook.installed);
        rc = tree.cook();
    }
    CHECK(rc != 0); // the handler succeeded without extra.dat; the processor still refused to publish
    CHECK(injector.fired(cp::FaultSite::FailedRead) == 1U);
    check_unchanged(good, snapshot(tree, meta));

    injector.disarm();
    REQUIRE(tree.cook() == 0);
    const cont::Array<crd::u8> blob = cached_blob(tree, id);
    REQUIRE(blob.size() == 3U);
    CHECK(blob[0] == 2U);
    CHECK(blob[1] == 0xE0U);
    CHECK(blob[2] == 0xE1U);
}

TEST_CASE("fault injection: an id sidecar that exists but cannot be read is never minted over",
          "[resources][diag9b][fault][cook]")
{
    const CookTree tree;
    const fs::Path source = tree.src / "model.gltf";
    write_source(source, {1U});
    const ResourceId kept = ResourceId::mint_random();
    cont::String     sidecar_text("[id]\nuuid = \"", &s_fi_alloc);
    sidecar_text.append(kept.to_string(&s_fi_alloc));
    sidecar_text.append("\"\n");
    const fs::Path sidecar = tree.src / "model.gltf.tex.0.meta";
    REQUIRE(fs::write_file_text(sidecar, cont::StringView{sidecar_text.data(), sidecar_text.size()}));
    const cont::Array<crd::u8> sidecar_bytes = bytes_of(sidecar);

    const fs::Path missing = tree.src / "model.gltf.tex.1.meta";
    cp::FaultInjector injector;
    const bool        seams = crd::fault_seams_compiled();
    if (seams)
    {
        REQUIRE(injector.arm(plan_of("seed=41;failed-read@0+0%1000000"), kInject) == cp::FaultArmStatus::Armed);
    }
    else
    {
        CHECK(injector.arm(plan_of("seed=41;failed-read@0+0%1000000"), kInject) == cp::FaultArmStatus::Unavailable);
    }
    {
        const ScopedIoHook hook(injector);
        CHECK(hook.installed == seams);
        ck::CookIO io(source.generic(), cont::StringView{}, &s_fi_alloc);
        ResourceId id = ResourceId::mint_random();
        if (seams)
        {
            // Every read fails: the existing sidecar is unreadable, so no id and no write.
            CHECK_FALSE(io.stable_id(cont::StringView{".tex.0"}, id));
            CHECK(id.is_null());
            CHECK(io.unreadable());
            CHECK(io.unreadable_path().ends_with(cont::StringView{"model.gltf.tex.0.meta"}));
        }
        else
        {
            CHECK(io.stable_id(cont::StringView{".tex.0"}, id));
            CHECK(id == kept);
        }
        // An absent sidecar is minted and persisted even when its (failed) read found nothing: absence is not loss.
        ResourceId fresh;
        CHECK(io.stable_id(cont::StringView{".tex.1"}, fresh));
        CHECK_FALSE(fresh.is_null());
        CHECK(fs::exists(missing));
    }
    CHECK(same_bytes(bytes_of(sidecar), sidecar_bytes));

    injector.disarm();
    ck::CookIO io(source.generic(), cont::StringView{}, &s_fi_alloc);
    ResourceId id;
    CHECK(io.stable_id(cont::StringView{".tex.0"}, id));
    CHECK(id == kept);
    CHECK_FALSE(io.unreadable());
}

namespace
{
// DIAG.9c: one run of the cook reproduction. A fresh tree is cooked clean, its source edited, and the edited cook run
// under `plan`; the run reproduces when that cook fails and publishes nothing (the pack, .meta, cook database and
// cached artifact are unchanged).
struct CookReproduction
{
    cp::FaultInjector injector;
    crd::u32          arms_refused = 0U;
    crd::u64          fired        = 0U; // the last run's fired faults
};

bool cook_reproduction(void* user, const cp::FaultPlan& plan, cp::FaultRunHits& hits)
{
    auto& r = *static_cast<CookReproduction*>(user);
    hits    = cp::FaultRunHits{};
    r.fired = 0U;
    const CookTree tree;
    const fs::Path source = tree.src / "a.bin";
    const fs::Path meta   = tree.src / "a.bin.meta";
    write_source(source, {1U, 2U, 3U});
    REQUIRE(tree.cook() == 0);
    const CookSnapshot good = snapshot(tree, meta);
    write_source(source, {9U, 9U, 9U, 9U});
    if (r.injector.arm(plan, kInject) != cp::FaultArmStatus::Armed)
    {
        ++r.arms_refused;
        return false;
    }
    int rc = 0;
    {
        const ScopedIoHook hook(r.injector);
        REQUIRE(hook.installed);
        rc = tree.cook();
    }
    for (crd::u32 s = 0U; s < cp::kFaultSiteCount; ++s)
    {
        hits.hits[s] = r.injector.hits(static_cast<cp::FaultSite>(s));
        r.fired += r.injector.fired(static_cast<cp::FaultSite>(s));
    }
    r.injector.disarm();
    if (rc == 0)
    {
        return false;
    }
    const CookSnapshot now = snapshot(tree, meta);
    return same_bytes(good.pack, now.pack) && same_bytes(good.meta, now.meta) && same_bytes(good.db, now.db) &&
           same_bytes(good.artifact, now.artifact);
}
} // namespace

TEST_CASE("fault injection: a seeded read plan that fails a cook minimizes to the reads the failure needs",
          "[resources][diag9c][minimize][cook]")
{
    ck::register_builtin_handlers();
    CookReproduction run;
    cp::FaultPlan    plan;
    plan.rule_count = 1U;
    plan.rules[0]   = cp::FaultRule{cp::FaultSite::FailedRead, 0U, 0U, 400000U, 0U}; // 40% of every read fails

    if (!crd::fault_seams_compiled())
    {
        // Shipping: no plan arms, no cook fails, and the plan is returned as given.
        plan.seed = 1U;
        cp::FaultMinimizeResult result;
        cp::minimize_fault_plan(plan, &cook_reproduction, &run, crd::minimize::Budget{}, result);
        CHECK(result.status == crd::minimize::Status::NotReproduced);
        CHECK(run.arms_refused == 1U);
        CHECK(result.plan.rules[0].probability_ppm == 400000U);
        return;
    }

    // The first seed whose plan fails the cook after firing at least two faults (so the cook recovered from some).
    crd::u64         original_fired = 0U;
    cp::FaultRunHits seen;
    bool             found = false;
    for (crd::u64 seed = 1U; seed < 200U && !found; ++seed)
    {
        plan.seed      = seed;
        found          = cook_reproduction(&run, plan, seen) && run.fired >= 2U;
        original_fired = run.fired;
    }
    REQUIRE(found);
    const crd::u64 reads = seen.hits[static_cast<crd::u32>(cp::FaultSite::FailedRead)];
    cont::String   fired_at;
    for (crd::u64 h = 0U; h < reads; ++h)
    {
        if (cp::fault_plan_fires(plan, cp::FaultSite::FailedRead, h))
        {
            char digits[24];
            (void)std::snprintf(digits, sizeof(digits), " %llu", static_cast<unsigned long long>(h));
            fired_at.append(digits);
        }
    }
    INFO("seed " << plan.seed << " fired " << original_fired << " of " << reads << " reads, at" << fired_at.c_str());

    crd::minimize::Budget budget;
    budget.runs_per_candidate = 2U;
    cp::FaultMinimizeResult result;
    cp::minimize_fault_plan(plan, &cook_reproduction, &run, budget, result);
    cont::String text;
    cp::fault_plan_to_text(result.plan, text);
    INFO(text.c_str());
    CHECK(result.status == crd::minimize::Status::Minimal);
    CHECK(result.pinned);
    CHECK(result.plan.seed == 0U);
    CHECK(result.hits >= 1U);
    // The cook recovered from some of the original faults (it fired at least two), so fewer are left: with this
    // cooker the first seed fails the edited cook's first three reads, it recovers from two, and the .meta fails it.
    CHECK(result.hits < original_fired);
    CHECK(result.stats.flaky == 0U);

    // Independently of the minimizer: armed alone the result fails the cook, firing exactly its hits, and without any
    // one of its hits it does not.
    cp::FaultRunHits hits;
    CHECK(cook_reproduction(&run, result.plan, hits));
    CHECK(run.fired == result.hits);
    for (crd::u32 i = 0U; i < result.plan.rule_count; ++i)
    {
        const cp::FaultRule rule = result.plan.rules[i];
        CHECK(rule.site == cp::FaultSite::FailedRead);
        CHECK(rule.probability_ppm == cp::kFaultPpmAlways);
        for (crd::u64 h = rule.first_hit; h < rule.first_hit + rule.hit_limit; ++h)
        {
            // Only faults the original plan fired are kept: minimizing removes faults, it never moves one.
            CHECK(h < reads);
            CHECK(cp::fault_plan_fires(plan, cp::FaultSite::FailedRead, h));
            // The plan with hit `h` removed: the rule split around it.
            cp::FaultPlan reduced;
            reduced.trigger_flight = result.plan.trigger_flight;
            for (crd::u32 j = 0U; j < result.plan.rule_count; ++j)
            {
                const cp::FaultRule other = result.plan.rules[j];
                if (j != i)
                {
                    reduced.rules[reduced.rule_count++] = other;
                    continue;
                }
                if (h > other.first_hit)
                {
                    reduced.rules[reduced.rule_count++] =
                        cp::FaultRule{other.site, other.first_hit, h - other.first_hit, cp::kFaultPpmAlways, 0U};
                }
                if (h + 1U < other.first_hit + other.hit_limit)
                {
                    reduced.rules[reduced.rule_count++] = cp::FaultRule{
                        other.site, h + 1U, other.first_hit + other.hit_limit - h - 1U, cp::kFaultPpmAlways, 0U};
                }
            }
            INFO("without hit " << h);
            if (reduced.rule_count == 0U)
            {
                // The only hit. A plan needs a rule, so this one covers a read no cook reaches: no fault at all.
                reduced.rules[reduced.rule_count++] =
                    cp::FaultRule{cp::FaultSite::FailedRead, crd::u64{1U} << 40U, 1U, cp::kFaultPpmAlways, 0U};
            }
            CHECK_FALSE(cook_reproduction(&run, reduced, hits));
        }
    }
}

// ---- the install stage ------------------------------------------------------------------------------------------

namespace
{
// The file name a pack written by temp_pack_path(stem, id) has, as the flight event names it.
cont::String pack_file_name(const char* stem, ResourceId id)
{
    cont::String name(stem, &s_fi_alloc);
    name.append(id.to_string(&s_fi_alloc));
    name.append(".crdr");
    return name;
}

// The byte offset of a one-artifact pack's artifact: everything before it is the manifest container.
crd::u64 artifact_offset(const fs::Path& pack, crd::usize artifact_size)
{
    const cont::Array<crd::u8> bytes = bytes_of(pack);
    REQUIRE(bytes.size() > artifact_size);
    return static_cast<crd::u64>(bytes.size() - artifact_size);
}
} // namespace

TEST_CASE("fault injection: a pack install interrupted by a failed or short read keeps the mounted assets",
          "[resources][diag9b][fault][install]")
{
    const ResourceId           id = ResourceId::mint_random();
    const ScopedTestFile       base(temp_pack_path("fault_install_base_", id));
    const ScopedTestFile       patch(temp_pack_path("fault_install_patch_", id));
    const cont::Array<crd::u8> patch_artifact = mesh_artifact(id, 5.0F);
    write_pack(base.path(), id, kFourCC_MESH, mesh_artifact(id, 1.0F));
    write_pack(patch.path(), id, kFourCC_MESH, patch_artifact);

    crd::memory::GrowableTlsfAllocator payload_heap;
    crd::memory::ThreadSafeAllocator   payload_safe(&payload_heap);
    ResourceManager                    rm(&s_fi_alloc);
    register_mesh_loader(&rm, &payload_safe);
    const MountId base_mount = rm.mount_manifest(base.path().generic());
    REQUIRE(base_mount.is_valid());
    auto handle = rm.load_sync<MeshResource>(id);
    REQUIRE(handle.is_ready());
    const MeshResource* const good       = handle.get();
    const crd::u32            generation = handle.generation();
    REQUIRE(good != nullptr);
    CHECK(first_x(*good) == 1.0F);

    cp::FaultInjector injector;
    if (!crd::fault_seams_compiled())
    {
        CHECK(injector.arm(plan_of("seed=21;failed-read@0+1%1000000"), kInject) == cp::FaultArmStatus::Unavailable);
        const ScopedIoHook hook(injector);
        CHECK_FALSE(hook.installed);
        const MountId patch_mount = rm.mount_manifest(patch.path().generic());
        REQUIRE(patch_mount.is_valid());
        REQUIRE(rm.find_entry(id) != nullptr);
        CHECK(rm.find_entry(id)->mount_id == patch_mount.value);
        return;
    }

    // Everything an install that did not happen must leave as it was: one mount serving the id, and the loaded
    // payload with its pointer, generation and bytes.
    const auto check_kept = [&]()
    {
        CHECK(rm.mount_count() == 1U);
        CHECK(rm.entry_count() == 1U);
        const MountEntry* entry = rm.find_entry(id);
        REQUIRE(entry != nullptr);
        CHECK(entry->mount_id == base_mount.value);
        CHECK(handle.state() == LoadState::Ready);
        CHECK(handle.get() == good);
        CHECK(handle.generation() == generation);
        CHECK(first_x(*handle.get()) == 1.0F);
    };

    cp::FlightRecorder recorder;
    cp::FlightPolicy   policy;
    policy.capacity    = 64U;
    policy.pre_events  = 32U;
    policy.post_events = 1U;
    REQUIRE(recorder.init(policy) == cp::FlightPolicyStatus::Committed);
    injector.attach_flight_recorder(&recorder);

    // The install reads the pack once, whole. A short read is cut inside the container header (16 bytes), and past
    // the manifest, four bytes into the artifact: either way the install is refused, never mounted torn.
    const crd::u64 past_manifest = artifact_offset(patch.path(), patch_artifact.size()) + 4U;
    char           cut_plan[96];
    (void)std::snprintf(cut_plan, sizeof(cut_plan), "seed=23;partial-read@0+1%%1000000=%llu",
                        static_cast<unsigned long long>(past_manifest));
    const char* const plans[] = {
        "seed=21;failed-read@0+1%1000000",
        "seed=22;partial-read@0+1%1000000=16",
        cut_plan,
    };
    const cont::String patch_name = pack_file_name("fault_install_patch_", id);
    cont::String       tail(" on ", &s_fi_alloc);
    tail.append(patch_name);
    const auto install_faulted = [&](const char* plan)
    {
        REQUIRE(injector.arm(plan_of(plan), kInject) == cp::FaultArmStatus::Armed);
        MountId mount;
        {
            const ScopedIoHook hook(injector);
            REQUIRE(hook.installed);
            mount = rm.mount_manifest(patch.path().generic());
        }
        CHECK_FALSE(mount.is_valid());
        CHECK(injector.fired(cp::FaultSite::FailedRead) + injector.fired(cp::FaultSite::PartialRead) == 1U);
        check_kept();
        const FaultEvidence evidence = take_fault_evidence(recorder);
        CHECK(ends_with(evidence.message, tail.c_str()));
        return evidence;
    };
    for (const char* plan : plans)
    {
        INFO(plan);
        const FaultEvidence evidence = install_faulted(plan);
        // The recipe's one-hit plan interrupts the same install on its own.
        INFO(evidence.minimal.c_str());
        (void)install_faulted(evidence.minimal.c_str());
    }

    // Disarmed, the same install mounts, the newest mount serves the id, and a reload of it swaps the new content in.
    injector.disarm();
    const MountId patch_mount = rm.mount_manifest(patch.path().generic());
    REQUIRE(patch_mount.is_valid());
    CHECK(rm.mount_count() == 2U);
    REQUIRE(rm.find_entry(id) != nullptr);
    CHECK(rm.find_entry(id)->mount_id == patch_mount.value);
    CHECK(rm.reload_mount_now(patch_mount) == 1U);
    CHECK(handle.generation() == generation + 1U);
    CHECK(first_x(*handle.get()) == 5.0F);
    recorder.shutdown();
}

TEST_CASE("a torn pack is refused at install and the mounted asset keeps serving", "[resources][diag9b][install]")
{
    const ResourceId           id = ResourceId::mint_random();
    const ScopedTestFile       base(temp_pack_path("torn_install_base_", id));
    const ScopedTestFile       patch(temp_pack_path("torn_install_patch_", id));
    const cont::Array<crd::u8> patch_artifact = mesh_artifact(id, 5.0F);
    write_pack(base.path(), id, kFourCC_MESH, mesh_artifact(id, 1.0F));
    write_pack(patch.path(), id, kFourCC_MESH, patch_artifact);
    const cont::Array<crd::u8> whole = bytes_of(patch.path());

    crd::memory::GrowableTlsfAllocator payload_heap;
    crd::memory::ThreadSafeAllocator   payload_safe(&payload_heap);
    ResourceManager                    rm(&s_fi_alloc);
    register_mesh_loader(&rm, &payload_safe);
    const MountId base_mount = rm.mount_manifest(base.path().generic());
    REQUIRE(base_mount.is_valid());

    // The patch on disk ends halfway through its artifact (its manifest is whole), then one byte short of its end,
    // then inside the manifest container. None may mount: a mounted torn entry would shadow the base mount's.
    const crd::u64   offset = artifact_offset(patch.path(), patch_artifact.size());
    const crd::usize cuts[] = {static_cast<crd::usize>(offset) + patch_artifact.size() / 2U, whole.size() - 1U, 40U};
    for (const crd::usize cut : cuts)
    {
        INFO("pack cut to " << cut << " of " << whole.size() << " bytes");
        REQUIRE(fs::write_file_binary(patch.path(), cont::ConstSpan<crd::u8>{whole.data(), cut}));
        CHECK_FALSE(rm.mount_manifest(patch.path().generic()).is_valid());
        CHECK(rm.mount_count() == 1U);
        REQUIRE(rm.find_entry(id) != nullptr);
        CHECK(rm.find_entry(id)->mount_id == base_mount.value);
    }

    // A first load after the refused installs still reads the base mount's artifact.
    auto handle = rm.load_sync<MeshResource>(id);
    REQUIRE(handle.is_ready());
    CHECK(first_x(*handle.get()) == 1.0F);

    // The whole patch mounts and serves the id.
    REQUIRE(fs::write_file_binary(patch.path(), cont::as_const_span(whole)));
    const MountId patch_mount = rm.mount_manifest(patch.path().generic());
    REQUIRE(patch_mount.is_valid());
    REQUIRE(rm.find_entry(id) != nullptr);
    CHECK(rm.find_entry(id)->mount_id == patch_mount.value);
}

// ---- teardown ---------------------------------------------------------------------------------------------------

namespace
{
// Counts the allocations live through it, so a case can show a teardown returned every one. Thread-safe when its
// backing allocator is.
class CountingAllocator final : public crd::memory::IAllocator
{
public:
    explicit CountingAllocator(crd::memory::IAllocator* backing) noexcept : m_backing(backing)
    {
        m_name = "fault-teardown-counting";
    }

    void* allocate(crd::usize size, crd::usize alignment = crd::memory::kDefaultAlignment) override
    {
        void* p = m_backing->allocate(size, alignment);
        if (p != nullptr)
        {
            m_live.fetch_add(1, std::memory_order_relaxed);
        }
        return p;
    }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment = crd::memory::kDefaultAlignment) override
    {
        void* p = m_backing->try_allocate(size, alignment);
        if (p != nullptr)
        {
            m_live.fetch_add(1, std::memory_order_relaxed);
        }
        return p;
    }
    void deallocate(void* p) noexcept override
    {
        if (p != nullptr)
        {
            m_live.fetch_sub(1, std::memory_order_relaxed);
            m_backing->deallocate(p);
        }
    }
    [[nodiscard]] bool owns(const void* p) const noexcept override { return m_backing->owns(p); }
    [[nodiscard]] crd::usize allocation_size(const void* p) const noexcept override
    {
        return m_backing->allocation_size(p);
    }

    [[nodiscard]] crd::i64 live() const noexcept { return m_live.load(std::memory_order_relaxed); }

private:
    crd::memory::IAllocator* m_backing;
    std::atomic<crd::i64>    m_live{0};
};

// The capture a recorder delivers when it shuts down with a window open.
struct TeardownSink
{
    crd::u32          delivered  = 0U;
    cp::FlightEnd     end        = cp::FlightEnd::None;
    bool              truncated  = false;
    cp::FlightTrigger kind       = cp::FlightTrigger::Manual;
    crd::u64          suppressed = 0U;
};

void teardown_sink(const cp::FlightCapture& capture, void* user)
{
    auto* sink = static_cast<TeardownSink*>(user);
    sink->delivered += 1U;
    sink->end        = capture.end;
    sink->truncated  = capture.post_truncated;
    sink->kind       = capture.trigger.kind;
    sink->suppressed = capture.suppressed_triggers;
}

crd::u64 total_hits(const cp::FaultInjector& injector)
{
    crd::u64 n = 0U;
    for (crd::u32 s = 0U; s < cp::kFaultSiteCount; ++s)
    {
        n += injector.hits(static_cast<cp::FaultSite>(s));
    }
    return n;
}

crd::u64 total_fired(const cp::FaultInjector& injector)
{
    crd::u64 n = 0U;
    for (crd::u32 s = 0U; s < cp::kFaultSiteCount; ++s)
    {
        n += injector.fired(static_cast<cp::FaultSite>(s));
    }
    return n;
}
} // namespace

TEST_CASE("fault injection: teardown after injected faults releases everything the run allocated",
          "[resources][diag9b][fault][teardown]")
{
    // crd::jobs is initialised by the ResourcesJobsListener in test_resource_manager.cpp (same binary).
    const ResourceId     id        = ResourceId::mint_random();
    const ResourceId     stream_id = ResourceId::mint_random();
    const ScopedTestFile pack(temp_pack_path("fault_teardown_", id));
    const ScopedTestFile patch(temp_pack_path("fault_teardown_patch_", id));
    const ScopedTestFile stream_pack(temp_pack_path("fault_teardown_stream_", stream_id));
    write_pack(pack.path(), id, kFourCC_MESH, mesh_artifact(id, 1.0F));
    write_pack(patch.path(), id, kFourCC_MESH, mesh_artifact(id, 3.0F));
    write_pack(stream_pack.path(), stream_id, kFourCC_MESH, mesh_artifact(stream_id, 2.0F));

    crd::memory::GrowableTlsfAllocator heap;
    crd::memory::ThreadSafeAllocator   heap_safe(&heap);
    CountingAllocator                  counted(&heap_safe);
    cp::FaultInjector                  injector;
    const bool                         seams = crd::fault_seams_compiled();

    TeardownSink       sink;
    cp::FlightRecorder recorder;
    cp::FlightPolicy   policy;
    policy.capacity    = 256U;
    policy.pre_events  = 32U;
    policy.post_events = 200U; // the first fault's window stays open until the recorder shuts down
    REQUIRE(recorder.init(policy) == cp::FlightPolicyStatus::Committed);
    recorder.set_sink(&teardown_sink, &sink);
    injector.attach_flight_recorder(&recorder);

    // Arming restarts the injector's counters, so the faults fired under each plan are summed before the next.
    crd::u64   fired_total = 0U;
    const auto arm         = [&](const char* plan)
    {
        fired_total += total_fired(injector);
        REQUIRE(injector.arm(plan_of(plan), kInject) == cp::FaultArmStatus::Armed);
    };

    crd::u64 hits_before_teardown = 0U;
    {
        cp::FaultAllocator payload(&counted, &injector, "fault-teardown-payload");
        const ScopedIoHook hook(injector); // outlives the manager, so the teardown below runs hooked
        CHECK(hook.installed == seams);
        {
            ResourceManager rm(&counted);
            register_mesh_loader(&rm, &payload);
            const MountId mount = rm.mount_manifest(pack.path().generic());
            REQUIRE(mount.is_valid());
            REQUIRE(rm.mount_manifest(stream_pack.path().generic()).is_valid());
            auto handle = rm.load_sync<MeshResource>(id);
            REQUIRE(handle.is_ready());
            write_pack(pack.path(), id, kFourCC_MESH, mesh_artifact(id, 7.0F));

            if (seams)
            {
                // A run in which every kind of read and allocation fault fires: interrupted reloads, a refused
                // install, streamed loads that fail.
                arm("seed=31;allocation-failure@0+1%1000000");
                CHECK(rm.reload_mount_now(mount) == 0U);
                arm("seed=32;failed-read@0+1%1000000");
                CHECK(rm.reload_mount_now(mount) == 0U);
                arm("seed=33;partial-read@0+1%1000000=16");
                CHECK(rm.reload_mount_now(mount) == 0U);
                arm("seed=34;failed-read@0+1%1000000");
                CHECK_FALSE(rm.mount_manifest(patch.path().generic()).is_valid());
                const char* const streamed[] = {
                    "seed=35;cancelled-completion@0+1%1000000",
                    "seed=36;failed-read@0+1%1000000",
                };
                for (const char* plan : streamed)
                {
                    INFO(plan);
                    arm(plan);
                    auto failed = rm.load_streamed<MeshResource>(stream_id);
                    CHECK(failed.wait_ready() == LoadState::Failed);
                }
                REQUIRE(handle.get() != nullptr);
                CHECK(first_x(*handle.get()) == 1.0F);
                // A streamed load still in flight when the host starts its teardown.
                arm("seed=37;delayed-completion@0+1%1000000=50000000");
            }
            else
            {
                CHECK(injector.arm(plan_of("seed=31;allocation-failure@0+1%1000000"), kInject) ==
                      cp::FaultArmStatus::Unavailable);
            }

            // The host's required drain (wait on every handle before the manager dies) waits the delayed completion
            // out, and the load completes.
            const auto t0        = std::chrono::steady_clock::now();
            auto       in_flight = rm.load_streamed<MeshResource>(stream_id);
            CHECK(in_flight.wait_ready() == LoadState::Ready);
            if (seams)
            {
                CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(50));
                CHECK(injector.fired(cp::FaultSite::DelayedCompletion) == 1U);
            }
            REQUIRE(in_flight.get() != nullptr);
            CHECK(first_x(*in_flight.get()) == 2.0F);

            // A clean reload last, so the teardown also frees a retired payload still in its grace period. Disarming
            // keeps the counters; the next arm adds the delayed completion to the total.
            injector.disarm();
            CHECK(rm.reload_mount_now(mount) == 1U);
            CHECK(first_x(*handle.get()) == 7.0F);

            // The teardown runs with every read and every recoverable allocation armed to fail. It performs neither,
            // so no site is consulted while the handles drop and the manager is destroyed.
            if (seams)
            {
                arm("seed=38;failed-read@0+0%1000000;allocation-failure@0+0%1000000");
            }
            hits_before_teardown = total_hits(injector);
        }
        CHECK(total_hits(injector) == hits_before_teardown);
        CHECK(total_fired(injector) == 0U);
    }
    injector.disarm();
    CHECK(counted.live() == 0);

    // The first fault's window was still open; shutting the recorder down delivers it, marked truncated, with every
    // later fault counted as suppressed.
    recorder.shutdown();
    if (seams)
    {
        CHECK(fired_total == 7U);
        CHECK(sink.delivered == 1U);
        CHECK(sink.kind == cp::FlightTrigger::InjectedFault);
        CHECK(sink.end == cp::FlightEnd::Shutdown);
        CHECK(sink.truncated);
        CHECK(sink.suppressed + 1U == fired_total);
    }
    else
    {
        CHECK(sink.delivered == 0U);
    }
}
