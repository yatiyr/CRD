// Resource manager contract fatals (crd-resources-fatal-* CTests, tests/assets/resources). A control block is what the
// manager reports through (a failed load is a Failed block too) and a load context is what its job runs from, so when
// the manager's bookkeeping allocator refuses one (a sub-budget allocator returns nullptr) the manager is out of
// memory, fatal in every build, never a block or a context built on null. One mode per run (argv[1]); see
// expected_fatal.hpp for the exit protocol:
//   failed-block-refused      load_sync of an id no mounted pack names: the Failed block is refused
//   sync-block-refused        load_sync of a mounted id: its first control block is refused
//   async-block-refused       load_async of a mounted id: likewise
//   streamed-block-refused    load_streamed of a mounted id: likewise
//   async-context-refused     load_async: the block is granted, the job's context is refused
//   streamed-context-refused  load_streamed: likewise
// The allocator refuses by request shape, not by count: a control block is the only request of its size and alignment
// (a string asks for byte alignment), so a change upstream of the refused request cannot move the refusal onto it.
// The pack is deleted once mounted (and once the warm-up load has read it), so no run leaves a file behind.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "specimen_common.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_control_block.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <initializer_list>
#include <memory>

namespace
{
namespace fs = crd::platform::fs;
namespace cont = crd::containers;
namespace res = crd::resources;

// Grants every request from the default allocator until armed. Armed to refuse the block, it refuses the first request
// shaped like a control block; armed to refuse after the block, it grants that request and refuses every later one.
class ShapeRefusingAllocator final : public crd::memory::IAllocator
{
public:
    enum class Arm : crd::u8
    {
        Off,
        RefuseBlock,
        RefuseAfterBlock,
    };

    ShapeRefusingAllocator() noexcept { m_name = "resources-fatal-shape-refusing"; }

    void* allocate(crd::usize size, crd::usize alignment) override { return try_allocate(size, alignment); }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment) override
    {
        const bool block_shaped =
            size == sizeof(res::ResourceControlBlock) && alignment == alignof(res::ResourceControlBlock);
        if (m_refusing)
        {
            return nullptr;
        }
        if (block_shaped && m_arm == Arm::RefuseBlock)
        {
            return nullptr;
        }
        if (block_shaped && m_arm == Arm::RefuseAfterBlock)
        {
            m_refusing = true; // this block is granted; the context that follows it is not
        }
        return crd::memory::default_allocator()->try_allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override { crd::memory::default_allocator()->deallocate(p); }
    [[nodiscard]] bool owns(const void* p) const noexcept override { return p != nullptr; }

    void arm(Arm a) noexcept { m_arm = a; }

private:
    Arm m_arm = Arm::Off;
    bool m_refusing = false;
};

// Hands every load the same static payload: the specimen needs a Ready warm-up load, not a resource.
struct SpecimenLoader final : public res::ILoader
{
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return res::kFourCC_BLOB; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void* load(const res::LoadContext& /*ctx*/) override { return &payload; }
    void unload(void* /*payload*/) noexcept override {}

