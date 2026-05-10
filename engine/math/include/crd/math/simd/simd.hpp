// Umbrella header for crd::math::simd — Phase 3.1 v0a.
//
// SIMD wrapper types for the AoSoA-N storage that eylem (Phase 3.1) and
// hesap (Phase 3.1.6) are built on. Lives in `crd::math::simd` to stay
// clear of `crd::math::Vec4f` (the 4D math vector). Backend chosen at
// compile time per `backend.hpp`; see ADR-0063 for the determinism contract
// these types obey.

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/math/simd/vec4i.hpp>
#include <crd/math/simd/vec8i.hpp>
#include <crd/math/simd/convert.hpp>
#include <crd/math/simd/mat4f.hpp>
#include <crd/math/simd/quatf.hpp>
#include <crd/math/simd/soa.hpp>
