#pragma once

// crd-toml -- a native TOML reader/writer built on the engine containers.
//
// Replaces the third-party tomlplusplus. The public surface mirrors the subset of
// toml++ the engine used (node / table / array / parse / parse_result), so call
// sites move over with an include swap and `toml::` -> `crd::toml::`.
//
// Supported: comments (# to end of line, inline), bare and quoted keys, `[table]`
// and dotted `[table.sub]` headers, `[[array-of-tables]]`, inline arrays `[...]`
// (multi-line), inline tables `{...}`, and the scalar types the assets use:
// basic strings, integers, floats and booleans. Not supported (unused by the
// engine's assets): multi-line/literal strings, datetimes, hex/oct/bin integers.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

#include <optional>
#include <string_view>

namespace crd::toml
{
namespace cont = crd::containers;

enum class Kind : crd::u8
{
    None,
    Boolean,
    Integer,
    Float,
    String,
    Array,
    Table,
};

class node_view;
class table_range;

// Source location of a value/table in the document (toml++-compatible shape).
struct source_position
{
    crd::u32 line = 0;
    crd::u32 column = 0;
};
struct source_region
{
    source_position begin;
    source_position end;
};

// One TOML value. A single universal type (like toml++'s node) that can be a
// scalar, an array of nodes, or a table (ordered key/value pairs). `table` and
// `array` below are thin node subtypes whose only job is to default-construct with
// the right kind.
class node
{
public:
    node() = default;

    Kind kind() const noexcept { return m_kind; }

    bool is_table() const noexcept { return m_kind == Kind::Table; }
    bool is_array() const noexcept { return m_kind == Kind::Array; }
    bool is_string() const noexcept { return m_kind == Kind::String; }
    bool is_integer() const noexcept { return m_kind == Kind::Integer; }
    bool is_floating_point() const noexcept { return m_kind == Kind::Float; }
    bool is_boolean() const noexcept { return m_kind == Kind::Boolean; }
    bool is_number() const noexcept { return m_kind == Kind::Integer || m_kind == Kind::Float; }
    bool is_array_of_tables() const noexcept
    {
        if (m_kind != Kind::Array || m_children.size() == 0)
        {
            return false;
        }
        for (crd::usize i = 0; i < m_children.size(); ++i)
        {
            if (m_children[i].m_kind != Kind::Table)
            {
                return false;
            }
        }
        return true;
    }

    // Source location (1-based) recorded by the parser; zero for built nodes.
    source_region source() const noexcept { return source_region{{m_line, m_col}, {m_line, m_col}}; }
    void          set_source(crd::u32 line, crd::u32 col) noexcept
    {
        m_line = line;
        m_col = col;
    }

    // Typed extraction (toml++-compatible). Returns nullopt when the stored kind
    // does not match the requested type. For std::string_view the view points into
    // this node's owned storage and is valid while the node lives.
    template <typename T> std::optional<T> value() const noexcept;
    template <typename T> T value_or(T fallback) const noexcept
    {
        const auto v = value<T>();
        return v.has_value() ? *v : fallback;
    }

    // Sub-value access. as_table()/as_array() return this node when it is of that
    // kind, else nullptr (toml++ returns table*/array*; here node* suffices for
    // every call site). as_string() likewise returns this when it is a string.
    node*       as_table() noexcept { return m_kind == Kind::Table ? this : nullptr; }
    const node* as_table() const noexcept { return m_kind == Kind::Table ? this : nullptr; }
    node*       as_array() noexcept { return m_kind == Kind::Array ? this : nullptr; }
    const node* as_array() const noexcept { return m_kind == Kind::Array ? this : nullptr; }
    const node* as_string() const noexcept { return m_kind == Kind::String ? this : nullptr; }

    // Table lookup by key (nullptr when absent or when this is not a table).
    node*       get(cont::StringView key) noexcept;
    const node* get(cont::StringView key) const noexcept;

    // Array element by index (nullptr when out of range or not an array).
    node*       get(crd::usize index) noexcept;
    const node* get(crd::usize index) const noexcept;

    // toml++-style keyed access: `node["key"].value<T>()`, chainable. A missing
    // key yields an empty view (falsy), never a crash.
    node_view operator[](cont::StringView key) const noexcept;

    // Ordered key/value iteration of a table: `for (auto&& [k, v] : t.items())`.
    // (Array elements iterate directly with begin()/end().)
    table_range items() const noexcept;

    // Element / entry count (array length, or table key count).
    crd::usize size() const noexcept;

