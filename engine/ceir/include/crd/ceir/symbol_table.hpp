#pragma once

// crd-ceir — SymbolTable + Visibility (CEIR-1b, §34). The name→definition index for the `ceir.func` dialect (and any
// later symbol-defining op). ARENA-BACKED: the underlying `crd::containers::HashMap` uses the Context arena, so it is
// never freed individually — its buckets are reclaimed wholesale when the Context is destroyed (the CEIR-1a arena
// mutation policy). Keys are arena-stable `StringView`s (the Context interns the name before `define`).

#include <crd/ceir/detail/string_view_hash.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir
{
class Operation;

// Symbol visibility (§34). A minimal typed field NOW; CEIR-1c may re-express it as an interned attribute, but this
// three-state model is stable and is NOT throwaway — the symbol table stores it per definition.
enum class Visibility : u8
{
    Public = 0, // exported — resolvable from other modules
    Private,    // module-internal
    Nested,     // resolvable only within the parent symbol-table scope
};

// One symbol definition: the defining op + its visibility.
struct SymbolEntry
{
    Operation* op         = nullptr;
    Visibility visibility = Visibility::Public;
};

// A per-Module symbol table: symbol name → {defining op, visibility}. Reuses `crd::containers::HashMap` (Robin Hood).
class SymbolTable
{
public:
    explicit SymbolTable(memory::IAllocator* alloc) : m_table(alloc) {}

    // Register `name → {op, vis}`. Returns false if `name` is ALREADY defined (the existing entry is kept — a
    // redefinition is a caller error the func-dialect helper turns into a nullptr, never a silent overwrite).
    bool define(containers::StringView name, Operation* op, Visibility vis)
    {
        return m_table.insert(name, SymbolEntry{op, vis});
    }

    [[nodiscard]] SymbolEntry*       lookup(containers::StringView name) noexcept { return m_table.find(name); }
    [[nodiscard]] const SymbolEntry* lookup(containers::StringView name) const noexcept { return m_table.find(name); }
    [[nodiscard]] bool  contains(containers::StringView name) const noexcept { return m_table.contains(name); }
    [[nodiscard]] usize size() const noexcept { return m_table.size(); }

private:
    containers::HashMap<containers::StringView, SymbolEntry, detail::StringViewHash> m_table;
};
} // namespace crd::ceir
