#pragma once

// DIAG.7a(d2b-dx12-a): attach a stable Cerid identity to a native ID3D12Object as its debug name, and mint/retire it
// in the process-wide identity registry. Src-private to gpu-context-dx12 -- the vendor type (ID3D12Object) stays out
// of the public gpu-context modules; only crd::gpu::ObjectIdentity crosses that boundary.
//
// DECLARATIONS ONLY -- defined out-of-line in dx12_identity_naming.cpp: keeps <d3d12.h> and the SetName/widen body in
// one .cpp instead of inlining it into every TU that names an object. (A win-clang-cl-shipping thin-LTO lld-link
// crash seen during DIAG.7a(d2b) is a NON-DETERMINISTIC toolchain crash in LTO codegen -- varying signature,
// reproduces even in isolation -- not tied to this helper or the DX12 wiring; see the session doc.)
//
// The debug name is the bracketed form format_debug_name() builds, e.g. "[crd:res:0000002a:g00000007] dx12-storage".
// A validation message about the object then resolves back to the identity via the DIAG.7a(d1) capture parse, and the
// same name reads back in-process via GetPrivateData(WKPDID_D3DDebugObjectNameW) -- how (d2b-dx12-a) proves the wire.

#include <crd/gpu/object_identity.hpp>

#include <string_view>

struct ID3D12Object; // forward-declared: <d3d12.h> stays in the .cpp, not in this header

namespace crd::gpu::detail
{

// Mint an identity of `kind`, set `object`'s debug name to "[<encoded id>] <site>", and return the identity. A failed
// SetName is non-fatal (debug names are best-effort and never fail resource creation; counting such failures is (e)),
// and the identity is minted regardless so retire-on-destroy lifetime tracking still holds. Equivalent to
// mint(kind) + dx12_name_object(object, id, site).
[[nodiscard]] ObjectIdentity dx12_attach_identity(ID3D12Object* object, ObjectKind kind, std::string_view site) noexcept;

// DIAG.7a(d2b-dx12-b): name a native object with an EXISTING identity -- no mint. A logical resource that owns several
// native objects (a raster target's colour + resolve + depth + readback + rtv/dsv heaps; a G-buffer's N colour planes)
// gets ONE identity, minted once on its primary via dx12_attach_identity, then stamped onto every sibling here. A
// validation message about ANY of those objects then resolves to the one logical identity -- the choice point's
// "one identity per logical resource, not per native object". No-op on a null object; a failed SetName is non-fatal.
void dx12_name_object(ID3D12Object* object, const ObjectIdentity& id, std::string_view site) noexcept;

// Retire an identity when its native object is destroyed. The generation bump means a later message about the freed
// object parses to alive()==false -- the "recently retired provenance" primitive lifecycle coverage (e) builds on.
void dx12_detach_identity(const ObjectIdentity& id) noexcept;

} // namespace crd::gpu::detail