    // Table insertion (last write wins). Promotes a None node to a table.
    void insert_or_assign(cont::StringView key, node value);
    void insert_or_assign(cont::StringView key, const char* value);
    void insert_or_assign(cont::StringView key, cont::StringView value);
    void insert_or_assign(cont::StringView key, const cont::String& value);
    void insert_or_assign(cont::StringView key, crd::i64 value);
    void insert_or_assign(cont::StringView key, int value);
    void insert_or_assign(cont::StringView key, crd::f64 value);
    void insert_or_assign(cont::StringView key, crd::f32 value);
    void insert_or_assign(cont::StringView key, bool value);

    // Array append. Promotes a None node to an array.
    void push_back(node value);
    void push_back(const char* value);
    void push_back(cont::StringView value);
    void push_back(const cont::String& value);
    void push_back(crd::i64 value);
    void push_back(int value);
    void push_back(crd::f64 value);
    void push_back(crd::f32 value);
    void push_back(bool value);

    // Array iteration: `for (const auto& element : *node.as_array())`.
    node*       begin() noexcept { return m_children.begin(); }
    node*       end() noexcept { return m_children.end(); }
    const node* begin() const noexcept { return m_children.begin(); }
    const node* end() const noexcept { return m_children.end(); }

    // Table key at ordered position (parallel to the array of children); empty
    // view for a non-table or out-of-range index. Enables ordered iteration.
    cont::StringView key_at(crd::usize index) const noexcept;

    // Serialize this node (typically a table) to canonical TOML text.
    cont::String to_toml() const;

    // Low-level setters used by the parser and by `table`/`array`.
    void set_kind(Kind k) noexcept { m_kind = k; }
    void set_boolean(bool v) noexcept
    {
        m_kind = Kind::Boolean;
        m_b = v;
    }
    void set_integer(crd::i64 v) noexcept
    {
        m_kind = Kind::Integer;
        m_i = v;
    }
    void set_float(crd::f64 v) noexcept
    {
        m_kind = Kind::Float;
        m_f = v;
    }
    void set_string(cont::StringView v)
    {
        m_kind = Kind::String;
        m_s = cont::String{v};
    }

private:
    void serialize(cont::String& out, crd::u32 depth, cont::StringView path) const;

    Kind                      m_kind = Kind::None;
    bool                      m_b = false;
    crd::i64                  m_i = 0;
    crd::f64                  m_f = 0.0;
    cont::String              m_s;
    cont::Array<cont::String> m_keys;     // table keys (parallel to m_children when Table)
    cont::Array<node>         m_children; // table values, or array elements
    crd::u32                  m_line = 0; // 1-based source line (parser-stamped)
    crd::u32                  m_col = 0;  // 1-based source column (parser-stamped)
};

// Typed extraction specializations (strict, like toml++: no int<->float coercion).
template <> inline std::optional<std::string_view> node::value<std::string_view>() const noexcept
{
    if (m_kind != Kind::String)
    {
        return std::nullopt;
    }
    return std::string_view{m_s};
}
template <> inline std::optional<crd::i64> node::value<crd::i64>() const noexcept
{
    if (m_kind == Kind::Integer)
    {
        return m_i;
    }
    // toml++ extracts an integer from a float that has no fractional part, so a
    // writer that prints 8 as "8.000000" (fixed precision) still round-trips.
    if (m_kind == Kind::Float)
    {
        const crd::i64 as_int = static_cast<crd::i64>(m_f);
        if (static_cast<crd::f64>(as_int) == m_f)
        {
            return as_int;
        }
    }
    return std::nullopt;
}
template <> inline std::optional<double> node::value<double>() const noexcept
{
    if (m_kind == Kind::Float)
    {
        return m_f;
    }
    // toml++ widens an integer to double on request (e.g. a `scale = 1` read as
    // a float), so an emitter that prints 1.0 as "1" still round-trips.
    if (m_kind == Kind::Integer)
    {
        return static_cast<double>(m_i);
    }
    return std::nullopt;
}
template <> inline std::optional<bool> node::value<bool>() const noexcept
{
    if (m_kind != Kind::Boolean)
    {
        return std::nullopt;
    }
    return m_b;
}

// A non-owning, nullable view of a node (toml++'s node_view). Returned by keyed
// access; every accessor is null-safe so `t["a"]["b"].value<int>()` never crashes.
class node_view
{
public:
    node_view() = default;
    explicit node_view(const node* n) noexcept : m_node(n) {}

    explicit operator bool() const noexcept { return m_node != nullptr && m_node->kind() != Kind::None; }
    bool     has_value() const noexcept { return static_cast<bool>(*this); }

