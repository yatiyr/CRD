// DIAG.7a(d2b): the single out-of-line definition of the process-wide identity registry accessor.
//
// Deliberately NOT an inline function in the header: an inline Meyers singleton whose local static is a mutex +
// SlotMap<u8>[3] would emit a linkonce_odr guard variable and registered destructor into every calling TU. Defining
// the accessor once, here, gives the process-wide static exactly one home -- good practice on its own merits.
// NOTE: a win-clang-cl-shipping thin-LTO lld-link crash seen during DIAG.7a(d2b) is a NON-DETERMINISTIC toolchain crash
// in LTO codegen (varying signature, reproduces even in isolation), not tied to this code. It is not
// this out-of-line move's concern; see the session doc.

#include <crd/gpu/identity_registry.hpp>

namespace crd::gpu
{

IdentityRegistry& identity_registry()
{
    static IdentityRegistry instance; // default_allocator(); first-use init, destroyed at exit after that allocator
    return instance;
}

} // namespace crd::gpu
