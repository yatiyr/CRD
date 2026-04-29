#include <crd/shader/runtime.hpp>

#include <unordered_map>

namespace crd::shader
{
namespace
{
class StoredEffect final : public Effect
{
public:
    StoredEffect(EffectHandle handle, EffectDesc desc) : m_handle(handle), m_desc(std::move(desc)) {}

    [[nodiscard]] EffectHandle handle() const noexcept override { return m_handle; }
    [[nodiscard]] crd::containers::StringView name() const noexcept override { return m_desc.name; }
    [[nodiscard]] crd::containers::ConstSpan<ParameterDesc> parameters() const noexcept override
    {
        return m_desc.parameters;
    }
    [[nodiscard]] crd::containers::ConstSpan<DescriptorBindingDesc> descriptor_bindings() const noexcept override
    {
        return m_desc.descriptor_bindings;
    }
    [[nodiscard]] crd::containers::ConstSpan<PushConstantRangeDesc> push_constants() const noexcept override
    {
        return m_desc.push_constants;
    }
    [[nodiscard]] crd::containers::ConstSpan<VertexAttributeLayoutDesc> vertex_attributes() const noexcept override
    {
        return m_desc.vertex_attributes;
    }

private:
    EffectHandle m_handle{};
    EffectDesc m_desc{};
};

class LocalRuntime final : public Runtime
{
public:
    [[nodiscard]] EffectHandle create_effect(const EffectDesc& desc) override
    {
        const EffectHandle handle{m_next_effect_handle++};
        m_effects.emplace(handle.value, StoredEffect(handle, desc));
        return handle;
    }

    [[nodiscard]] const Effect* find_effect(EffectHandle handle) const noexcept override
    {
        const auto it = m_effects.find(handle.value);
        if (it == m_effects.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    [[nodiscard]] VariantHandle request_variant(const VariantCompileRequest& request,
                                                CompileDiagnostics& diagnostics) override
    {
        diagnostics.succeeded = false;
        diagnostics.message = crd::containers::String{};

        if (!request.effect.is_valid())
        {
            diagnostics.message = crd::containers::String("Variant request used an invalid effect handle");
            return {};
        }
        if (m_effects.find(request.effect.value) == m_effects.end())
        {
            diagnostics.message = crd::containers::String("Variant request referenced an unknown effect handle");
            return {};
        }

        const VariantHandle handle{m_next_variant_handle++};
        m_variants.emplace(handle.value, true);
        diagnostics.succeeded = true;
        diagnostics.message = crd::containers::String("Envelope-only runtime accepted variant request");
        return handle;
    }

    [[nodiscard]] bool is_variant_ready(VariantHandle handle) const noexcept override
    {
        const auto it = m_variants.find(handle.value);
        return it != m_variants.end() && it->second;
    }

    [[nodiscard]] bool reload_effect(EffectHandle handle, ReloadEvent& event) override
    {
        event.effect = handle;
        event.using_last_good = false;
        event.succeeded = find_effect(handle) != nullptr;
        return event.succeeded;
    }

private:
    crd::u64 m_next_effect_handle = 1;
    crd::u64 m_next_variant_handle = 1;
    std::unordered_map<crd::u64, StoredEffect> m_effects{};
    std::unordered_map<crd::u64, bool> m_variants{};
};
} // namespace

std::unique_ptr<Runtime> create_runtime()
{
    return std::make_unique<LocalRuntime>();
}
} // namespace crd::shader
