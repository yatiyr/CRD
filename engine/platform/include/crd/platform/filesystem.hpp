#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>

namespace crd::platform::fs
{
// Path - UTF-8 engine-facing path wrapper.
//
// Internally we store forward slashes (`/`) in `m_generic`, regardless of
// host OS. This keeps logs, serialization, and string comparisons stable.
// OS-native conversions happen at the platform boundary only.
class Path
{
public:
    Path() noexcept = default;
    explicit Path(containers::String s);
    explicit Path(containers::StringView sv);
    explicit Path(const char* cstr);

    [[nodiscard]] bool empty() const noexcept { return m_generic.empty(); }
    [[nodiscard]] containers::StringView generic() const noexcept
    {
        return containers::StringView{m_generic.data(), m_generic.size()};
    }
    [[nodiscard]] containers::String native() const;

    [[nodiscard]] Path operator/(containers::StringView segment) const;

    friend bool operator==(const Path& a, const Path& b) noexcept { return a.m_generic == b.m_generic; }

private:
    static void normalize_in_place(containers::String& s);

    containers::String m_generic{};
};

[[nodiscard]] Path current_working_dir() noexcept;
[[nodiscard]] Path executable_dir() noexcept;
[[nodiscard]] Path user_config_dir(containers::StringView app_name) noexcept;

[[nodiscard]] bool exists(const Path& path) noexcept;
[[nodiscard]] bool is_file(const Path& path) noexcept;
[[nodiscard]] bool is_directory(const Path& path) noexcept;
[[nodiscard]] u64 file_size(const Path& path) noexcept;
[[nodiscard]] i64 last_modified_unix_seconds(const Path& path) noexcept;

[[nodiscard]] bool read_file_text(const Path& path, containers::String& out) noexcept;
[[nodiscard]] bool read_file_binary(const Path& path, containers::Array<u8>& out) noexcept;
[[nodiscard]] bool read_file_range(const Path& path, crd::u64 offset, crd::u64 size, containers::Array<u8>& out) noexcept;
[[nodiscard]] bool write_file_text(const Path& path, containers::StringView contents) noexcept;
[[nodiscard]] bool write_file_binary(const Path& path, containers::ConstSpan<u8> contents) noexcept;

[[nodiscard]] bool create_directories(const Path& path) noexcept;
[[nodiscard]] bool remove_file(const Path& path) noexcept;
[[nodiscard]] bool remove_all(const Path& path) noexcept;
void list_directory(const Path& path, containers::Array<Path>& out) noexcept;
} // namespace crd::platform::fs
