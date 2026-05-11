// NullPhysicsScene — v1a stub implementation of IPhysicsScene.
//
// Records bodies / colliders / joints in flat in-order arrays so add/remove
// round-trip + has_*() work. step() is a no-op (no integration, no
// constraint solving). Sufficient for tests + tools to link against the
// interface; v1b ships the real implementation in crd-eylem-rigid3d.

#include <crd/containers/array.hpp>
#include <crd/eylem/mass_properties.hpp>
#include <crd/eylem/material_pool.hpp>
#include <crd/eylem/physics_scene.hpp>

#include <memory>
#include <optional>

namespace crd::eylem
{
namespace
{
class NullPhysicsScene final : public IPhysicsScene
{
public:
    explicit NullPhysicsScene(const PhysicsConfig& config)
        : m_config(config)
    {
    }

    [[nodiscard]] const PhysicsConfig& config() const noexcept override { return m_config; }

    void set_gravity(crd::math::Vec3f g) noexcept override { m_config.gravity = g; }

    [[nodiscard]] crd::math::Vec3f gravity() const noexcept override { return m_config.gravity; }

    [[nodiscard]] BodyId add_body(const RigidBody& body) override
    {
        const crd::u32 idx = static_cast<crd::u32>(m_bodies.size()) + 1U; // index 0 reserved
        m_bodies.push_back(body);
        return BodyId::make(idx, /*generation=*/1U);
    }

    void remove_body(BodyId /*id*/) override
    {
        // Null impl: no-op. Real impl recycles slots + bumps generation.
    }

    [[nodiscard]] bool has_body(BodyId id) const noexcept override
    {
        if (id.is_null())
        {
            return false;
        }
        const crd::u32 i = id.index();
        return (i >= 1U) && (i <= m_bodies.size());
    }

    [[nodiscard]] crd::usize body_count() const noexcept override { return m_bodies.size(); }

    // ---- ADR-0069 §11 — material pool surface --------------------------
    [[nodiscard]] MaterialId create_material(const Material& material) override
    {
        return m_materials.insert(material);
    }

    void update_material(MaterialId id, const Material& material) noexcept override
    {
        m_materials.update(id, material);
    }

    [[nodiscard]] const Material& material(MaterialId id) const noexcept override
    {
        return m_materials.get(id);
    }

    [[nodiscard]] bool has_material(MaterialId id) const noexcept override
    {
        return m_materials.contains(id);
    }

    [[nodiscard]] DerivedMassProperties derive_body_mass(BodyId id) const override
    {
        if (!has_body(id))
        {
            return DerivedMassProperties{};
        }

        // Collect colliders for this body in storage order — which is
        // ascending ColliderId order in the null impl (m_colliders is
        // append-only and ColliderId.index() = position + 1). The free
        // function then runs the deterministic Σ in that exact order.
        crd::containers::Array<Collider> body_colliders{m_colliders.allocator()};
        body_colliders.reserve(m_colliders.size());
        for (const StoredCollider& sc : m_colliders)
        {
            if (sc.body == id)
            {
                body_colliders.push_back(sc.collider);
            }
        }

        const auto accessor = +[](void* user_data, MaterialId mid) -> const Material&
        {
            return static_cast<const MaterialPool*>(user_data)->get(mid);
        };
        // const_cast: MaterialPool::get is const-correct; the accessor
        // signature uses void* for type erasure across cooker / scene
        // callers and discards const at the boundary.
        return derive_mass_properties(
            crd::containers::ConstSpan<Collider>(body_colliders.data(), body_colliders.size()),
            accessor,
            const_cast<MaterialPool*>(&m_materials));
    }

    using IPhysicsScene::add_collider; // bring the 3-arg convenience overload into scope

    [[nodiscard]] ColliderId add_collider(BodyId          body,
                                          const Collider& collider) override
    {
        const crd::u32 idx = static_cast<crd::u32>(m_colliders.size()) + 1U;
        m_colliders.push_back(StoredCollider{body, collider});
        return ColliderId::make(idx, /*generation=*/1U);
    }

