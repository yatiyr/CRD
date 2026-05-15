#pragma once

// ---------------------------------------------------------------------------
// crd-units — umbrella header.
//
// At v0a-1 close: dim.hpp + dim_aliases.hpp + quantity.hpp.
// At v0a-2 close: + units_si.hpp + units_compound.hpp + value_in.hpp.
// At v0a-3 close: + units_affine.hpp + units_nonlinear.hpp + literals.hpp
//                 + vec_quantity.hpp + format.hpp.
//
// Consumers include `<crd/units/units.hpp>` to pull the full surface;
// individual headers can be included for narrower compile-time cost.
// ---------------------------------------------------------------------------

#include <crd/units/dim.hpp>
#include <crd/units/dim_aliases.hpp>
#include <crd/units/literals.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/units_affine.hpp>
#include <crd/units/units_compound.hpp>
#include <crd/units/units_nonlinear.hpp>
#include <crd/units/units_si.hpp>
#include <crd/units/value_in.hpp>
