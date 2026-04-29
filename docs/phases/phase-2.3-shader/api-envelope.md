# `crd-shader` API Envelope (strawman)

Headers only. This is an envelope, not an implementation contract yet.

```cpp
// include/crd/shader/types.hpp
namespace crd::shader
{
enum class Stage : u8 { Vertex, Fragment, Compute };

struct ModuleHandle { u64 value = 0; };
struct EffectHandle { u64 value = 0; };
struct VariantHandle { u64 value = 0; };

enum class ParameterClass : u8
{
    Texture,
    Sampler,
    Buffer,
    PushConstant,
    SpecializationConstant,
};

struct ParameterDesc
{
    containers::String name{};
    ParameterClass parameter_class = ParameterClass::Texture;
    u32 set_index = 0;
    u32 binding = 0;
    u32 size_bytes = 0;
};

struct DescriptorBindingDesc
{
    u32 set_index = 0;
    u32 binding = 0;
    u32 count = 1;
    Stage visibility = Stage::Fragment;
};

struct PushConstantRangeDesc
{
    u32 offset = 0;
    u32 size_bytes = 0;
    Stage visibility = Stage::Vertex;
};

struct VertexAttributeLayoutDesc
{
    containers::String semantic{};
    u32 location = 0;
    rhi::Format format = rhi::Format::Undefined;
    u32 offset_bytes = 0;
};
}

// include/crd/shader/variant_key.hpp
namespace crd::shader
{
struct VariantKey
{
    u64 value = 0;
};

struct VariantRequest
{
    // structural axes only
    u32 pass_type = 0;
    bool skinned = false;
    u32 alpha_mode = 0;
    u32 render_path = 0;

    // numeric axes stay out of the structural key and become specialization
    // constants in backend-facing compilation requests.
};
}

// include/crd/shader/effect.hpp
namespace crd::shader
{
struct EffectDesc
{
    containers::String name{};
    containers::String source_path{};
};

class Effect
{
public:
    [[nodiscard]] virtual EffectHandle handle() const noexcept = 0;
    [[nodiscard]] virtual containers::StringView name() const noexcept = 0;
    [[nodiscard]] virtual containers::ConstSpan<ParameterDesc> parameters() const noexcept = 0;
    [[nodiscard]] virtual containers::ConstSpan<DescriptorBindingDesc> descriptor_bindings() const noexcept = 0;
    [[nodiscard]] virtual containers::ConstSpan<PushConstantRangeDesc> push_constants() const noexcept = 0;
    [[nodiscard]] virtual containers::ConstSpan<VertexAttributeLayoutDesc> vertex_attributes() const noexcept = 0;
};
}

// include/crd/shader/compiler.hpp
namespace crd::shader
{
struct FrontendCompileRequest
{
    containers::String source_path{};
    Stage stage = Stage::Vertex;
    containers::String entry_point{"main"};
};

struct SpecializationValue
{
    u32 constant_id = 0;
    u64 value = 0;
};

struct VariantCompileRequest
{
    EffectHandle effect{};
    VariantRequest variant{};
    containers::ConstSpan<SpecializationValue> specialization_values{};
};

struct CompileDiagnostics
{
    bool succeeded = false;
    containers::String message{};
};
}

// include/crd/shader/runtime.hpp
namespace crd::shader
{
struct ReloadEvent
{
    EffectHandle effect{};
    bool succeeded = false;
    bool using_last_good = false;
};

class Runtime
{
public:
    virtual ~Runtime() = default;

    [[nodiscard]] virtual EffectHandle create_effect(const EffectDesc& desc) = 0;
    [[nodiscard]] virtual const Effect* find_effect(EffectHandle handle) const noexcept = 0;
    [[nodiscard]] virtual VariantHandle request_variant(const VariantCompileRequest& request,
                                                        CompileDiagnostics& diagnostics) = 0;

    [[nodiscard]] virtual bool is_variant_ready(VariantHandle handle) const noexcept = 0;
    [[nodiscard]] virtual bool reload_effect(EffectHandle handle, ReloadEvent& event) = 0;
};
}
```

## Envelope properties proved by construction

- No GLSL/SPIR-V/Vulkan types in the public surface.
- A node-editor frontend can plug in by producing the same canonical IR-facing
  compile requests and effect metadata, without changing consumers.
- A material system can hold `EffectHandle` and request variants by typed key.
- Hot-reload is observable (`ReloadEvent`) without implying that consumers can
  crash mid-frame when source is bad.