    void remove_collider(ColliderId /*id*/) override {}

    [[nodiscard]] bool has_collider(ColliderId id) const noexcept override
    {
        if (id.is_null())
        {
            return false;
        }
        const crd::u32 i = id.index();
        return (i >= 1U) && (i <= m_colliders.size());
    }

    [[nodiscard]] JointId add_joint(const Joint& joint) override
    {
        const crd::u32 idx = static_cast<crd::u32>(m_joints.size()) + 1U;
        m_joints.push_back(joint);
        return JointId::make(idx, /*generation=*/1U);
    }

    void remove_joint(JointId /*id*/) override {}

    [[nodiscard]] bool has_joint(JointId id) const noexcept override
    {
        if (id.is_null())
        {
            return false;
        }
        const crd::u32 i = id.index();
        return (i >= 1U) && (i <= m_joints.size());
    }

    [[nodiscard]] RigidBody body_state(BodyId id) const override
    {
        if (!has_body(id))
        {
            return RigidBody{};
        }
        return m_bodies[id.index() - 1U];
    }

    void set_body_state(BodyId id, const RigidBody& state) override
    {
        if (has_body(id))
        {
            m_bodies[id.index() - 1U] = state;
        }
    }

    void apply_force(BodyId /*id*/, crd::math::Vec3f /*force*/) override {}
    void apply_torque(BodyId /*id*/, crd::math::Vec3f /*torque*/) override {}
    void apply_impulse(BodyId /*id*/, crd::math::Vec3f /*impulse*/, crd::math::Vec3f /*world_pos*/) override {}

    void step(crd::f32 /*dt*/) override
    {
        // No integration in the null impl. Real impl in v1b+ runs the SI solver.
    }

    // ---- ADR-0068 surface — null impl returns nothing / accepts nothing ----
    void exclude_pair(BodyId /*a*/, BodyId /*b*/) noexcept override {}
    void include_pair(BodyId /*a*/, BodyId /*b*/) noexcept override {}
    [[nodiscard]] bool is_pair_excluded(BodyId /*a*/, BodyId /*b*/) const noexcept override
    {
        return false;
    }
    void set_collision_predicate(ICollisionPredicate* /*predicate*/) noexcept override {}
    [[nodiscard]] ICollisionPredicate* collision_predicate() const noexcept override
    {
        return nullptr;
    }
    void set_contact_modify_callback(IContactModifyCallback* /*callback*/) noexcept override {}
    [[nodiscard]] IContactModifyCallback* contact_modify_callback() const noexcept override
    {
        return nullptr;
    }
    [[nodiscard]] crd::containers::ConstSpan<ContactEvent> drain_contact_events() noexcept override
    {
        return {};
    }
    [[nodiscard]] crd::containers::ConstSpan<TriggerEvent> drain_trigger_events() noexcept override
    {
        return {};
    }

    [[nodiscard]] std::optional<RaycastHit> raycast(crd::math::Vec3f /*origin*/,
                                                    crd::math::Vec3f /*direction*/,
                                                    crd::f32         /*max_distance*/) const override
    {
        return std::nullopt;
    }

private:
    // v1a-material-c: per-collider material is referenced via Collider::material
    // (a MaterialId handle into m_materials). The collider record no longer
    // stores Material inline.
    // v1a-material-d: track the owning body so derive_body_mass can walk
    // the body's collider compound in ascending ColliderId order (storage
    // order == ColliderId order in the null impl).
    struct StoredCollider
    {
        BodyId   body;
        Collider collider;
    };

    PhysicsConfig                  m_config;
    MaterialPool                           m_materials;
    crd::containers::Array<RigidBody>      m_bodies;
    crd::containers::Array<StoredCollider> m_colliders;
    crd::containers::Array<Joint>          m_joints;
};
} // namespace

std::unique_ptr<IPhysicsScene> make_null_physics_scene(const PhysicsConfig& config)
{
    return std::make_unique<NullPhysicsScene>(config);
}

} // namespace crd::eylem
