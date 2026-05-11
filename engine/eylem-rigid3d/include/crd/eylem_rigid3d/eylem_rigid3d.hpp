#pragma once

// crd-eylem-rigid3d -- concrete rigid-3D physics impl. Phase 3.1 v1b.
//
// Peer module to crd-eylem (interface). v1b ships:
//   v1b-a:  BodyPool (AoSoA-8 storage)              ← shipped
//   v1b-b:  ColliderPool (sphere/box/capsule)
//   v1b-c:  RigidBodyComponent + EylemSystem
//
// Application wiring (after v1b-c):
//
//     crd::eylem::PhysicsConfig cfg;
//     cfg.persistent_alloc = &my_tlsf;
//     cfg.solver_scratch   = &my_linear;
//     auto scene = crd::eylem_rigid3d::make_scene(cfg);
//     world.register_component<crd::eylem::RigidBodyComponent>(...);
//     world.register_system(std::make_unique<crd::eylem_rigid3d::EylemSystem>(*scene));

#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/eylem_rigid3d/collider_pool.hpp>
