#pragma once

#include <crd/rhi/types.hpp>

namespace crd::rhi
{
class Pipeline
{
public:
    virtual ~Pipeline() = default;

    [[nodiscard]] virtual const GraphicsPipelineDesc& desc() const noexcept = 0;
};
} // namespace crd::rhi
