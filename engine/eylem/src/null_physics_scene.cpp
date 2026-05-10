// NullPhysicsScene — v1a stub implementation of IPhysicsScene.
//
// Records bodies / colliders / joints in flat in-order arrays so add/remove
// round-trip + has_*() work. step() is a no-op (no integration, no
// constraint solving). Sufficient for tests + tools to link against the
// interface; v1b ships the real implementation in crd-eylem-rigid3d.

#include <crd/containers/array.hpp>
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

    [[nodiscard]] ColliderId add_collider(BodyId          /*body*/,
                                          const Collider& collider,
                                          const Material& material) override
    {
        const crd::u32 idx = static_cast<crd::u32>(m_colliders.size()) + 1U;
        m_colliders.push_back(StoredCollider{collider, material});
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

    [[nodiscard]] std::optional<RaycastHit> raycast(crd::math::Vec3f /*origin*/,
                                                    crd::math::Vec3f /*direction*/,
                                                    crd::f32         /*max_distance*/) const override
    {
        return std::nullopt;
    }

private:
    struct StoredCollider
    {
        Collider collider;
        Material material;
    };

    PhysicsConfig                  m_config;
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
