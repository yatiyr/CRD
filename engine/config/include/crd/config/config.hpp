#pragma once

#include <crd/config/log_channel.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/math/vec.hpp>
#include <crd/platform/filesystem.hpp>

#include <memory>
#include <type_traits>

namespace crd::config
{
class Config
{
public:
    Config() noexcept;
    ~Config() noexcept;

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    Config(Config&&) noexcept;
    Config& operator=(Config&&) noexcept;

    [[nodiscard]] bool load_from_string(crd::containers::StringView toml_text) noexcept;
    [[nodiscard]] bool load_from_file(const crd::platform::fs::Path& path) noexcept;
    [[nodiscard]] bool reload() noexcept;

    [[nodiscard]] bool is_loaded() const noexcept { return m_loaded; }
    [[nodiscard]] const crd::platform::fs::Path& source_path() const noexcept { return m_source_path; }

    [[nodiscard]] bool contains(crd::containers::StringView key) const noexcept;

    template <typename ValueType>
    [[nodiscard]] ValueType get(crd::containers::StringView key, const ValueType& fallback) const noexcept
    {
        return get_impl<ValueType>(key, fallback);
    }

    template <typename ValueType> void set(crd::containers::StringView key, const ValueType& value)
    {
        set_impl<ValueType>(key, value);
    }

private:
    template <typename ValueType>
    [[nodiscard]] ValueType get_impl(crd::containers::StringView key, const ValueType& fallback) const noexcept;

    template <typename ValueType> void set_impl(crd::containers::StringView key, const ValueType& value);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    crd::platform::fs::Path m_source_path{};
    bool m_loaded = false;
};
} // namespace crd::config
