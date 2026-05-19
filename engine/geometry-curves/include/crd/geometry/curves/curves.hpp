#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — umbrella header. Phase 3.1.7 v10a (2026-05-19).
//
// Convenience include that pulls in every curve type + the evaluator + the
// validators. Consumers wanting a narrow include should pull the specific
// `<crd/geometry/curves/X.hpp>` header instead.
// ---------------------------------------------------------------------------

#include <crd/geometry/curves/arc.hpp>
#include <crd/geometry/curves/arclength.hpp>
#include <crd/geometry/curves/bezier.hpp>
#include <crd/geometry/curves/bspline.hpp>
#include <crd/geometry/curves/catmull_rom.hpp>
#include <crd/geometry/curves/evaluator.hpp>
#include <crd/geometry/curves/frames.hpp>
#include <crd/geometry/curves/hermite.hpp>
#include <crd/geometry/curves/polyline.hpp>
#include <crd/geometry/curves/queries.hpp>
#include <crd/geometry/curves/queries_typed.hpp>
#include <crd/geometry/curves/sample.hpp>
#include <crd/geometry/curves/types.hpp>
#include <crd/geometry/curves/validate.hpp>
