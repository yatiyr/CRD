#pragma once

#include <crd/rhi/types.hpp>

namespace crd::rhi
{
class Image
{
public:
    virtual ~Image() = default;

    [[nodiscard]] virtual const ImageDesc& desc() const noexcept = 0;
};
} // namespace crd::rhi
