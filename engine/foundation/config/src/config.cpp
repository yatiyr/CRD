#include <crd/config/config.hpp>
#include <crd/log/log.hpp>

#include <memory>
#include <toml++/toml.hpp>

namespace crd::config
{
struct Config::Impl
{
    toml::table root{};
};

namespace
{
[[nodiscard]] crd::containers::Array<crd::containers::String> split_key(crd::containers::StringView key)
{
    crd::containers::Array<crd::containers::String> parts;
    crd::usize start = 0;
    for (crd::usize i = 0; i <= key.size(); ++i)
    {
        if (i == key.size() || key[i] == '.')
        {
            if (i > start)
            {
                parts.push_back(crd::containers::String(key.substr(start, i - start)));
            }
            start = i + 1;
        }
    }
    return parts;
}

[[nodiscard]] const toml::node* find_node(const toml::table& root, crd::containers::StringView key) noexcept
{
    const toml::node* current = &root;
    const auto parts = split_key(key);
    for (const auto& part : parts)
    {
        const auto* table = current->as_table();
        if (table == nullptr)
        {
            return nullptr;
        }
        current = table->get(part.c_str());
        if (current == nullptr)
        {
            return nullptr;
        }
    }
    return current;
}

[[nodiscard]] toml::table* ensure_parent_table(toml::table& root, crd::containers::StringView key)
{
    const auto parts = split_key(key);
    toml::table* current = &root;
    if (parts.empty())
    {
        return current;
    }
    for (crd::usize i = 0; i + 1 < parts.size(); ++i)
    {
        toml::node* node = current->get(parts[i].c_str());
        if (node == nullptr || !node->is_table())
        {
            current->insert_or_assign(parts[i].c_str(), toml::table{});
            node = current->get(parts[i].c_str());
        }
        current = node->as_table();
        CRD_ASSERT(current != nullptr);
    }
    return current;
}

[[nodiscard]] crd::containers::String leaf_key(crd::containers::StringView key)
{
    const auto parts = split_key(key);
    CRD_ASSERT(parts.size() > 0);
    return parts.back();
}

template <typename ValueType> [[nodiscard]] bool read_scalar(const toml::node& node, ValueType& out) noexcept
{
    if constexpr (std::is_same_v<ValueType, crd::i64>)
    {
        if (auto value = node.value<crd::i64>())
        {
            out = *value;
            return true;
        }
    }
    else if constexpr (std::is_same_v<ValueType, int>)
    {
        if (auto value = node.value<crd::i64>())
        {
            out = static_cast<int>(*value);
            return true;
        }
    }
    else if constexpr (std::is_same_v<ValueType, crd::f64>)
    {
        if (auto value = node.value<double>())
        {
            out = static_cast<crd::f64>(*value);
            return true;
        }
    }
    else if constexpr (std::is_same_v<ValueType, crd::f32>)
    {
        if (auto value = node.value<double>())
        {
            out = static_cast<crd::f32>(*value);
            return true;
        }
    }
    else if constexpr (std::is_same_v<ValueType, bool>)
    {
        if (auto value = node.value<bool>())
        {
            out = *value;
            return true;
        }
    }
    else if constexpr (std::is_same_v<ValueType, crd::containers::String>)
    {
        if (auto value = node.value<std::string>())
        {
            out = crd::containers::String(value->c_str());
            return true;
        }
    }
    return false;
}

template <typename ValueType> [[nodiscard]] bool read_array(const toml::node& node, ValueType& out) noexcept
{
    using ElementType = typename ValueType::value_type;
    const auto* arr = node.as_array();
    if (arr == nullptr)
    {
        return false;
    }
    ValueType parsed;
    for (const auto& element : *arr)
    {
        ElementType value{};
        if (!read_scalar<ElementType>(element, value))
        {
            return false;
        }
        parsed.push_back(value);
    }
    out = std::move(parsed);
    return true;
}

[[nodiscard]] bool read_vec4f(const toml::node& node, crd::math::Vec4f& out) noexcept
{
    const auto* arr = node.as_array();
    if (arr == nullptr || arr->size() != 4U)
    {
        return false;
    }
    crd::f32 values[4]{};
    for (crd::usize i = 0; i < 4U; ++i)
    {
        if (auto value = arr->get(i)->value<double>())
        {
            values[i] = static_cast<crd::f32>(*value);
        }
        else
        {
            return false;
        }
    }
    out = crd::math::Vec4f(values[0], values[1], values[2], values[3]);
    return true;
}

template <typename ValueType> void write_scalar(toml::table& table, const char* key, const ValueType& value)
{
    if constexpr (std::is_same_v<ValueType, crd::containers::String>)
    {
        table.insert_or_assign(key, std::string(value.c_str()));
    }
    else
    {
        table.insert_or_assign(key, value);
    }
}

template <typename ValueType> void write_array(toml::table& table, const char* key, const ValueType& value)
{
    toml::array arr;
    for (const auto& element : value)
    {
        if constexpr (std::is_same_v<typename ValueType::value_type, crd::containers::String>)
        {
            arr.push_back(std::string(element.c_str()));
        }
        else
        {
            arr.push_back(element);
        }
    }
    table.insert_or_assign(key, std::move(arr));
}

void write_vec4f(toml::table& table, const char* key, const crd::math::Vec4f& value)
{
    toml::array arr;
    arr.push_back(value.x);
    arr.push_back(value.y);
    arr.push_back(value.z);
    arr.push_back(value.w);
    table.insert_or_assign(key, std::move(arr));
}
} // namespace

Config::Config() noexcept : m_impl(std::make_unique<Impl>()) {}

Config::~Config() noexcept = default;

Config::Config(Config&&) noexcept = default;

Config& Config::operator=(Config&&) noexcept = default;

bool Config::load_from_string(crd::containers::StringView toml_text) noexcept
{
    const auto result = toml::parse(std::string_view(toml_text.data(), toml_text.size()), std::string_view{"config"});
    if (!result)
    {
        const auto desc = result.error().description();
        CRD_LOG_ERROR(g_log_config, "TOML parse failed: {}", std::string_view(desc.data(), desc.size()));
        m_loaded = false;
        return false;
    }
    m_impl->root = result.table();
    m_loaded = true;
    return true;
}

bool Config::load_from_file(const crd::platform::fs::Path& path) noexcept
{
    crd::containers::String text;
    if (!crd::platform::fs::read_file_text(path, text))
    {
        const crd::containers::String path_str(path.generic());
        CRD_LOG_ERROR(g_log_config, "Failed to read config file '{}'", path_str.c_str());
        m_loaded = false;
        return false;
    }
    m_source_path = path;
    return load_from_string(text);
}

bool Config::reload() noexcept
{
    if (m_source_path.empty())
    {
        CRD_LOG_WARN(g_log_config, "reload() called without a source path");
        return false;
    }
    return load_from_file(m_source_path);
}

bool Config::contains(crd::containers::StringView key) const noexcept
{
    return find_node(m_impl->root, key) != nullptr;
}

template <typename ValueType>
ValueType Config::get_impl(crd::containers::StringView key, const ValueType& fallback) const noexcept
{
    const toml::node* node = find_node(m_impl->root, key);
    if (node == nullptr)
    {
        CRD_LOG_WARN(g_log_config, "Missing config key '{}'; using fallback", key.data());
        return fallback;
    }

    ValueType out{};
    bool ok = false;
    if constexpr (std::is_same_v<ValueType, crd::i64> || std::is_same_v<ValueType, int> ||
                  std::is_same_v<ValueType, crd::f64> || std::is_same_v<ValueType, crd::f32> ||
                  std::is_same_v<ValueType, bool> || std::is_same_v<ValueType, crd::containers::String>)
    {
        ok = read_scalar<ValueType>(*node, out);
    }
    else if constexpr (std::is_same_v<ValueType, crd::containers::Array<crd::i64>> ||
                       std::is_same_v<ValueType, crd::containers::Array<crd::f32>> ||
                       std::is_same_v<ValueType, crd::containers::Array<crd::containers::String>>)
    {
        ok = read_array<ValueType>(*node, out);
    }
    else if constexpr (std::is_same_v<ValueType, crd::math::Vec4f>)
    {
        ok = read_vec4f(*node, out);
    }

    if (!ok)
    {
        CRD_LOG_WARN(g_log_config, "Config key '{}' had incompatible type; using fallback", key.data());
        return fallback;
    }
    return out;
}

template <typename ValueType> void Config::set_impl(crd::containers::StringView key, const ValueType& value)
{
    toml::table* parent = ensure_parent_table(m_impl->root, key);
    const auto leaf = leaf_key(key);

    if constexpr (std::is_same_v<ValueType, crd::i64> || std::is_same_v<ValueType, int> ||
                  std::is_same_v<ValueType, crd::f64> || std::is_same_v<ValueType, crd::f32> ||
                  std::is_same_v<ValueType, bool> || std::is_same_v<ValueType, crd::containers::String>)
    {
        write_scalar(*parent, leaf.c_str(), value);
    }
    else if constexpr (std::is_same_v<ValueType, crd::containers::Array<crd::i64>> ||
                       std::is_same_v<ValueType, crd::containers::Array<crd::f32>> ||
                       std::is_same_v<ValueType, crd::containers::Array<crd::containers::String>>)
    {
        write_array(*parent, leaf.c_str(), value);
    }
    else if constexpr (std::is_same_v<ValueType, crd::math::Vec4f>)
    {
        write_vec4f(*parent, leaf.c_str(), value);
    }
}

template int Config::get_impl<int>(crd::containers::StringView, const int&) const noexcept;
template crd::i64 Config::get_impl<crd::i64>(crd::containers::StringView, const crd::i64&) const noexcept;
template crd::f32 Config::get_impl<crd::f32>(crd::containers::StringView, const crd::f32&) const noexcept;
template crd::f64 Config::get_impl<crd::f64>(crd::containers::StringView, const crd::f64&) const noexcept;
template bool Config::get_impl<bool>(crd::containers::StringView, const bool&) const noexcept;
template crd::containers::String
Config::get_impl<crd::containers::String>(crd::containers::StringView, const crd::containers::String&) const noexcept;
template crd::containers::Array<crd::i64>
Config::get_impl<crd::containers::Array<crd::i64>>(crd::containers::StringView,
                                                   const crd::containers::Array<crd::i64>&) const noexcept;
template crd::containers::Array<crd::f32>
Config::get_impl<crd::containers::Array<crd::f32>>(crd::containers::StringView,
                                                   const crd::containers::Array<crd::f32>&) const noexcept;
template crd::containers::Array<crd::containers::String>
Config::get_impl<crd::containers::Array<crd::containers::String>>(
    crd::containers::StringView, const crd::containers::Array<crd::containers::String>&) const noexcept;
template crd::math::Vec4f Config::get_impl<crd::math::Vec4f>(crd::containers::StringView,
                                                             const crd::math::Vec4f&) const noexcept;

template void Config::set_impl<int>(crd::containers::StringView, const int&);
template void Config::set_impl<crd::i64>(crd::containers::StringView, const crd::i64&);
template void Config::set_impl<crd::f32>(crd::containers::StringView, const crd::f32&);
template void Config::set_impl<crd::f64>(crd::containers::StringView, const crd::f64&);
template void Config::set_impl<bool>(crd::containers::StringView, const bool&);
template void Config::set_impl<crd::containers::String>(crd::containers::StringView, const crd::containers::String&);
template void Config::set_impl<crd::containers::Array<crd::i64>>(crd::containers::StringView,
                                                                 const crd::containers::Array<crd::i64>&);
template void Config::set_impl<crd::containers::Array<crd::f32>>(crd::containers::StringView,
                                                                 const crd::containers::Array<crd::f32>&);
template void Config::set_impl<crd::containers::Array<crd::containers::String>>(
    crd::containers::StringView, const crd::containers::Array<crd::containers::String>&);
template void Config::set_impl<crd::math::Vec4f>(crd::containers::StringView, const crd::math::Vec4f&);
} // namespace crd::config
