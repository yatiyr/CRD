#include <crd/shader/types.hpp>

namespace crd::shader
{
namespace
{
[[nodiscard]] constexpr crd::u64 fnv1a_mix(crd::u64 hash, crd::u64 value) noexcept
{
    constexpr crd::u64 kPrime = 1099511628211ULL;
    hash ^= value;
    hash *= kPrime;
    return hash;
}
} // namespace

VariantKey make_variant_key(const VariantRequest& request) noexcept
{
    constexpr crd::u64 kOffsetBasis = 14695981039346656037ULL;
    crd::u64 hash = kOffsetBasis;
    hash = fnv1a_mix(hash, static_cast<crd::u64>(request.pass_type));
    hash = fnv1a_mix(hash, request.skinned ? 1ULL : 0ULL);
    hash = fnv1a_mix(hash, static_cast<crd::u64>(request.alpha_mode));
    hash = fnv1a_mix(hash, static_cast<crd::u64>(request.render_path));
    return VariantKey{hash};
}

MechanismDecision decide_mechanism(VariantAxis axis) noexcept
{
    switch (axis)
    {
        case VariantAxis::PassType:
            return {axis, Mechanism::Permutation,
                    crd::containers::String("Pass type changes shader structure and pipeline compatibility")};
        case VariantAxis::Skinning:
            return {axis, Mechanism::Permutation,
                    crd::containers::String("Skinned/static changes vertex input and shader structure")};
        case VariantAxis::AlphaMode:
            return {axis, Mechanism::Permutation,
                    crd::containers::String("Alpha mode changes structural shader path and render state expectations")};
        case VariantAxis::RenderPath:
            return {axis, Mechanism::Permutation,
                    crd::containers::String("Forward/deferred is a structural render-path distinction")};
        case VariantAxis::CascadeCount:
            return {axis, Mechanism::SpecializationConstant,
                    crd::containers::String("Cascade count is numeric compile-time configuration")};
        case VariantAxis::LightCap:
            return {axis, Mechanism::SpecializationConstant,
                    crd::containers::String("Light cap is a numeric compile-time limit")};
        case VariantAxis::LoopBound:
            return {axis, Mechanism::SpecializationConstant,
                    crd::containers::String("Loop bound is a numeric compile-time parameter")};
        case VariantAxis::MaterialParameter:
            return {
                axis, Mechanism::ResourceBinding,
                crd::containers::String("Material data belongs in bindless/UBO/push-constant space, not permutations")};
        case VariantAxis::CheapRuntimeToggle:
            return {axis, Mechanism::DynamicBranch,
                    crd::containers::String("Cheap low-frequency runtime toggle is best expressed as a branch")};
        default:
            return {axis, Mechanism::Rejected, crd::containers::String("Unknown axis")};
    }
}
} // namespace crd::shader