    template <typename T> std::optional<T> value() const noexcept
    {
        return m_node != nullptr ? m_node->value<T>() : std::nullopt;
    }
    template <typename T> T value_or(T fallback) const noexcept
    {
        const auto v = value<T>();
        return v.has_value() ? *v : fallback;
    }

    const node* as_table() const noexcept { return m_node != nullptr ? m_node->as_table() : nullptr; }
    const node* as_array() const noexcept { return m_node != nullptr ? m_node->as_array() : nullptr; }
    const node* as_string() const noexcept { return m_node != nullptr ? m_node->as_string() : nullptr; }

    bool is_table() const noexcept { return m_node != nullptr && m_node->is_table(); }
    bool is_array() const noexcept { return m_node != nullptr && m_node->is_array(); }
    bool is_string() const noexcept { return m_node != nullptr && m_node->is_string(); }
    bool is_integer() const noexcept { return m_node != nullptr && m_node->is_integer(); }
    bool is_boolean() const noexcept { return m_node != nullptr && m_node->is_boolean(); }
    bool is_number() const noexcept { return m_node != nullptr && m_node->is_number(); }

    node_view operator[](cont::StringView key) const noexcept
    {
        return node_view{m_node != nullptr ? m_node->get(key) : nullptr};
    }

    const node* node_ptr() const noexcept { return m_node; }

private:
    const node* m_node = nullptr;
};

inline node_view node::operator[](cont::StringView key) const noexcept
{
    return node_view{get(key)};
}

// One table entry yielded by table_range: `first` is the key, `second` the value.
struct key_value
{
    cont::StringView first;
    const node&      second;
};

// Range over a table's ordered entries. Only meaningful for a table node.
class table_range
{
public:
    class const_iterator
    {
    public:
        const_iterator(const node* tbl, crd::usize index) noexcept : m_tbl(tbl), m_index(index) {}
        key_value       operator*() const noexcept { return key_value{m_tbl->key_at(m_index), *m_tbl->get(m_index)}; }
        const_iterator& operator++() noexcept
        {
            ++m_index;
            return *this;
        }
        bool operator!=(const const_iterator& o) const noexcept { return m_index != o.m_index; }

    private:
        const node* m_tbl;
        crd::usize  m_index;
    };

    explicit table_range(const node* tbl) noexcept : m_tbl(tbl) {}
    const_iterator begin() const noexcept { return const_iterator{m_tbl, 0}; }
    const_iterator end() const noexcept { return const_iterator{m_tbl, m_tbl != nullptr ? m_tbl->size() : 0}; }

private:
    const node* m_tbl;
};

inline table_range node::items() const noexcept
{
    return table_range{this};
}

// toml++-compatible spellings: default-construct with the right kind.
struct table : node
{
    table() noexcept { set_kind(Kind::Table); }
};

struct array : node
{
    array() noexcept { set_kind(Kind::Array); }
};

// A parse failure with a human-readable description and 1-based line/column.
class parse_error
{
public:
    parse_error() = default;
    parse_error(cont::String message, crd::u32 line, crd::u32 column)
        : m_message(static_cast<cont::String&&>(message)), m_line(line), m_column(column)
    {
    }

    cont::StringView description() const noexcept { return m_message; }
    crd::u32         line() const noexcept { return m_line; }
    crd::u32         column() const noexcept { return m_column; }
    source_region    source() const noexcept { return source_region{{m_line, m_column}, {m_line, m_column}}; }

private:
    cont::String m_message;
    crd::u32     m_line = 0;
    crd::u32     m_column = 0;
};

// Result of parse(): truthy on success. On success table() is the root table.
class parse_result
{
public:
    explicit operator bool() const noexcept { return m_ok; }
    bool                succeeded() const noexcept { return m_ok; }
    const parse_error&  error() const noexcept { return m_error; }
    node&               table() noexcept { return m_root; }
    const node&         table() const noexcept { return m_root; }
    node_view           operator[](cont::StringView key) const noexcept { return node_view{m_root.get(key)}; }

    static parse_result success(node root)
    {
        parse_result r;
        r.m_ok = true;
        r.m_root = static_cast<node&&>(root);
        return r;
    }
    static parse_result failure(parse_error err)
    {
        parse_result r;
        r.m_ok = false;
        r.m_error = static_cast<parse_error&&>(err);
        return r;
    }

private:
    bool        m_ok = false;
    node        m_root;
    parse_error m_error;
};

// Parse a TOML document. `name` is used only in error messages.
parse_result parse(std::string_view text, std::string_view name = {});

} // namespace crd::toml
