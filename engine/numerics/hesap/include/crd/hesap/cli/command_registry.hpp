#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/cli/command_schema.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::cli
{
// -----------------------------------------------------------------------
// CommandRegistry — process-wide registry for hesap CLI commands.
//
// Per advisor 2026-05-19: Meyers singleton. The CRD_HESAP_CLI_REGISTER_MODULE
// macro expands to a static-storage-duration registrar object whose ctor
// calls `global_registry().register_command(...)`. Lazy-on-first-access
// initialisation defeats static-init-order fiasco (ADR-0081 §Consequences).
//
// Public surface (v0a):
//   - register_command(CommandSchema, ImplFn) — called from static-init
//     CRD_HESAP_CLI_REGISTER_MODULE blocks.
//   - find(name) -> const CommandRecord* — lookup by dotted name.
//   - all() -> view of every registered record (for meta.list-commands
//     and meta.export-mcp-tools).
//
// v0a deliberately does NOT ship: command-line parser, REPL, JSON-RPC
// server, MCP server. Those land in Phase 4.0 crd-cli + crd-rpc. v0a
// ships the registry + schemas only — protocol plumbing.
// -----------------------------------------------------------------------

// Args passed to a command implementation. v0a shipped an opaque placeholder;
// v0b adds the typed `values` map per ADR-0081 §3 so impls can read their
// parameters without hand-rolled JSON parsing. The shape stays additive (only
// new fields appended) per schema-versioning policy.
struct CommandArgs
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    crd::containers::HashMap<crd::containers::String, ArgValue> values;

    explicit CommandArgs(crd::memory::IAllocator* a = crd::memory::default_allocator())
        : alloc(a), values(a)
    {
    }

    // Convenience setters — mirror ArgValue's setters keyed by param name.
    void set_bool(crd::containers::StringView name, bool v);
    void set_i64(crd::containers::StringView name, crd::i64 v);
    void set_u64(crd::containers::StringView name, crd::u64 v);
    void set_f64(crd::containers::StringView name, crd::f64 v);
    void set_complex64(crd::containers::StringView name, const crd::hesap::Complex64& v);
    void set_string(crd::containers::StringView name, crd::containers::StringView v);
    void set_f64_array(crd::containers::StringView name, crd::containers::ConstSpan<crd::f64> v);
    void set_i64_array(crd::containers::StringView name, crd::containers::ConstSpan<crd::i64> v);
    void set_matrix_id(crd::containers::StringView name, crd::hesap::MatrixId v);
    void set_vector_id(crd::containers::StringView name, crd::hesap::VectorId v);

    // Lookup helpers — return nullopt / empty view when missing or kind mismatch.
    [[nodiscard]] const ArgValue* find(crd::containers::StringView name) const noexcept;
    [[nodiscard]] std::optional<crd::f64> get_f64(crd::containers::StringView name) const;
    [[nodiscard]] std::optional<crd::i64> get_i64(crd::containers::StringView name) const;
    [[nodiscard]] std::optional<crd::u64> get_u64(crd::containers::StringView name) const;
    [[nodiscard]] std::optional<bool> get_bool(crd::containers::StringView name) const;
    [[nodiscard]] std::optional<crd::hesap::Complex64> get_complex64(crd::containers::StringView name) const;
    [[nodiscard]] crd::containers::StringView get_string(crd::containers::StringView name) const;
    [[nodiscard]] crd::containers::ConstSpan<crd::f64> get_f64_array(crd::containers::StringView name) const;
    [[nodiscard]] crd::containers::ConstSpan<crd::i64> get_i64_array(crd::containers::StringView name) const;
    [[nodiscard]] std::optional<crd::hesap::MatrixId> get_matrix_id(crd::containers::StringView name) const;
    [[nodiscard]] std::optional<crd::hesap::VectorId> get_vector_id(crd::containers::StringView name) const;
};

using CommandImpl = CommandResult (*)(const CommandArgs& args);

struct CommandRecord
{
    CommandSchema schema;
    CommandImpl impl = nullptr;

    explicit CommandRecord(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) : schema(alloc) {}
};

class CommandRegistry
{
public:
    // Singleton accessor (Meyers). Lazy-initialised on first call; safe
    // across translation units.
    [[nodiscard]] static CommandRegistry& global() noexcept;