    inline static crd::u32 payload = 0U;
};

constexpr crd::u32 kBlobBytes = 4U;

// A two-entry PACK (warm-up and target, four bytes each) at <temp>/<id>.crdr. False when it could not be written.
[[nodiscard]] bool write_pack(const fs::Path& path, res::ResourceId warm, res::ResourceId target)
{
    crd::memory::IAllocator* const alloc = crd::memory::default_allocator();
    cont::Array<crd::u8> pool(alloc);
    for (const char c : cont::StringView{"resources-fatal.blob"})
    {
        pool.push_back(static_cast<crd::u8>(c));
    }
    pool.push_back(0U);

    cont::Array<res::ManifestEntry> entries(alloc);
    for (const res::ResourceId id : {warm, target})
    {
        res::ManifestEntry e;
        e.id = id;
        e.type_fourcc = res::kFourCC_BLOB;
        e.blob_size = kBlobBytes;
        e.name_strp_idx = 0U;
        entries.push_back(e);
    }
    const res::ResourceId pack_id = res::ResourceId::mint_random();
    crd::u64 base = 0U;
    {
        res::CrdrWriter measure(alloc, pack_id, res::kFourCC_PACK);
        res::manifest_write(measure, cont::as_const_span(entries), cont::as_const_span(pool));
        base = static_cast<crd::u64>(measure.finish().size());
    }
    entries[0].blob_offset = base;
    entries[1].blob_offset = base + kBlobBytes;
    res::CrdrWriter writer(alloc, pack_id, res::kFourCC_PACK);
    res::manifest_write(writer, cont::as_const_span(entries), cont::as_const_span(pool));
    cont::Array<crd::u8> bytes = writer.finish();
    for (crd::u32 i = 0U; i < 2U * kBlobBytes; ++i)
    {
        bytes.push_back(static_cast<crd::u8>(0xB0U + i));
    }
    return fs::write_file_binary(path, cont::as_const_span(bytes));
}

enum class Route : crd::u8
{
    Sync,
    Async,
    Streamed,
};

// Loads `id` along `route`. The armed fatal fires inside the call, so it never returns in a passing run.
void load(res::ResourceManager& manager, res::ResourceId id, Route route)
{
    switch (route)
    {
        case Route::Sync:
            (void)manager.load_sync<crd::u32>(id);
            return;
        case Route::Async:
            (void)manager.load_async<crd::u32>(id);
            return;
        case Route::Streamed:
            (void)manager.load_streamed<crd::u32>(id);
            return;
    }
}

// Mounts the two-entry pack, optionally loads the warm-up id once (so the in-flight table has its buckets and the
// next allocation after the target's block is its context), deletes the pack, arms the allocator and loads the target.
// Returning at all means the fatal did not fire.
void run_load(ShapeRefusingAllocator::Arm arm, Route route, bool warm_up)
{
    ShapeRefusingAllocator alloc;
    const res::ResourceId warm = res::ResourceId::mint_random();
    const res::ResourceId target = res::ResourceId::mint_random();
    const cont::String stem = target.to_string(crd::memory::default_allocator());
    cont::String name(stem.data(), stem.size(), crd::memory::default_allocator());
    name.append(".crdr");
    const fs::Path pack = fs::temp_directory() / cont::StringView{name.data(), name.size()};
    if (!write_pack(pack, warm, target))
    {
        return; // no fatal reached: the CTest's marker fails the run
    }
    res::ResourceManager manager(&alloc);
    manager.register_loader(std::make_unique<SpecimenLoader>());
    const bool mounted = manager.mount_manifest(pack.generic()).is_valid();
    if (mounted && warm_up)
    {
        (void)manager.load_sync<crd::u32>(warm);
    }
    (void)fs::remove_file(pack);
    if (!mounted)
    {
        return;
    }
    alloc.arm(arm);
    load(manager, target, route);
}

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    using Arm = ShapeRefusingAllocator::Arm;
    constexpr const char* block_oom = "ResourceManager: out of memory for a resource control block";
    if (is(mode, "failed-block-refused"))
    {
        crd_diag_fatal::arm(mode, block_oom);
        ShapeRefusingAllocator alloc;
        res::ResourceManager manager(&alloc);
        alloc.arm(Arm::RefuseBlock);
        (void)manager.load_sync<crd::u32>(res::ResourceId::mint_random());
        return true;
    }
    if (is(mode, "sync-block-refused"))
    {
        crd_diag_fatal::arm(mode, block_oom);
        run_load(Arm::RefuseBlock, Route::Sync, false);
        return true;
    }
    if (is(mode, "async-block-refused"))
    {
        crd_diag_fatal::arm(mode, block_oom);
        run_load(Arm::RefuseBlock, Route::Async, false);
        return true;
    }
    if (is(mode, "streamed-block-refused"))
    {
        crd_diag_fatal::arm(mode, block_oom);
        run_load(Arm::RefuseBlock, Route::Streamed, false);
        return true;
    }
    if (is(mode, "async-context-refused"))
    {
        crd_diag_fatal::arm(mode, "ResourceManager: out of memory for an async load context");
        run_load(Arm::RefuseAfterBlock, Route::Async, true);
        return true;
    }
    if (is(mode, "streamed-context-refused"))
    {
        crd_diag_fatal::arm(mode, "ResourceManager: out of memory for a streamed load context");
        run_load(Arm::RefuseAfterBlock, Route::Streamed, true);
        return true;
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
