#pragma once

// ---------------------------------------------------------------------------
// crd-units -- Quantity<D, T> precision-suffix aliases (Phase 3.1.7.5 v0b-1).
//
// Per ADR-0078 §2 D1, every named SI / derived quantity gets a template
// alias `Name<T> = Quantity<dim::Name, T>` plus concrete `Name32` /
// `Name64` precision-suffix aliases. Consumers pick precision per domain:
//
//   game-runtime + physics + rendering   -> 32-bit (sub-mm @ 1 km is exact)
//   CAD micrometer + aerospace + science -> 64-bit
//
// `crd-time::Duration` is `Quantity<dim::Time, f64>` (already shipped;
// deliberate accumulation-safety choice for hours-scale replay / capture).
// Code that interoperates with crd-time uses `Time64` / `Duration` (they
// are the same type) at the boundary.
//
// `Temperature<T>` and `TemperatureDelta<T>` already live in
// units_affine.hpp -- not duplicated here.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/units/dim_aliases.hpp>
#include <crd/units/quantity.hpp>

namespace crd::units
{

// ===========================================================================
// Template aliases -- Name<T> = Quantity<dim::Name, T>
// ===========================================================================

// Base / geometric
template <typename T = crd::f32> using Length            = Quantity<dim::Length, T>;
template <typename T = crd::f32> using Area              = Quantity<dim::Area, T>;
template <typename T = crd::f32> using Volume            = Quantity<dim::Volume, T>;

// Mass + amount + photometric
template <typename T = crd::f32> using Mass              = Quantity<dim::Mass, T>;
template <typename T = crd::f32> using Amount            = Quantity<dim::Amount, T>;
template <typename T = crd::f32> using LuminousI         = Quantity<dim::LuminousI, T>;

// Time + frequency
template <typename T = crd::f64> using Time              = Quantity<dim::Time, T>;
template <typename T = crd::f32> using Frequency         = Quantity<dim::Frequency, T>;

// Angle
template <typename T = crd::f32> using Angle             = Quantity<dim::Angle, T>;
template <typename T = crd::f32> using AngularVelocity   = Quantity<dim::AngularVelocity, T>;
template <typename T = crd::f32> using AngularAccel      = Quantity<dim::AngularAccel, T>;

// Kinematic
template <typename T = crd::f32> using Velocity          = Quantity<dim::Velocity, T>;
template <typename T = crd::f32> using Acceleration      = Quantity<dim::Acceleration, T>;
template <typename T = crd::f32> using Jerk              = Quantity<dim::Jerk, T>;

// Inertia
template <typename T = crd::f32> using Momentum          = Quantity<dim::Momentum, T>;
template <typename T = crd::f32> using MomentOfInertia   = Quantity<dim::MomentOfInertia, T>;
template <typename T = crd::f32> using AngularMomentum   = Quantity<dim::AngularMomentum, T>;

// Force / pressure / energy / power
template <typename T = crd::f32> using Force             = Quantity<dim::Force, T>;
template <typename T = crd::f32> using Pressure          = Quantity<dim::Pressure, T>;
template <typename T = crd::f32> using Energy            = Quantity<dim::Energy, T>;
template <typename T = crd::f32> using Power             = Quantity<dim::Power, T>;
template <typename T = crd::f32> using Torque            = Quantity<dim::Torque, T>;

// Material
template <typename T = crd::f32> using Density           = Quantity<dim::Density, T>;
template <typename T = crd::f32> using SpecificVolume    = Quantity<dim::SpecificVolume, T>;

// Electrical
template <typename T = crd::f32> using Current           = Quantity<dim::Current, T>;
template <typename T = crd::f32> using Charge            = Quantity<dim::Charge, T>;
template <typename T = crd::f32> using Voltage           = Quantity<dim::Voltage, T>;
template <typename T = crd::f32> using Resistance        = Quantity<dim::Resistance, T>;
template <typename T = crd::f32> using Conductance       = Quantity<dim::Conductance, T>;
template <typename T = crd::f32> using Capacitance       = Quantity<dim::Capacitance, T>;
template <typename T = crd::f32> using Inductance        = Quantity<dim::Inductance, T>;
template <typename T = crd::f32> using MagneticFlux      = Quantity<dim::MagneticFlux, T>;
template <typename T = crd::f32> using MagneticField     = Quantity<dim::MagneticField, T>;

// Photometric
template <typename T = crd::f32> using LuminousFlux      = Quantity<dim::LuminousFlux, T>;
template <typename T = crd::f32> using Illuminance       = Quantity<dim::Illuminance, T>;
template <typename T = crd::f32> using Luminance         = Quantity<dim::Luminance, T>;

// Thermodynamic
template <typename T = crd::f32> using HeatCapacity        = Quantity<dim::HeatCapacity, T>;
template <typename T = crd::f32> using SpecificHeat        = Quantity<dim::SpecificHeat, T>;
template <typename T = crd::f32> using ThermalConductivity = Quantity<dim::ThermalConductivity, T>;
template <typename T = crd::f32> using HeatFlux            = Quantity<dim::HeatFlux, T>;

// Fluid
template <typename T = crd::f32> using DynamicViscosity   = Quantity<dim::DynamicViscosity, T>;
template <typename T = crd::f32> using KinematicViscosity = Quantity<dim::KinematicViscosity, T>;
template <typename T = crd::f32> using MassFlowRate       = Quantity<dim::MassFlowRate, T>;
template <typename T = crd::f32> using VolumetricFlowRate = Quantity<dim::VolumetricFlowRate, T>;

// Dimensionless
template <typename T = crd::f32> using Dimensionless     = Quantity<dim::Dimensionless, T>;

// ===========================================================================
// Concrete precision-suffix aliases
// ===========================================================================
//
// Naming: <Name>32 = <Name><f32>, <Name>64 = <Name><f64>. Pin: the bare
// `Name` (default-template) is `<f32>` for game-runtime; the `Time` alias
// defaults to `<f64>` to match `crd-time::Duration`.

using Length32 = Length<crd::f32>;  using Length64 = Length<crd::f64>;
using Area32   = Area<crd::f32>;    using Area64   = Area<crd::f64>;
using Volume32 = Volume<crd::f32>;  using Volume64 = Volume<crd::f64>;
using Mass32   = Mass<crd::f32>;    using Mass64   = Mass<crd::f64>;
using Time32   = Time<crd::f32>;    using Time64   = Time<crd::f64>;
using Frequency32 = Frequency<crd::f32>;  using Frequency64 = Frequency<crd::f64>;
using Angle32  = Angle<crd::f32>;   using Angle64  = Angle<crd::f64>;

using Velocity32     = Velocity<crd::f32>;     using Velocity64     = Velocity<crd::f64>;
using Acceleration32 = Acceleration<crd::f32>; using Acceleration64 = Acceleration<crd::f64>;
using Force32        = Force<crd::f32>;        using Force64        = Force<crd::f64>;
using Pressure32     = Pressure<crd::f32>;     using Pressure64     = Pressure<crd::f64>;
using Energy32       = Energy<crd::f32>;       using Energy64       = Energy<crd::f64>;
using Power32        = Power<crd::f32>;        using Power64        = Power<crd::f64>;
using Torque32       = Torque<crd::f32>;       using Torque64       = Torque<crd::f64>;
using Density32      = Density<crd::f32>;      using Density64      = Density<crd::f64>;

using Voltage32     = Voltage<crd::f32>;     using Voltage64     = Voltage<crd::f64>;
using Current32     = Current<crd::f32>;     using Current64     = Current<crd::f64>;
using Resistance32  = Resistance<crd::f32>;  using Resistance64  = Resistance<crd::f64>;

using AngularVelocity32 = AngularVelocity<crd::f32>; using AngularVelocity64 = AngularVelocity<crd::f64>;

} // namespace crd::units
