// crd-toml -- native TOML reader/writer implementation. See crd/toml/toml.hpp.

#include <crd/toml/toml.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace crd::toml
{
namespace
{
node make_string(cont::StringView s)
{
    node n;
    n.set_string(s);
    return n;
}
node make_int(crd::i64 v)
{
    node n;
    n.set_integer(v);
    return n;
}
node make_float(crd::f64 v)
{
    node n;
    n.set_float(v);
    return n;
}
node make_bool(bool v)
{
    node n;
    n.set_boolean(v);
    return n;
}

void append_escaped(cont::String& out, std::string_view s)
{
    out.push_back('"');
    for (char c : s)
    {
        switch (c)
        {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
}

void append_int(cont::String& out, crd::i64 v)
{
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    if (n > 0)
    {
        out.append(buf, static_cast<crd::usize>(n));
    }
}

void append_float(cont::String& out, crd::f64 v)
{
    char buf[64];
    // %.17g round-trips a double; ensure a decimal point so re-parsing stays Float.
    int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
    if (n <= 0)
    {
        out.append("0.0");
        return;
    }
    std::string_view sv{buf, static_cast<std::size_t>(n)};
    out.append(buf, static_cast<crd::usize>(n));
    if (sv.find('.') == std::string_view::npos && sv.find('e') == std::string_view::npos &&
        sv.find('E') == std::string_view::npos && sv.find("inf") == std::string_view::npos &&
        sv.find("nan") == std::string_view::npos)
    {
        out.append(".0");
    }
}
} // namespace

node* node::get(cont::StringView key) noexcept
{
    if (m_kind != Kind::Table)
    {
        return nullptr;
    }
    const std::string_view want{key};
    for (crd::usize i = 0; i < m_keys.size(); ++i)
    {
        if (std::string_view{m_keys[i]} == want)
        {
            return &m_children[i];
        }
    }
    return nullptr;
}

const node* node::get(cont::StringView key) const noexcept
{
    return const_cast<node*>(this)->get(key);
}

node* node::get(crd::usize index) noexcept
{
    if (index >= m_children.size())
    {
        return nullptr;
    }
    return &m_children[index];
}

const node* node::get(crd::usize index) const noexcept
{
    if (index >= m_children.size())
    {
        return nullptr;
    }
    return &m_children[index];
}

crd::usize node::size() const noexcept
{
    return m_children.size();
}

cont::StringView node::key_at(crd::usize index) const noexcept
{
    if (m_kind != Kind::Table || index >= m_keys.size())
    {
        return cont::StringView{};
    }
    return m_keys[index];
}

void node::insert_or_assign(cont::StringView key, node value)
{
    if (m_kind != Kind::Table)
    {
        m_kind = Kind::Table;
    }
    const std::string_view want{key};
    for (crd::usize i = 0; i < m_keys.size(); ++i)
    {
        if (std::string_view{m_keys[i]} == want)
        {
            m_children[i] = static_cast<node&&>(value);
            return;
        }
    }
    m_keys.push_back(cont::String{key});
    m_children.push_back(static_cast<node&&>(value));
}

void node::insert_or_assign(cont::StringView key, const char* value)
{
    insert_or_assign(key, make_string(value != nullptr ? cont::StringView{value} : cont::StringView{}));
}
void node::insert_or_assign(cont::StringView key, cont::StringView value)
{
    insert_or_assign(key, make_string(value));
}
void node::insert_or_assign(cont::StringView key, const cont::String& value)
{
    insert_or_assign(key, make_string(value));
}
void node::insert_or_assign(cont::StringView key, crd::i64 value)
{
    insert_or_assign(key, make_int(value));
}
void node::insert_or_assign(cont::StringView key, int value)
{
    insert_or_assign(key, make_int(static_cast<crd::i64>(value)));
}
void node::insert_or_assign(cont::StringView key, crd::f64 value)
{
    insert_or_assign(key, make_float(value));
}
void node::insert_or_assign(cont::StringView key, crd::f32 value)
{
    insert_or_assign(key, make_float(static_cast<crd::f64>(value)));
}
void node::insert_or_assign(cont::StringView key, bool value)
{
    insert_or_assign(key, make_bool(value));
}

void node::push_back(node value)
{
    if (m_kind != Kind::Array)
    {
        m_kind = Kind::Array;
    }
    m_children.push_back(static_cast<node&&>(value));
}
void node::push_back(const char* value)
{
    push_back(make_string(value != nullptr ? cont::StringView{value} : cont::StringView{}));
}
void node::push_back(cont::StringView value)
{
    push_back(make_string(value));
}
void node::push_back(const cont::String& value)
{
    push_back(make_string(value));
}
void node::push_back(crd::i64 value)
{
    push_back(make_int(value));
}
void node::push_back(int value)
{
    push_back(make_int(static_cast<crd::i64>(value)));
}
void node::push_back(crd::f64 value)
{
    push_back(make_float(value));
}
void node::push_back(crd::f32 value)
{
    push_back(make_float(static_cast<crd::f64>(value)));
}
void node::push_back(bool value)
{
    push_back(make_bool(value));
}

// ---- serialization --------------------------------------------------------

namespace
{
bool is_scalar_kind(Kind k) noexcept
{
    return k == Kind::Boolean || k == Kind::Integer || k == Kind::Float || k == Kind::String;
}

void append_inline(cont::String& out, const node& n); // fwd

void append_inline_array(cont::String& out, const node& arr)
{
    out.push_back('[');
    for (crd::usize i = 0; i < arr.size(); ++i)
    {
        if (i != 0)
        {
            out.append(", ");
        }
        append_inline(out, *arr.get(i));
    }
    out.push_back(']');
}

void append_inline(cont::String& out, const node& n)
{
    switch (n.kind())
    {
    case Kind::Boolean: out.append(n.value<bool>().value_or(false) ? "true" : "false"); break;
    case Kind::Integer: append_int(out, n.value<crd::i64>().value_or(0)); break;
    case Kind::Float: append_float(out, n.value<double>().value_or(0.0)); break;
    case Kind::String:
    {
        const auto sv = n.value<std::string_view>();
        append_escaped(out, sv ? *sv : std::string_view{});
        break;
    }
    case Kind::Array: append_inline_array(out, n); break;
    case Kind::Table:
    {
        // Inline table.
        out.append("{ ");
        for (crd::usize i = 0; i < n.size(); ++i)
        {
            if (i != 0)
            {
                out.append(", ");
            }
            out.append(n.key_at(i));
            out.append(" = ");
            append_inline(out, *n.get(i));
        }
        out.append(" }");
        break;
    }
    case Kind::None: out.append("\"\""); break;
    }
}

bool array_is_all_tables(const node& arr)
{
    if (arr.size() == 0)
    {
        return false;
    }
    for (crd::usize i = 0; i < arr.size(); ++i)
    {
        if (arr.get(i)->kind() != Kind::Table)
        {
            return false;
        }
    }
    return true;
}
} // namespace

void node::serialize(cont::String& out, crd::u32 depth, cont::StringView path) const
{
    (void)depth;
    if (m_kind != Kind::Table)
    {
        return;
    }
    // Inline scalars and scalar/mixed arrays first.
    for (crd::usize i = 0; i < m_keys.size(); ++i)
    {
        const node& child = m_children[i];
        if (is_scalar_kind(child.kind()) || (child.kind() == Kind::Array && !array_is_all_tables(child)))
        {
            out.append(m_keys[i]);
            out.append(" = ");
            append_inline(out, child);
            out.push_back('\n');
        }
    }
    // Then sub-tables and arrays-of-tables as headers.
    for (crd::usize i = 0; i < m_keys.size(); ++i)
    {
        const node&  child = m_children[i];
        cont::String child_path{path};
        if (!child_path.empty())
        {
            child_path.push_back('.');
        }
        child_path.append(m_keys[i]);

        if (child.kind() == Kind::Table)
        {
            out.push_back('\n');
            out.push_back('[');
            out.append(child_path);
            out.append("]\n");
            child.serialize(out, depth + 1, child_path);
        }
        else if (child.kind() == Kind::Array && array_is_all_tables(child))
        {
            for (crd::usize e = 0; e < child.size(); ++e)
            {
                out.push_back('\n');
                out.append("[[");
                out.append(child_path);
                out.append("]]\n");
                child.get(e)->serialize(out, depth + 1, child_path);
            }
        }
    }
}

cont::String node::to_toml() const
{
    cont::String out;
    serialize(out, 0, cont::StringView{});
    return out;
}

// ---- parser ---------------------------------------------------------------

namespace
{
class Parser
{
public:
    Parser(std::string_view text, std::string_view name) : m_p(text.data()), m_end(text.data() + text.size()), m_name(name)
    {
    }

    parse_result run()
    {
        node root;
        root.set_kind(Kind::Table);
        node* current = &root;

        for (;;)
        {
            skip_ws_comments_newlines();
            if (at_end())
            {
                break;
            }
            if (peek() == '[')
            {
                if (!parse_header(root, current))
                {
                    return fail();
                }
            }
            else
            {
                if (!parse_keyval(*current))
                {
                    return fail();
                }
            }
            skip_inline_ws();
            skip_comment();
            if (!at_end() && peek() != '\n' && peek() != '\r')
            {
                set_error("expected end of line after entry");
                return fail();
            }
        }
        if (m_failed)
        {
            return fail();
        }
        return parse_result::success(static_cast<node&&>(root));
    }

private:
    const char*      m_p;
    const char*      m_end;
    std::string_view m_name;
    crd::u32         m_line = 1;
    crd::u32         m_col = 1;
    bool             m_failed = false;
    cont::String     m_error_msg;
    crd::u32         m_error_line = 0;
    crd::u32         m_error_col = 0;

    bool at_end() const noexcept { return m_p >= m_end; }
    char peek() const noexcept { return at_end() ? '\0' : *m_p; }
    char peek2() const noexcept { return (m_p + 1 >= m_end) ? '\0' : *(m_p + 1); }

    char advance() noexcept
    {
        const char c = *m_p++;
        if (c == '\n')
        {
            ++m_line;
            m_col = 1;
        }
        else
        {
            ++m_col;
        }
        return c;
    }

    void set_error(const char* msg)
    {
        if (!m_failed)
        {
            m_failed = true;
            m_error_msg = cont::String{msg};
            m_error_line = m_line;
            m_error_col = m_col;
        }
    }

    parse_result fail()
    {
        cont::String full;
        if (!m_name.empty())
        {
            full.append(m_name);
            full.append(": ");
        }
        full.append(m_error_msg.empty() ? cont::StringView{"parse error"} : cont::StringView{m_error_msg});
        return parse_result::failure(parse_error{static_cast<cont::String&&>(full), m_error_line, m_error_col});
    }

    static bool is_inline_ws(char c) noexcept { return c == ' ' || c == '\t'; }
    static bool is_bare_key_char(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    void skip_inline_ws() noexcept
    {
        while (!at_end() && is_inline_ws(peek()))
        {
            advance();
        }
    }

    void skip_comment() noexcept
    {
        if (!at_end() && peek() == '#')
        {
            while (!at_end() && peek() != '\n')
            {
                advance();
            }
        }
    }

    void skip_ws_comments_newlines() noexcept
    {
        for (;;)
        {
            while (!at_end() && (is_inline_ws(peek()) || peek() == '\n' || peek() == '\r'))
            {
                advance();
            }
            if (!at_end() && peek() == '#')
            {
                skip_comment();
                continue;
            }
            break;
        }
    }

    // Reads a bare or quoted key into `out`. Returns false on error.
    bool parse_key(cont::String& out)
    {
        skip_inline_ws();
        if (at_end())
        {
            set_error("expected key");
            return false;
        }
        if (peek() == '"' || peek() == '\'')
        {
            return parse_quoted_string(out);
        }
        if (!is_bare_key_char(peek()))
        {
            set_error("invalid key");
            return false;
        }
        while (!at_end() && is_bare_key_char(peek()))
        {
            out.push_back(advance());
        }
        return true;
    }

    bool parse_quoted_string(cont::String& out)
    {
        const char quote = advance(); // consume opening quote
        if (quote == '\'')
        {
            // literal string: no escapes
            while (!at_end() && peek() != '\'')
            {
                if (peek() == '\n')
                {
                    set_error("unterminated string");
                    return false;
                }
                out.push_back(advance());
            }
            if (at_end())
            {
                set_error("unterminated string");
                return false;
            }
            advance(); // closing '
            return true;
        }
        // basic string with escapes
        while (!at_end() && peek() != '"')
        {
            char c = peek();
            if (c == '\n')
            {
                set_error("unterminated string");
                return false;
            }
            if (c == '\\')
            {
                advance();
                if (at_end())
                {
                    set_error("unterminated escape");
                    return false;
                }
                const char e = advance();
                switch (e)
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '0': out.push_back('\0'); break;
                case 'u':
                case 'U':
                {
                    const int digits = (e == 'u') ? 4 : 8;
                    if (!append_unicode(out, digits))
                    {
                        return false;
                    }
                    break;
                }
                default:
                    set_error("invalid escape sequence");
                    return false;
                }
            }
            else
            {
                out.push_back(advance());
            }
        }
        if (at_end())
        {
            set_error("unterminated string");
            return false;
        }
        advance(); // closing "
        return true;
    }

    bool append_unicode(cont::String& out, int digits)
    {
        crd::u32 cp = 0;
        for (int i = 0; i < digits; ++i)
        {
            if (at_end())
            {
                set_error("truncated unicode escape");
                return false;
            }
            const char h = advance();
            cp <<= 4U;
            if (h >= '0' && h <= '9')
            {
                cp |= static_cast<crd::u32>(h - '0');
            }
            else if (h >= 'a' && h <= 'f')
            {
                cp |= static_cast<crd::u32>(h - 'a' + 10);
            }
            else if (h >= 'A' && h <= 'F')
            {
                cp |= static_cast<crd::u32>(h - 'A' + 10);
            }
            else
            {
                set_error("invalid unicode escape");
                return false;
            }
        }
        // encode cp as UTF-8
        if (cp < 0x80U)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800U)
        {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else if (cp < 0x10000U)
        {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        return true;
    }

    // `[a.b.c]` selects/creates a nested table; `[[a.b]]` appends a table to an
    // array of tables. Sets `current` to the target table for following keys.
    bool parse_header(node& root, node*& current)
    {
        const crd::u32 hline = m_line;
        const crd::u32 hcol = m_col;
        advance(); // '['
        const bool array_of_tables = (!at_end() && peek() == '[');
        if (array_of_tables)
        {
            advance(); // second '['
        }
        cont::Array<cont::String> path;
        for (;;)
        {
            skip_inline_ws();
            cont::String seg;
            if (!parse_key(seg))
            {
                return false;
            }
            path.push_back(static_cast<cont::String&&>(seg));
            skip_inline_ws();
            if (!at_end() && peek() == '.')
            {
                advance();
                continue;
            }
            break;
        }
        if (at_end() || peek() != ']')
        {
            set_error("expected ']' in table header");
            return false;
        }
        advance(); // ']'
        if (array_of_tables)
        {
            if (at_end() || peek() != ']')
            {
                set_error("expected ']]' in array-of-tables header");
                return false;
            }
            advance();
        }
        if (path.empty())
        {
            set_error("empty table header");
            return false;
        }

        node* cur = &root;
        for (crd::usize i = 0; i + 1 < path.size(); ++i)
        {
            cur = descend_table(*cur, path[i]);
            if (cur == nullptr)
            {
                set_error("table path conflicts with a non-table value");
                return false;
            }
        }
        const cont::String& leaf = path[path.size() - 1];
        if (array_of_tables)
        {
            node* arr = cur->get(cont::StringView{leaf});
            if (arr == nullptr)
            {
                node fresh;
                fresh.set_kind(Kind::Array);
                cur->insert_or_assign(cont::StringView{leaf}, static_cast<node&&>(fresh));
                arr = cur->get(cont::StringView{leaf});
            }
            if (arr == nullptr || arr->kind() != Kind::Array)
            {
                set_error("array-of-tables path conflicts with a non-array value");
                return false;
            }
            node elem;
            elem.set_kind(Kind::Table);
            arr->push_back(static_cast<node&&>(elem));
            current = arr->get(arr->size() - 1);
        }
        else
        {
            current = descend_table(*cur, leaf);
            if (current == nullptr)
            {
                set_error("table path conflicts with a non-table value");
                return false;
            }
        }
        if (current != nullptr)
        {
            current->set_source(hline, hcol);
        }
        return true;
    }

    // Get-or-create a child table `key` inside `parent`.
    static node* descend_table(node& parent, const cont::String& key)
    {
        node* existing = parent.get(cont::StringView{key});
        if (existing != nullptr)
        {
            if (existing->kind() == Kind::Table)
            {
                return existing;
            }
            // A dotted path segment through an array-of-tables selects its most
            // recent element, so `[pass.params]` after `[[pass]]` lands inside the
            // last pass (standard TOML).
            if (existing->kind() == Kind::Array && existing->size() > 0)
            {
                node* last = existing->get(existing->size() - 1);
                return (last != nullptr && last->kind() == Kind::Table) ? last : nullptr;
            }
            return nullptr;
        }
        node fresh;
        fresh.set_kind(Kind::Table);
        parent.insert_or_assign(cont::StringView{key}, static_cast<node&&>(fresh));
        return parent.get(cont::StringView{key});
    }

    bool parse_keyval(node& tbl)
    {
        cont::String key;
        if (!parse_key(key))
        {
            return false;
        }
        skip_inline_ws();
        if (at_end() || peek() != '=')
        {
            set_error("expected '=' after key");
            return false;
        }
        advance(); // '='
        skip_inline_ws();
        const crd::u32 vline = m_line;
        const crd::u32 vcol = m_col;
        node value;
        if (!parse_value(value))
        {
            return false;
        }
        value.set_source(vline, vcol);
        tbl.insert_or_assign(cont::StringView{key}, static_cast<node&&>(value));
        return true;
    }

    bool parse_value(node& out)
    {
        skip_inline_ws();
        if (at_end())
        {
            set_error("expected value");
            return false;
        }
        const char c = peek();
        if (c == '"' || c == '\'')
        {
            cont::String s;
            if (!parse_quoted_string(s))
            {
                return false;
            }
            out.set_string(s);
            return true;
        }
        if (c == '[')
        {
            return parse_array(out);
        }
        if (c == '{')
        {
            return parse_inline_table(out);
        }
        if (c == 't' || c == 'f')
        {
            return parse_bool(out);
        }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9'))
        {
            return parse_number(out);
        }
        set_error("unexpected character in value");
        return false;
    }

    bool parse_bool(node& out)
    {
        if (match("true"))
        {
            out.set_boolean(true);
            return true;
        }
        if (match("false"))
        {
            out.set_boolean(false);
            return true;
        }
        set_error("invalid boolean");
        return false;
    }

    bool match(const char* word) noexcept
    {
        const std::size_t len = std::strlen(word);
        if (static_cast<std::size_t>(m_end - m_p) < len)
        {
            return false;
        }
        if (std::strncmp(m_p, word, len) != 0)
        {
            return false;
        }
        for (std::size_t i = 0; i < len; ++i)
        {
            advance();
        }
        return true;
    }

    bool parse_number(node& out)
    {
        const char* start = m_p;
        bool        is_float = false;
        if (!at_end() && (peek() == '-' || peek() == '+'))
        {
            advance();
        }
        while (!at_end())
        {
            const char c = peek();
            if ((c >= '0' && c <= '9') || c == '_')
            {
                advance();
            }
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
            {
                is_float = is_float || (c == '.' || c == 'e' || c == 'E');
                advance();
            }
            else
            {
                break;
            }
        }
        // Copy the token without underscores into a small buffer.
        char        buf[64];
        std::size_t n = 0;
        for (const char* q = start; q < m_p && n + 1 < sizeof(buf); ++q)
        {
            if (*q != '_')
            {
                buf[n++] = *q;
            }
        }
        buf[n] = '\0';
        if (n == 0)
        {
            set_error("invalid number");
            return false;
        }
        char* endp = nullptr;
        if (is_float)
        {
            const double v = std::strtod(buf, &endp);
            if (endp != buf + n)
            {
                set_error("invalid float");
                return false;
            }
            out.set_float(v);
        }
        else
        {
            const long long v = std::strtoll(buf, &endp, 10);
            if (endp != buf + n)
            {
                set_error("invalid integer");
                return false;
            }
            out.set_integer(static_cast<crd::i64>(v));
        }
        return true;
    }

    bool parse_array(node& out)
    {
        advance(); // '['
        out.set_kind(Kind::Array);
        for (;;)
        {
            skip_ws_comments_newlines();
            if (at_end())
            {
                set_error("unterminated array");
                return false;
            }
            if (peek() == ']')
            {
                advance();
                return true;
            }
            node elem;
            if (!parse_value(elem))
            {
                return false;
            }
            out.push_back(static_cast<node&&>(elem));
            skip_ws_comments_newlines();
            if (!at_end() && peek() == ',')
            {
                advance();
                continue;
            }
            skip_ws_comments_newlines();
            if (!at_end() && peek() == ']')
            {
                advance();
                return true;
            }
            set_error("expected ',' or ']' in array");
            return false;
        }
    }

    bool parse_inline_table(node& out)
    {
        advance(); // '{'
        out.set_kind(Kind::Table);
        skip_inline_ws();
        if (!at_end() && peek() == '}')
        {
            advance();
            return true;
        }
        for (;;)
        {
            skip_inline_ws();
            cont::String key;
            if (!parse_key(key))
            {
                return false;
            }
            skip_inline_ws();
            if (at_end() || peek() != '=')
            {
                set_error("expected '=' in inline table");
                return false;
            }
            advance();
            skip_inline_ws();
            node value;
            if (!parse_value(value))
            {
                return false;
            }
            out.insert_or_assign(cont::StringView{key}, static_cast<node&&>(value));
            skip_inline_ws();
            if (!at_end() && peek() == ',')
            {
                advance();
                continue;
            }
            if (!at_end() && peek() == '}')
            {
                advance();
                return true;
            }
            set_error("expected ',' or '}' in inline table");
            return false;
        }
    }
};
} // namespace

parse_result parse(std::string_view text, std::string_view name)
{
    Parser parser(text, name);
    return parser.run();
}

} // namespace crd::toml
