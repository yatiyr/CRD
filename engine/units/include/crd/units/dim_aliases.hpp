#pragma once

// ---------------------------------------------------------------------------
// crd-units — named base + derived dimension aliases (Phase 3.1.7.5 v0a-1).
//
// Base SI dimensions + Angle as the tagged 8th base. Derived dimensions
// (Velocity / Acceleration / Force / Pressure / Energy / Power / etc.) are
// declared as type aliases over DimMul/DimDiv so they are structurally
// identical to the algebraic combinations (`dim_equal_v<dim::Velocity,
// DimDiv<dim::Length, dim::Time>> == true`).
//
// **Note on dimensionally-degenerate distinctions** (strict SI):
//   - Energy and Torque both reduce to kg·m²·s⁻² (Dim<2, 1, -2, 0, ...>).
//     In strict SI they are dimensionally equal. mp-units handles this via
//     a "kind" tag in Quantity that lives orthogonal to Dim<>. v0a-1 does
//     NOT ship the kind tag — Energy and Torque are deliberately aliases.
//     Disambiguation is a v0a-3 (or later) amendment when a consumer
//     surfaces.
//   - Frequency and AngularVelocity both reduce to s⁻¹ in strict SI
//     (Cerid's tag of Angle as the 8th base lets us split: Frequency =
//     Dim<0,0,-1,0,...,0> vs AngularVelocity = Dim<0,0,-1,...,1>). These
//     are dimensionally distinct in our system.
//   - LuminousFlux (cd·sr) and LuminousI both reduce to cd in strict SI
//     (steradian is dimensionless m²/m²). v0a-1 aliases LuminousFlux to
//     LuminousI; a SolidAngle 9th tag is a future amendment if needed.
// ---------------------------------------------------------------------------

#include <crd/units/dim.hpp>

namespace crd::units::dim
{

// ===========================================================================
// Base dimensions
// ===========================================================================
//                              L   M   T   I   Th  N   J   A
using Dimensionless = Dim<      0,  0,  0,  0,  0,  0,  0,  0>;
using Length        = Dim<      1,  0,  0,  0,  0,  0,  0,  0>;
using Mass          = Dim<      0,  1,  0,  0,  0,  0,  0,  0>;
using Time          = Dim<      0,  0,  1,  0,  0,  0,  0,  0>;
using Current       = Dim<      0,  0,  0,  1,  0,  0,  0,  0>;
using Temperature   = Dim<      0,  0,  0,  0,  1,  0,  0,  0>;
using Amount        = Dim<      0,  0,  0,  0,  0,  1,  0,  0>;
using LuminousI     = Dim<      0,  0,  0,  0,  0,  0,  1,  0>;
using Angle         = Dim<      0,  0,  0,  0,  0,  0,  0,  1>;

// ===========================================================================
// Derived — algebraic
// ===========================================================================

// Geometric
using Area              = DimMul<Length, Length>;
using Volume            = DimMul<Area, Length>;
using SolidAngle        = Dimensionless;  // sr = m²/m² in strict SI; future 9th tag if needed

// Kinematic
using Velocity          = DimDiv<Length, Time>;
using Acceleration      = DimDiv<Velocity, Time>;
using Jerk              = DimDiv<Acceleration, Time>;
using AngularVelocity   = DimDiv<Angle, Time>;
using AngularAccel      = DimDiv<AngularVelocity, Time>;
using Frequency         = DimInv<Time>;

// Inertia
using Momentum          = DimMul<Mass, Velocity>;
using MomentOfInertia   = DimMul<Mass, Area>;
using AngularMomentum   = DimMul<MomentOfInertia, AngularVelocity>;

// Inverse forms (Phase 3.1.7.5 v0c-1) — eylem RigidBody stores 1/mass and
// 1/inertia for div-by-zero-free constraint math (Bullet / Box2D / PhysX
// convention). Typing them as DimInv<Mass> / DimInv<MomentOfInertia> lets
// `Force * InverseMass -> Acceleration` and `Torque * InverseMOI ->
// AngularAccel` type-check end-to-end.
using InverseMass         = DimInv<Mass>;
using InverseMomentOfInertia = DimInv<MomentOfInertia>;

// Force / pressure / energy / power
using Force             = DimMul<Mass, Acceleration>;
using Pressure          = DimDiv<Force, Area>;
using Energy            = DimMul<Force, Length>;
using Power             = DimDiv<Energy, Time>;
using Torque            = DimMul<Force, Length>;  // Dim<> identical to Energy in strict SI; see header comment.

// Material
using Density           = DimDiv<Mass, Volume>;
using SpecificVolume    = DimInv<Density>;

// Thermodynamic (consumed by future Phase 3.1.10 crd-cfd, Phase 3.1.13 crd-cam additive)
using HeatCapacity      = DimDiv<Energy, Temperature>;
using SpecificHeat      = DimDiv<HeatCapacity, Mass>;
using ThermalConductivity = DimDiv<DimDiv<Power, Length>, Temperature>;
using HeatFlux          = DimDiv<Power, Area>;
using EntropyDim        = HeatCapacity;  // J/K — same Dim<> as heat capacity

// Electrical (consumed by future Phase 3.1.17 crd-eda + EDA-side modules)
using Charge            = DimMul<Current, Time>;
using Voltage           = DimDiv<Power, Current>;
using Resistance        = DimDiv<Voltage, Current>;
using Conductance       = DimInv<Resistance>;
using Capacitance       = DimDiv<Charge, Voltage>;
using Inductance        = DimDiv<DimMul<Voltage, Time>, Current>;
using MagneticFlux      = DimMul<Voltage, Time>;
using MagneticField     = DimDiv<MagneticFlux, Area>;

// Photometric (consumed by future Phase 3.5+ rendering area lights)
// LuminousFlux = cd*sr; sr is dimensionless in strict SI, so structurally identical to LuminousI.
using LuminousFlux      = LuminousI;
using Illuminance       = DimDiv<LuminousFlux, Area>;
using Luminance         = DimDiv<LuminousI, Area>;

// Fluid (consumed by future Phase 3.1.10 crd-cfd)
using DynamicViscosity   = DimMul<Pressure, Time>;
using KinematicViscosity = DimDiv<DynamicViscosity, Density>;
using MassFlowRate       = DimDiv<Mass, Time>;
using VolumetricFlowRate = DimDiv<Volume, Time>;

} // namespace crd::units::dim
