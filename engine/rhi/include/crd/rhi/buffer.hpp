#pragma once

#include <crd/rhi/types.hpp>

namespace crd::rhi
{
class Buffer
{
public:
    virtual ~Buffer() = default;

    [[nodiscard]] virtual const BufferDesc& desc() const noexcept = 0;
    [[nodiscard]] virtual void* map() noexcept = 0;
    virtual void unmap() noexcept = 0;
};
} // namespace crd::rhi
