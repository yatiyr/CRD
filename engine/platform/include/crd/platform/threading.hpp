#pragma once

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::platform::threading
{
void set_current_thread_name(containers::StringView name) noexcept;

[[nodiscard]] u32 current_thread_id() noexcept;
[[nodiscard]] u32 hardware_concurrency() noexcept;
[[nodiscard]] u32 logical_core_count() noexcept;
[[nodiscard]] u32 physical_core_count() noexcept;

[[nodiscard]] bool set_thread_affinity(u32 core_index) noexcept;
void cpu_pause() noexcept;
} // namespace crd::platform::threading
