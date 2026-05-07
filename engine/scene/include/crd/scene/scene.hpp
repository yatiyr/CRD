#pragma once

// crd-scene umbrella header. Phase 3.0 v1a — entity identity layer only.
// Subsequent slices grow this with components, storage backends, relations,
// query DSL, schedule, and indexes (see docs/phases/phase-3.0-scene-ecs.md).

#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/slot_map.hpp>
#include <crd/scene/storage_backend.hpp>
#include <crd/scene/world.hpp>
