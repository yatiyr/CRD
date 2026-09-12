// crd-shader-cook — the D5 hot-reload implementation (ADR-0104). See reload.hpp for the contract.
#include <crd/shadercook/reload.hpp>

#include <crd/kir/ckir_serialize.hpp>

#include <cstring>

namespace crd::shadercook
{

ReloadableCompute::Status ReloadableCompute::reload(
    const crd::kir::KGraph& g, const crd::kir::KEntry& e, crd::containers::StringView name, CookBackend backend,
    PipelineCreateFn create, void* user)
{
    Status st;

    // Content hash of the IR = the D2 cook cache key. An unchanged graph re-uses the live pipeline (a cheap no-op).
    crd::containers::Array<crd::u8> ir = crd::kir::serialize_graph(g, e, m_alloc);
    const crd::resources::ResourceId h = crd::resources::ResourceId::from_content(crd::containers::as_const_span(ir));
    if (m_current && h == m_hash)
    {
        st.ok = true; // already live at this hash — nothing to rebuild
        return st;
    }

    CookOptions opts;
    opts.backends     = static_cast<crd::u32>(backend);
    CookResult ck = cook_compute_shader(g, e, name, opts, m_alloc);
    if (!ck.ok) { return st; }
    st.ok = true;

    ShaderBundle bundle(m_alloc);
    if (!read_shader_bundle(crd::containers::as_const_span(ck.crdr), bundle)) { st.ok = false; return st; }
    const auto refl_span = bundle.reflection();
    if (refl_span.size() != sizeof(crd::kir::ShaderReflection)) { st.ok = false; return st; }
    crd::kir::ShaderReflection refl{};
    std::memcpy(&refl, refl_span.data(), sizeof(refl));

    auto pipe = create(bundle.bytecode(backend), refl.n_bindings, user);
    if (pipe == nullptr) { st.ok = false; return st; }

    // Atomic swap: retire the previous pipeline (kept one generation), install the new one, adopt the new bundle + hash.
    m_retired = std::move(m_current);
    m_current = std::move(pipe);
    m_crdr    = std::move(ck.crdr);
    m_hash    = h;
    ++m_generation;
    st.changed = true;
    return st;
}

} // namespace crd::shadercook
