// Scene contract fatals (crd-scene-fatal-* CTests, tests/world/scene). The scene's lookups that a stored id or a
// writer's contract makes certain are fatal on a miss in every build, never a null pointer followed (they read
// through null in a build without asserts before). One mode per run (argv[1]); see expected_fatal.hpp for the exit
// protocol:
//   registry-require-unregistered  ComponentRegistry::require_info for an id the registry never issued
//   archetype-require-unissued     ArchetypeGraph::require for an archetype id the graph never issued
//   transform-writer-without-transform  World::set_translation on an entity that carries no Transform
//   transform-writer-dead-entity   World::set_scale on a destroyed entity
//   commands-add-unregistered      Commands::add_component of a type the world never registered
// A storage whose allocator refuses (a sub-budget allocator returns nullptr) is out of memory, fatal in every build,
// never a component pool built on null:
//   sparse-pool-refused            SparseSetStorage::insert needs a new pool once the allocator refuses

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "specimen_common.hpp"

#include <crd/memory/allocator.hpp>
#include <crd/scene/archetype.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/sparse_set_storage.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

#include <cstring>

namespace
{
namespace sc = crd::scene;

struct NeverRegistered
{
    crd::u32 v = 0;
};

struct SparseOnly
{
    crd::u32 v = 0;
};

// An allocator that grants from the default allocator until refuse() and returns nullptr from then on, as an
// exhausted sub-budget allocator does.
class SwitchableAllocator final : public crd::memory::IAllocator
{
public:
    SwitchableAllocator() noexcept { m_name = "scene-fatal-switchable"; }

    void* allocate(crd::usize size, crd::usize alignment) override { return try_allocate(size, alignment); }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment) override
    {
        return m_refusing ? nullptr : crd::memory::default_allocator()->try_allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override { crd::memory::default_allocator()->deallocate(p); }
    [[nodiscard]] bool owns(const void* p) const noexcept override { return p != nullptr; }

    void refuse() noexcept { m_refusing = true; }

private:
    bool m_refusing = false;
};

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    if (is(mode, "registry-require-unregistered"))
    {
        crd_diag_fatal::arm(mode, "ComponentRegistry::require_info: the component id is not registered");
        const sc::ComponentRegistry registry;
        return registry.require_info(sc::ComponentId{5}).size != 0U;
    }
    if (is(mode, "archetype-require-unissued"))
    {
        crd_diag_fatal::arm(mode, "ArchetypeGraph::require: the archetype id was not issued by this graph");
        sc::World w;
        return w.storage().graph().require(sc::ArchetypeId{7U}).id.raw != 0U;
    }
    if (is(mode, "transform-writer-without-transform"))
    {
        crd_diag_fatal::arm(mode, "World transform writer: the entity is dead or has no Transform component");
        sc::World w;
        w.register_component<sc::Transform>();
        w.register_component<sc::TransformDirtyFlag>(sc::StorageHint::SparseSet);
        const sc::EntityId e = w.spawn();
        w.set_translation(e, crd::math::Vec3f{1.0F, 0.0F, 0.0F});
        return true;
    }
    if (is(mode, "transform-writer-dead-entity"))
    {
        crd_diag_fatal::arm(mode, "World transform writer: the entity is dead or has no Transform component");
        sc::World w;
        w.register_component<sc::Transform>();
        w.register_component<sc::TransformDirtyFlag>(sc::StorageHint::SparseSet);
        const sc::EntityId e = w.spawn();
        w.add_component<sc::Transform>(e, sc::Transform{});
        w.destroy_immediate(e);
        w.set_scale(e, crd::math::Vec3f{2.0F, 2.0F, 2.0F});
        return true;
    }
    if (is(mode, "commands-add-unregistered"))
    {
        crd_diag_fatal::arm(mode, "ComponentRegistry::require_info: the component id is not registered");
        sc::World w;
        const sc::EntityId e = w.spawn();
        w.commands().add_component<NeverRegistered>(e, NeverRegistered{3U});
        return true;
    }
    if (is(mode, "sparse-pool-refused"))
    {
        crd_diag_fatal::arm(mode, "SparseSetStorage: out of memory for a component pool");
        SwitchableAllocator alloc;
        sc::ComponentRegistry registry;
        const sc::ComponentId id = registry.register_type<SparseOnly>(sc::StorageHint::SparseSet);
        sc::SparseSetStorage storage{&alloc, registry};
        alloc.refuse(); // the pool slots are granted; the first pool is not
        SparseOnly value{4U};
        storage.insert(sc::EntityId::make(1U, 1U), id, &value);
        return storage.has(sc::EntityId::make(1U, 1U), id);
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
