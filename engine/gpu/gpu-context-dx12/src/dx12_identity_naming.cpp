// DIAG.7a(d2b-dx12-a): the single out-of-line definition of the DX12 identity-naming helpers -- keeping <d3d12.h> and
// the SetName/widen body in one .cpp rather than inlining it into every TU that names an object (out-of-line on its own
// merits). A win-clang-cl-shipping thin-LTO lld-link crash seen during DIAG.7a(d2b) is a NON-DETERMINISTIC toolchain
// crash in LTO codegen (varying signature, reproduces in isolation), not tied to this code; see the session doc.

#include "dx12_identity_naming.hpp"

#include <crd/gpu/identity_registry.hpp>

#include <d3d12.h>

namespace crd::gpu::detail
{

void dx12_name_object(ID3D12Object* object, const ObjectIdentity& id, std::string_view site) noexcept
{
    if (object == nullptr)
    {
        return;
    }
    char             narrow[kDebugNamePrefixChars + 96];
    const crd::usize n = format_debug_name(id, site, narrow, sizeof(narrow));
    if (n == 0U)
    {
        return;
    }
    wchar_t wide[kDebugNamePrefixChars + 96];
    for (crd::usize i = 0; i < n; ++i)
    {
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(narrow[i])); // pure-ASCII: direct widen
    }
    wide[n] = L'\0';
    (void)object->SetName(wide); // best-effort; a failed debug name must not fail creation
}

ObjectIdentity dx12_attach_identity(ID3D12Object* object, ObjectKind kind, std::string_view site) noexcept
{
    const ObjectIdentity id = identity_registry().mint(kind);
    dx12_name_object(object, id, site); // one mint, then name the primary native object with it
    return id;
}

void dx12_detach_identity(const ObjectIdentity& id) noexcept
{
    (void)identity_registry().retire(id);
}

Dx12PassEventScope::Dx12PassEventScope(ID3D12GraphicsCommandList* list, const ObjectIdentity& id,
                                       const char* name) noexcept
{
    if (list == nullptr)
    {
        return;
    }
    const std::string_view nm = (name != nullptr) ? std::string_view(name) : std::string_view("pass");
    char                   narrow[kDebugNamePrefixChars + 96];
    const crd::usize       n = format_debug_name(id, nm, narrow, sizeof(narrow)); // 0 if invalid/too small
    if (n == 0U) // do NOT Begin -> the dtor stays a no-op (balanced)
    {
        return;
    }
    wchar_t wide[kDebugNamePrefixChars + 96];
    for (crd::usize i = 0; i < n; ++i)
    {
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(narrow[i]));
    }
    wide[n] = L'\0';
    constexpr UINT unicode_event = 0U; // the legacy WINPIX_EVENT_UNICODE_VERSION metadata
    list->BeginEvent(unicode_event, wide, static_cast<UINT>((n + 1U) * sizeof(wchar_t)));
    m_list = list; // Begin ran -> arm End
}

Dx12PassEventScope::~Dx12PassEventScope() noexcept
{
    if (m_list != nullptr)
    {
        m_list->EndEvent();
    }
}

} // namespace crd::gpu::detail
