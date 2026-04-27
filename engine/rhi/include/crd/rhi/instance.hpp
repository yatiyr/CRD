#pragma once

#include <crd/containers/array.hpp>
#include <crd/rhi/device.hpp>

#include <memory>

namespace crd::rhi
{
class Instance
{
public:
    virtual ~Instance() = default;

    [[nodiscard]] virtual BackendApi api() const noexcept = 0;
    virtual void enumerate_adapters(crd::containers::Array<AdapterInfo>& out) const = 0;
    [[nodiscard]] virtual std::unique_ptr<Device> create_device(const DeviceDesc& desc) = 0;
};
} // namespace crd::rhi
