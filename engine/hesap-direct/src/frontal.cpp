#include <crd/hesap/complex.hpp>
#include <crd/hesap/direct/frontal.hpp>

// Explicit instantiations of the extend_add assembly kernel across the four
// hesap value types (f32 / f64 / Complex32 / Complex64). Gives the STATIC lib
// concrete symbols + a compile check of the substrate; the template body lives
// in frontal.hpp so consumers in other modules instantiate on use.
namespace crd::hesap::direct
{
template void extend_add<crd::f32>(Frontal<crd::f32>&, const Frontal<crd::f32>&, crd::memory::IAllocator*);
template void extend_add<crd::f64>(Frontal<crd::f64>&, const Frontal<crd::f64>&, crd::memory::IAllocator*);
template void extend_add<Complex<crd::f32>>(Frontal<Complex<crd::f32>>&, const Frontal<Complex<crd::f32>>&,
                                            crd::memory::IAllocator*);
template void extend_add<Complex<crd::f64>>(Frontal<Complex<crd::f64>>&, const Frontal<Complex<crd::f64>>&,
                                            crd::memory::IAllocator*);
} // namespace crd::hesap::direct