    // Register a command. The schema is moved in; name must be unique.
    // Returns true on success; false (and emits a Diagnostic via the
    // engine's assert path) if a duplicate name is registered.
    bool register_command(CommandSchema schema, CommandImpl impl) noexcept;

    // Lookup by dotted name (e.g. "hesap.test.echo"). Returns nullptr
    // when not found.
    [[nodiscard]] const CommandRecord* find(crd::containers::StringView name) const noexcept;

    // Iterate every registered record. Order is insertion order; useful
    // for deterministic catalog emission (meta.export-mcp-tools).
    [[nodiscard]] crd::containers::ConstSpan<const CommandRecord*> all() const noexcept;

    [[nodiscard]] crd::usize size() const noexcept;

    // Clear all registrations. Used by tests; never call in production code.
    void clear_for_tests() noexcept;

    // Public default ctor lets tests construct LOCAL registry instances and
    // avoid stomping the global() registry's static-init populated entries
    // (BLAS L1 commands etc. arrive via CRD_HESAP_CLI_REGISTER_MODULE before
    // main runs; clearing the global registry would lose them irretrievably).
    CommandRegistry();
    ~CommandRegistry() = default;
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

private:
    // Owns the records by value; the pointer view returned from all() points
    // into m_records. m_index maps dotted-name → m_records index.
    crd::containers::Array<CommandRecord> m_records;
    crd::containers::HashMap<crd::containers::String, crd::usize> m_index;
    mutable crd::containers::Array<const CommandRecord*> m_pointer_cache;
    mutable bool m_pointer_cache_stale = true;
};

// -----------------------------------------------------------------------
// CRD_HESAP_CLI_REGISTER_MODULE — static-init registration macro.
//
// Usage:
//
//   namespace
//   {
//       crd::hesap::cli::CommandResult my_impl(const crd::hesap::cli::CommandArgs&);
//
//       struct Register
//       {
//           Register()
//           {
//               using namespace crd::hesap::cli;
//               CommandSchema s;
//               s.name = "hesap.dense.foo";
//               // ...fill in...
//               CommandRegistry::global().register_command(std::move(s), my_impl);
//           }
//       };
//       const Register g_register;
//   }
//
// The macro just wraps that boilerplate in a unique anonymous-namespace
// struct so the user doesn't have to invent a name. The `init_fn` argument
// is a callable that takes a CommandRegistry& and registers however many
// commands the module owns.
// -----------------------------------------------------------------------

namespace detail
{
template <typename Init>
struct ModuleRegistrar
{
    explicit ModuleRegistrar(Init init) noexcept { init(CommandRegistry::global()); }
};

// Helper that deduces the lambda's type from a single evaluation. The macro
// below uses this with `auto` so the lambda literal is materialised exactly
// once — using `decltype(lambda)` in a template-argument position evaluates
// the lambda expression a SECOND time, producing a fresh anonymous closure
// type that doesn't match the one passed as the ctor argument.
template <typename Init>
[[nodiscard]] auto make_module_registrar(Init init) noexcept -> ModuleRegistrar<Init>
{
    return ModuleRegistrar<Init>{init};
}
} // namespace detail

// Token-pasting helpers — macros are the right tool here (the line-number must
// expand at the call site to form a unique identifier; a template can't do that).
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define CRD_HESAP_CLI_REGISTER_MODULE_INTERNAL_CONCAT2(a, b) a##b
#define CRD_HESAP_CLI_REGISTER_MODULE_INTERNAL_CONCAT(a, b) CRD_HESAP_CLI_REGISTER_MODULE_INTERNAL_CONCAT2(a, b)

#define CRD_HESAP_CLI_REGISTER_MODULE(init_lambda)                                                                     \
    namespace                                                                                                          \
    {                                                                                                                  \
    const auto CRD_HESAP_CLI_REGISTER_MODULE_INTERNAL_CONCAT(g_crd_hesap_cli_registrar_, __LINE__) =                   \
        ::crd::hesap::cli::detail::make_module_registrar(init_lambda);                                                 \
    }
// NOLINTEND(cppcoreguidelines-macro-usage)

} // namespace crd::hesap::cli
