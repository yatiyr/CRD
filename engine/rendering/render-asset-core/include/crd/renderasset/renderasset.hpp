#pragma once

// crd-render-asset-core — umbrella include (RAF-1, mission §5 / §15).
//
// The canonical identity + diagnostics substrate shared by every render-asset
// family (shader · program · material · technique · render-phase · frame graph).
// See docs/design/raf-0-rendering-foundation-design.md §3 (type ownership).

#include <crd/renderasset/binding.hpp>
#include <crd/renderasset/cooked.hpp>
#include <crd/renderasset/dependency.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderasset/identity.hpp>
#include <crd/renderasset/registry.hpp>
