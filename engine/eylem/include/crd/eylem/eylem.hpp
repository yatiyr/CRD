#pragma once

// Umbrella header for the eylem (Cerid-native physics) public surface.
// Pulls in every type a consumer typically uses to spawn + step a scene.
//
// Most translation units want only one of these — prefer including the
// specific header directly. Use this when you need everything (e.g. in a
// PCH or a thin glue layer like a TOML cooker).
//
// Spec: ADR-0062 (architecture) + ADR-0063 (determinism contract).
// Phase plan: docs/phases/phase-3.1-eylem.md.

#include <crd/eylem/collider.hpp>
#include <crd/eylem/collision_filter.hpp>
#include <crd/eylem/force_field.hpp>
#include <crd/eylem/joint.hpp>
#include <crd/eylem/mass_properties.hpp>
#include <crd/eylem/material.hpp>
#include <crd/eylem/material_pool.hpp>
#include <crd/eylem/physics_config.hpp>
#include <crd/eylem/physics_scene.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem/types.hpp>
