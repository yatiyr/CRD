#pragma once

#include <crd/rhi/types.hpp>

namespace crd::rhi
{
class ShaderModule
{
public:
    virtual ~ShaderModule() = default;

    [[nodiscard]] virtual ShaderStage stage() const noexcept = 0;
    [[nodiscard]] virtual crd::containers::StringView entry_point() const noexcept = 0;
};
} // namespace crd::rhi
