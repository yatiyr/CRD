#include <crd/ceir/context.hpp>

#include <crd/ceir/detail/symbol_registration.hpp> // CEIR-8i: resync_symbols reuses the shared registration path
#include <crd/ceir/symbol_table.hpp>
#include <crd/containers/hash.hpp>
#include <crd/memory/construct.hpp>

namespace crd::ceir
{
namespace
{
// Byte-order (unsigned) less — the canonical Dict-key ordering (CEIR-8b).
[[nodiscard]] bool sv_less(containers::StringView a, containers::StringView b) noexcept
{
    const usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; ++i)
    {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca != cb) { return ca < cb; }
    }
    return a.size() < b.size();
}
} // namespace

Context::Context(memory::IAllocator* alloc, usize arena_chunk_bytes)
    : m_arena(arena_chunk_bytes, alloc), m_op_names(alloc), // GrowableLinearAllocator is (chunk_bytes, parent)
      m_type_class_names(alloc), m_attr_class_names(alloc), m_location_class_names(alloc), m_attr_values(alloc),
      m_files(alloc), m_dialects(&m_arena), m_op_infos(&m_arena), m_type_classes(&m_arena), m_attr_classes(&m_arena),
      m_location_classes(&m_arena), m_interface_names(alloc), m_capability_names(alloc)
{
}

OpId Context::intern_op(containers::StringView dialect, containers::StringView name)
{
    // Build "dialect.op" on the stack for hashing; op names are short.
    char        buf[256];
    const usize n = dialect.size() + 1U + name.size();
    CRD_ASSERT_MSG(n < sizeof(buf), "ceir op name too long");
    usize k = 0;
    for (usize i = 0; i < dialect.size(); ++i) { buf[k++] = dialect[i]; }
    buf[k++] = '.';
    for (usize i = 0; i < name.size(); ++i) { buf[k++] = name[i]; }
    buf[k] = '\0';

    const u64 h = containers::hash_string(buf, n);
    for (usize i = 0; i < m_op_names.size(); ++i)
    {
        if (m_op_names[i].hash == h) { return OpId{h}; } // already interned — no arena/heap churn
    }
    // New kind: copy the name into the arena and record it (reverse lookup for diagnostics).
    char* const stored = static_cast<char*>(m_arena.allocate(n + 1U, 1U));
    for (usize i = 0; i <= n; ++i) { stored[i] = buf[i]; }
    m_op_names.push_back(OpName{h, containers::StringView(stored, n)});
    return OpId{h};
}

containers::StringView Context::op_name(OpId id) const noexcept
{
    for (usize i = 0; i < m_op_names.size(); ++i)
    {
        if (m_op_names[i].hash == id.value) { return m_op_names[i].name; }
    }
    return containers::StringView{};
}

// ── CEIR-8a open-world type classes (ADR-0111) — intern/reverse-lookup mirroring intern_op/op_name exactly ──
TypeClassId Context::intern_type_class(containers::StringView dialect, containers::StringView cls)
{
    char        buf[256]; // "dialect.class" on the stack for hashing; class names are short (matches intern_op)
    usize       k = 0;
    const usize n = dialect.size() + 1U + cls.size();
    CRD_ASSERT_MSG(n < sizeof(buf), "intern_type_class: dialect.class too long");
    for (usize i = 0; i < dialect.size(); ++i) { buf[k++] = dialect[i]; }
    buf[k++] = '.';
    for (usize i = 0; i < cls.size(); ++i) { buf[k++] = cls[i]; }
    buf[k] = '\0';

    const u64 h = containers::hash_string(buf, n);
    for (usize i = 0; i < m_type_class_names.size(); ++i)
    {
        if (m_type_class_names[i].hash == h) { return TypeClassId{h}; } // already interned — no churn
    }
    char* const stored = static_cast<char*>(m_arena.allocate(n + 1U, 1U));
    for (usize i = 0; i <= n; ++i) { stored[i] = buf[i]; }
    m_type_class_names.push_back(OpName{h, containers::StringView(stored, n)});
    return TypeClassId{h};
}

containers::StringView Context::type_class_name(TypeClassId id) const noexcept
{
    for (usize i = 0; i < m_type_class_names.size(); ++i)
    {
        if (m_type_class_names[i].hash == id.value) { return m_type_class_names[i].name; }
    }
    return containers::StringView{};
}

const TypeClassInfo* Context::type_class_info(TypeClassId id) const noexcept
{
    TypeClassInfo* const* slot = m_type_classes.find(id.value);
    return slot != nullptr ? *slot : nullptr; // nullptr ⇒ unregistered ⇒ preserve opaquely (U-§56)
}

bool Context::verify_extern(const Type& t) const noexcept
{
    const TypeClassInfo* const info = type_class_info(t.type_class);
    if (info == nullptr || info->verify == nullptr) { return true; } // unregistered / no hook ⇒ preserve (U-§56)
    return info->verify(*this, t);
}

TypeId Context::type_extern(TypeClassId cls, const Type& params)
{
    Type t       = params; // the caller's slots (members/count/cols/is_signed/fkind/name/labels)
    t.kind       = TypeKind::Extern;
    t.type_class = cls;
    // A registered class stamps its CURRENT schema version (authoritative); an unregistered class keeps the caller's
    // (the decoder path sets it from the blob for a preserved unknown type — this factory path is for known classes).
    if (const TypeClassInfo* const info = type_class_info(cls)) { t.type_class_version = info->version; }
    // ⛔ the FACTORY boundary asserts (builder misuse = programmer error); the decoder/parser use verify_extern + reject.
    CRD_ASSERT_MSG(verify_extern(t), "type_extern: the type-class verify hook rejected this instance");
    return intern_type(t);
}

Module* Context::create_module(RegionKind body_kind)
{
    Module* const m = memory::construct<Module>(m_arena);
    m->m_body       = create_region(body_kind);
    m->m_symbols    = memory::construct<SymbolTable>(m_arena, &m_arena); // arena-backed name→def index (§34)
    return m;
}

containers::StringView Context::intern_symbol(containers::StringView name)
{
    if (name.empty()) { return {}; }
    char* const stored = static_cast<char*>(m_arena.allocate(name.size(), 1U));
    for (usize i = 0; i < name.size(); ++i) { stored[i] = name[i]; }
    return containers::StringView(stored, name.size());
}

AttrId Context::intern_attr(const AttrValue& v)
{
    // ⛔ CEIR-8b: only a CANONICAL value enters the table (every field a kind does not use is default) — a non-canonical
    // one would intern distinctly / serialize divergently (the intern_type house guard, extended to attributes).
    CRD_ASSERT_MSG(attr_is_canonical(v), "intern_attr: non-canonical attribute value");
    for (usize i = 0; i < m_attr_values.size(); ++i)
    {
        if (m_attr_values[i] == v) { return AttrId{static_cast<u32>(i + 1U)}; } // dedup by value (element-wise for aggregates)
    }
    AttrValue stored = v;
    // deep-stabilize the borrowed aggregate spans into the arena (like intern_type's child span); scalars/wrappers carry
    // no span. Keys are interned so their StringViews are arena-stable.
    if (v.elems.size() > 0U)
    {
        auto* const e = static_cast<AttrId*>(m_arena.allocate(v.elems.size() * sizeof(AttrId), alignof(AttrId)));
        for (usize i = 0; i < v.elems.size(); ++i) { e[i] = v.elems[i]; }
        stored.elems = containers::ConstSpan<AttrId>(e, v.elems.size());
    }
    if (v.keys.size() > 0U)
    {
        auto* const k = static_cast<containers::StringView*>(
            m_arena.allocate(v.keys.size() * sizeof(containers::StringView), alignof(containers::StringView)));
        for (usize i = 0; i < v.keys.size(); ++i) { k[i] = intern_symbol(v.keys[i]); }
        stored.keys = containers::ConstSpan<containers::StringView>(k, v.keys.size());
    }
    m_attr_values.push_back(stored);
    return AttrId{static_cast<u32>(m_attr_values.size())}; // index + 1 (0 = invalid)
}

AttrId Context::attr_dict(containers::ConstSpan<containers::StringView> keys, containers::ConstSpan<AttrId> values)
{
    CRD_ASSERT_MSG(keys.size() == values.size(), "attr_dict: keys/values length mismatch");
    // canonicalize: SORT (key, value) pairs by key byte-order (one representation for dedup + content-hash stability;
    // authored key order is not semantic). Insertion sort — dicts are small. A duplicate key trips attr_is_canonical.
    containers::Array<containers::StringView> ks(allocator());
    containers::Array<AttrId>                 vs(allocator());
    for (usize i = 0; i < keys.size(); ++i)
    {
        ks.push_back(keys[i]);
        vs.push_back(values[i]);
    }
    for (usize i = 1; i < ks.size(); ++i)
    {
        const containers::StringView kk = ks[i];
        const AttrId                 vv = vs[i];
        usize                        j  = i;
        while (j > 0U && sv_less(kk, ks[j - 1U]))
        {
            ks[j] = ks[j - 1U];
            vs[j] = vs[j - 1U];
            --j;
        }
        ks[j] = kk;
        vs[j] = vv;
    }
    return intern_attr(AttrValue::of_dict(containers::ConstSpan<containers::StringView>(ks.data(), ks.size()),
                                          containers::ConstSpan<AttrId>(vs.data(), vs.size())));
}

AttrId Context::attr_typed(TypeId ty, AttrId value)
{
    const AttrValue v = AttrValue::of_typed_const(ty, value);
    CRD_ASSERT_MSG(verify_attr_extern(v), "attr_typed: the payload must not itself be a wrapper (composition rule)");
    return intern_attr(v);
}

AttrId Context::attr_extern(AttrClassId cls, AttrId value)
{
    u32 ver = 0U;
    if (const AttrClassInfo* const info = attr_class_info(cls)) { ver = info->version; } // registered class stamps its version
    const AttrValue v = AttrValue::of_extern(cls, ver, value);
    CRD_ASSERT_MSG(verify_attr_extern(v), "attr_extern: the class verify hook rejected the value (or a wrapper payload)");
    return intern_attr(v);
}

// ── CEIR-8b open-world attribute classes (ADR-0112) — intern/reverse-lookup/descriptor, mirroring the type-class set ──
AttrClassId Context::intern_attr_class(containers::StringView dialect, containers::StringView cls)
{
    char        buf[256];
    usize       k = 0;
    const usize n = dialect.size() + 1U + cls.size();
    CRD_ASSERT_MSG(n < sizeof(buf), "intern_attr_class: dialect.attr too long");
    for (usize i = 0; i < dialect.size(); ++i) { buf[k++] = dialect[i]; }
    buf[k++] = '.';
    for (usize i = 0; i < cls.size(); ++i) { buf[k++] = cls[i]; }
    buf[k] = '\0';
    const u64 h = containers::hash_string(buf, n);
    for (usize i = 0; i < m_attr_class_names.size(); ++i)
    {
        if (m_attr_class_names[i].hash == h) { return AttrClassId{h}; }
    }
    char* const stored = static_cast<char*>(m_arena.allocate(n + 1U, 1U));
    for (usize i = 0; i <= n; ++i) { stored[i] = buf[i]; }
    m_attr_class_names.push_back(OpName{h, containers::StringView(stored, n)});
    return AttrClassId{h};
}

containers::StringView Context::attr_class_name(AttrClassId id) const noexcept
{
    for (usize i = 0; i < m_attr_class_names.size(); ++i)
    {
        if (m_attr_class_names[i].hash == id.value) { return m_attr_class_names[i].name; }
    }
    return containers::StringView{};
}

const AttrClassInfo* Context::attr_class_info(AttrClassId id) const noexcept
{
    AttrClassInfo* const* slot = m_attr_classes.find(id.value);
    return slot != nullptr ? *slot : nullptr; // nullptr ⇒ unregistered ⇒ preserve opaquely (U-§56)
}

bool Context::verify_attr_extern(const AttrValue& v) const noexcept
{
    // ⛔ the WRAPPER gate: a wrapper's payload must not itself be a wrapper (the qty<qty> composition rule) — for BOTH
    // TypedConst and Extern; then the Extern class's verify hook (unregistered/no-hook ⇒ preserve, U-§56).
    if (v.kind == AttrKind::TypedConst || v.kind == AttrKind::Extern)
    {
        const AttrValue pv = attr_value(v.payload);
        if (pv.kind == AttrKind::TypedConst || pv.kind == AttrKind::Extern) { return false; }
    }
    if (v.kind == AttrKind::Extern)
    {
        const AttrClassInfo* const info = attr_class_info(v.attr_class);
        if (info != nullptr && info->verify != nullptr) { return info->verify(*this, v); }
    }
    return true;
}

// ── CEIR-8c open-world effect-LOCATION classes (ADR-0113) — intern/reverse-lookup/descriptor, mirroring the attr-class
// set; plus the effective-resource-class rule the hazard analysis consults ──
LocationClassId Context::intern_location_class(containers::StringView dialect, containers::StringView cls)
{
    char        buf[256];
    usize       k = 0;
    const usize n = dialect.size() + 1U + cls.size();
    CRD_ASSERT_MSG(n < sizeof(buf), "intern_location_class: dialect.location too long");
    for (usize i = 0; i < dialect.size(); ++i) { buf[k++] = dialect[i]; }
    buf[k++] = '.';
    for (usize i = 0; i < cls.size(); ++i) { buf[k++] = cls[i]; }
    buf[k] = '\0';
    const u64 h = containers::hash_string(buf, n);
    for (usize i = 0; i < m_location_class_names.size(); ++i)
    {
        if (m_location_class_names[i].hash == h) { return LocationClassId{h}; }
    }
    char* const stored = static_cast<char*>(m_arena.allocate(n + 1U, 1U));
    for (usize i = 0; i <= n; ++i) { stored[i] = buf[i]; }
    m_location_class_names.push_back(OpName{h, containers::StringView(stored, n)});
    return LocationClassId{h};
}

containers::StringView Context::location_class_name(LocationClassId id) const noexcept
{
    for (usize i = 0; i < m_location_class_names.size(); ++i)
    {
        if (m_location_class_names[i].hash == id.value) { return m_location_class_names[i].name; }
    }
    return containers::StringView{};
}

const LocationClassInfo* Context::location_class_info(LocationClassId id) const noexcept
{
    LocationClassInfo* const* slot = m_location_classes.find(id.value);
    return slot != nullptr ? *slot : nullptr; // nullptr ⇒ unregistered ⇒ maximally-conflicting (Universe) in the analysis
}

ResourceClass Context::effect_resource_class(const EffectRecord& e) const noexcept
{
    if (e.target == EffectTarget::Extern)
    {
        const LocationClassInfo* const info = location_class_info(e.location_class);
        // ⛔ EMPTY≠UNKNOWN: an UNREGISTERED Extern location conflicts with EVERYTHING (never inert), so a plugin
        // location whose class this loader does not know is scheduled maximally conservatively.
        return info != nullptr ? info->resource_class : ResourceClass::Universe;
    }
    return effect_access(e.family).klass; // None/Operand/Result + the built-in kinds use the family's class (8c)
}

bool Context::effect_location_valid(const EffectRecord& e) const noexcept
{
    if (e.target != EffectTarget::Extern) { return true; }
    const LocationClassInfo* const info = location_class_info(e.location_class);
    if (info != nullptr && info->verify != nullptr) { return info->verify(*this, e); }
    return true; // unregistered / no hook ⇒ preserve (the analysis treats an unregistered class as Universe)
}

// ── CEIR-8d (ADR-0114) stable semantic identity — module-scoped, pre-order, idempotent assignment ──
void Context::stable_id_scan_max(Region* r, u64& mx) const noexcept
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (op->m_stable_id.value > mx) { mx = op->m_stable_id.value; }
            for (u32 i = 0; i < op->num_regions(); ++i) { stable_id_scan_max(op->region(i), mx); }
        }
    }
}
void Context::stable_id_assign_unset(Region* r, u64& next) const noexcept
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (!op->m_stable_id.valid()) { op->m_stable_id = StableId{next++}; } // one-time; never re-derive an assigned id
            for (u32 i = 0; i < op->num_regions(); ++i) { stable_id_assign_unset(op->region(i), next); }
        }
    }
}
void Context::assign_stable_ids(const Module& m) const noexcept
{
    u64 mx = 0U;
    stable_id_scan_max(m.body(), mx); // max of the CURRENT ids (pure function of existing ids + pre-order)
    // ⛔ CEIR-8d id-reuse guard (advisor pre-close): draw NEW ids from the WATERMARK too, not just the live max — a
    // tombstoned (erased) op is invisible to the scan, so without this a later op would reuse the dead op's id and the
    // §2.7 delete/re-add discriminator would silently pass (state corruption). Identity is monotone per module.
    if (m.m_stable_id_watermark > mx) { mx = m.m_stable_id_watermark; }
    u64 next = mx + 1U;
    stable_id_assign_unset(m.body(), next);
    m.m_stable_id_watermark = next - 1U; // the new high-water mark (>= the old one — monotone)
}
void Context::set_stable_id(Operation* op, StableId id) noexcept
{
    if (op != nullptr) { op->m_stable_id = id; } // deserialization-only (the STID loader); Context is a friend of Operation
}
void Context::set_stable_id_watermark(Module* m, u64 watermark) noexcept
{
    if (m != nullptr) { m->m_stable_id_watermark = watermark; } // deserialization-only: restore the monotone high-water mark
}

// ── CEIR-8i (ADR-0119) transaction support — inverse/rebuild atoms the `Transaction` recorder routes through ──
namespace
{
// Pre-order (block args → op results, recursing regions — the printer/STID order) search for the op carrying `id`.
[[nodiscard]] const Operation* find_sid(Region* r, StableId id) noexcept
{
    if (r == nullptr) { return nullptr; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (op->stable_id() == id) { return op; }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                if (const Operation* const hit = find_sid(op->region(i), id)) { return hit; }
            }
        }
    }
    return nullptr;
}
// Pre-order re-register of every symbol-defining op into `m`'s CURRENT SymbolTable; returns the FIRST duplicate op
// (nullptr ⇒ clean). Reuses the shared detail::register_symbol path (never a second registration path).
[[nodiscard]] const Operation* resync_walk(Region* r, Context& ctx, Module& m) noexcept
{
    if (r == nullptr) { return nullptr; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (!detail::register_symbol(ctx, m, op)) { return op; }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                if (const Operation* const dup = resync_walk(op->region(i), ctx, m)) { return dup; }
            }
        }
    }
    return nullptr;
}
} // namespace

void Context::reinsert_erased_op(Operation* op, Block* block, Operation* before,
                                 containers::ConstSpan<Value*> operands) noexcept
{
    if (op == nullptr) { return; }
    // erase() left the operand array + Use slots allocated (arena) but zeroed the count and nulled each Use.value; restore
    // the count and re-thread the recorded operand values into those same slots (re-adding each to its value's use-list).
    op->m_num_operands = static_cast<u32>(operands.size());
    for (u32 i = 0; i < op->m_num_operands; ++i)
    {
        Use& u  = op->m_operands[i];
        u.owner = op;
        u.value = operands[i];
        if (operands[i] != nullptr) { operands[i]->add_use(&u); }
    }
    op->m_erased = false;
    if (block != nullptr) { block->insert_before(op, before); } // before == nullptr ⇒ append (Block::insert_before)
}

void Context::detach_and_point_use(Use* u, Value* value) noexcept
{
    if (u == nullptr) { return; }
    if (u->value != nullptr) { u->value->remove_use(u); } // unlink from its current value's list (nulls u->value/next/prev)
    u->value = value;
    if (value != nullptr) { value->add_use(u); }
}

void Context::rauw_recording(Value* from, Value* to, containers::Array<Use*>& moved)
{
    if (from == nullptr || from == to) { return; }
    while (from->m_first_use != nullptr) // friend read; detach advances the head so the loop terminates
    {
        Use* const u = from->m_first_use;
        moved.push_back(u);
        detach_and_point_use(u, to);
    }
}

void Context::restore_attr_dict(Operation* op, NamedAttr* dict, u32 count) noexcept
{
    if (op == nullptr) { return; }
    op->m_attrs     = dict;  // the prior (Context-arena) snapshot BECOMES live module state (must outlive the Transaction)
    op->m_num_attrs = count; // dict == nullptr / count == 0 restores a no-attr op
}

bool Context::resync_symbols(Module& m, const Operation*& first_dup)
{
    first_dup                   = nullptr;
    SymbolTable* const old_table = m.m_symbols;
    SymbolTable* const fresh      = memory::construct<SymbolTable>(m_arena, &m_arena); // old table leaks (arena policy)
    m.m_symbols                  = fresh; // register_symbol registers into m.symbols() == fresh
    if (const Operation* const dup = resync_walk(m.body(), *this, m))
    {
        m.m_symbols = old_table; // atomic-on-failure: discard the fresh table, the old (pre-tx-correct) index is untouched
        first_dup   = dup;
        return false;
    }
    return true; // the fresh table is the committed index
}

const Operation* Context::find_by_stable_id(const Module& m, StableId id) const noexcept
{
    if (!id.valid()) { return nullptr; }
    return find_sid(m.body(), id);
}

// ── CEIR-8f capabilities (ADR-0116, U-§57) — intern (FNV, intern-only) / reverse-lookup / program set / host grant ──
CapabilityId Context::intern_capability(containers::StringView name)
{
    const u64 h = containers::hash_string(name.data(), name.size()); // the InterfaceId FNV shape — no verify/version
    for (usize i = 0; i < m_capability_names.size(); ++i)
    {
        if (m_capability_names[i].hash == h) { return CapabilityId{h}; }
    }
    m_capability_names.push_back(OpName{h, intern_symbol(name)});
    return CapabilityId{h};
}

containers::StringView Context::capability_name(CapabilityId id) const noexcept
{
    for (usize i = 0; i < m_capability_names.size(); ++i)
    {
        if (m_capability_names[i].hash == id.value) { return m_capability_names[i].name; }
    }
    return containers::StringView{};
}

namespace
{
// Module-wide pre-order (the StateW::go shape) — funcs ARE ops in the body, so this already visits every private
// callee; no call-resolution / "transitive" machinery. An UNREGISTERED op contributes `external.process` (EMPTY≠UNKNOWN).
void gather_program_caps(const Context& ctx, Region* r, containers::Array<CapabilityId>& out, CapabilityId external_process)
{
    if (r == nullptr) { return; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_info(op->kind()) == nullptr) { out.push_back(external_process); }
            else
            {
                const containers::ConstSpan<CapabilityId> caps = ctx.op_capabilities(op->kind());
                for (usize i = 0; i < caps.size(); ++i) { out.push_back(caps[i]); }
            }
            for (u32 i = 0; i < op->num_regions(); ++i) { gather_program_caps(ctx, op->region(i), out, external_process); }
        }
    }
}
} // namespace

void Context::program_capabilities(const Module& m, containers::Array<CapabilityId>& out) const
{
    const containers::StringView ep_name{"external.process"};
    const CapabilityId ep{containers::hash_string(ep_name.data(), ep_name.size())}; // pure FNV — no mutation (const method)
    gather_program_caps(*this, m.body(), out, ep);
    // SORT (insertion; sets are small) + DEDUP adjacent → a sorted-UNIQUE set: a REORDER is invariant, membership (a
    // deleted/added cap) changes the hash (the 8c/8d discriminator). ⛔ the id VALUE is what enters the hash.
    for (usize i = 1; i < out.size(); ++i)
    {
        const CapabilityId key = out[i];
        usize              j   = i;
        while (j > 0U && out[j - 1U].value > key.value)
        {
            out[j] = out[j - 1U];
            --j;
        }
        out[j] = key;
    }
    usize w = 0;
    for (usize i = 0; i < out.size(); ++i)
    {
        if (w == 0U || !(out[w - 1U] == out[i])) { out[w++] = out[i]; }
    }
    while (out.size() > w) { out.pop_back(); }
}

SafetyBits Context::op_safety(OpId kind) const noexcept
{
    const OpInfo* const info = op_info(kind);
    if (info == nullptr) { return {true, true, true}; } // ⛔ EMPTY≠UNKNOWN: unregistered ⇒ maximally unsafe
    SafetyBits s{false, false, false};
    for (u32 i = 0; i < info->num_effects; ++i) { s = s.merged(effect_safety(info->effects[i].family)); }
    return s; // a registered EFFECT-FREE op ⇒ all-false ⇒ realtime_safe (genuinely declared no effects)
}

bool Context::capabilities_satisfied(containers::ConstSpan<CapabilityId> required,
                                     containers::ConstSpan<CapabilityId> granted) noexcept
{
    for (usize i = 0; i < required.size(); ++i)
    {
        bool found = false;
        for (usize j = 0; j < granted.size() && !found; ++j) { found = required[i] == granted[j]; }
        if (!found) { return false; } // an ungranted required capability ⇒ the host must NOT run this program
    }
    return true;
}

AttrValue Context::attr_value(AttrId id) const noexcept
{
    if (!id.valid() || id.value > m_attr_values.size()) { return AttrValue::of_int(0); }
    return m_attr_values[id.value - 1U];
}

void Context::set_attr(Operation* op, containers::StringView name, AttrId value)
{
    for (u32 k = 0; k < op->m_num_attrs; ++k) // overwrite in place if `name` is already present
    {
        if (op->m_attrs[k].name == name)
        {
            op->m_attrs[k].value = value;
            return;
        }
    }
    const u32        n     = op->m_num_attrs; // grow by rebuild (old slice leaks into the arena — operand-grow policy)
    NamedAttr* const grown = memory::construct_array<NamedAttr>(m_arena, n + 1U);
    for (u32 k = 0; k < n; ++k) { grown[k] = op->m_attrs[k]; }
    grown[n]        = NamedAttr{intern_symbol(name), value};
    op->m_attrs     = grown;
    op->m_num_attrs = n + 1U;
}

// ── Types (CEIR-3a, §16) ── the AttrValue intern pattern, extended to structural types. `intern_type` DEEP-STABILIZES
// a borrowed `Type`: on a new record it arena-copies the child span, arena-interns each label, and interns the name —
// so a caller may build a temp `Type` pointing at stack data, and the stored one owns everything for the Context's life.
TypeId Context::intern_type(const Type& t)
{
    // ⛔ Symmetric with type_of's assert: only a CANONICAL type may enter the table — a structurally-invalid type would
    // OOB a later consumer, and a non-canonical one (junk in an ignored field) prints lossily, breaking form-agreement.
    // Every trusted path is canonical by construction (factories zero unused fields, substitute preserves them); the
    // untrusted binary path checks BEFORE interning — so this fires only on genuine programmer error.
    CRD_ASSERT_MSG(type_is_canonical(t), "intern_type: non-canonical or structurally-invalid type");
    for (usize i = 0; i < m_types.size(); ++i)
    {
        if (m_types[i] == t) { return TypeId{static_cast<u32>(i + 1U)}; } // dedup by structural equality (content)
    }
    Type stored = t;
    stored.name = intern_symbol(t.name); // arena-stable name (empty stays empty)
    if (!t.members.empty())
    {
        TypeId* const dst = memory::construct_array<TypeId>(m_arena, static_cast<u32>(t.members.size()));
        for (usize i = 0; i < t.members.size(); ++i) { dst[i] = t.members[i]; }
        stored.members = containers::ConstSpan<TypeId>(dst, t.members.size());
    }
    if (!t.labels.empty())
    {
        containers::StringView* const dst =
            memory::construct_array<containers::StringView>(m_arena, static_cast<u32>(t.labels.size()));
        for (usize i = 0; i < t.labels.size(); ++i) { dst[i] = intern_symbol(t.labels[i]); }
        stored.labels = containers::ConstSpan<containers::StringView>(dst, t.labels.size());
    }
    m_types.push_back(stored);
    return TypeId{static_cast<u32>(m_types.size())}; // index + 1 (0 = none)
}

Type Context::type_of(TypeId id) const noexcept
{
    // ⛔ No silent fallback: an out-of-range TypeId is a programmer error (IR built with an un-interned type handle).
    // The printer + serializer route here, so we abort loudly rather than mis-render a stray id as some default type.
    CRD_ASSERT_MSG(id.valid() && id.value <= m_types.size(), "type_of: invalid or un-interned TypeId");
    return m_types[id.value - 1U];
}

TypeId Context::type_int(u32 width, bool is_signed)
{
    Type t      = Type::scalar(TypeKind::Int);
    t.count     = width;
    t.is_signed = is_signed;
    return intern_type(t);
}

TypeId Context::type_float(FloatKind fk)
{
    Type t  = Type::scalar(TypeKind::Float);
    t.fkind = fk;
    return intern_type(t);
}

TypeId Context::type_vector(TypeId elem, u32 count)
{
    Type t          = Type::scalar(TypeKind::Vector);
    t.count         = count;
    const TypeId m[1] = {elem};
    t.members       = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

TypeId Context::type_matrix(TypeId elem, u32 rows, u32 cols)
{
    Type t          = Type::scalar(TypeKind::Matrix);
    t.count         = rows;
    t.cols          = cols;
    const TypeId m[1] = {elem};
    t.members       = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

TypeId Context::type_complex(TypeId elem)
{
    Type t          = Type::scalar(TypeKind::Complex);
    const TypeId m[1] = {elem};
    t.members       = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

TypeId Context::type_quaternion(TypeId elem)
{
    Type t          = Type::scalar(TypeKind::Quaternion);
    const TypeId m[1] = {elem};
    t.members       = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

TypeId Context::type_array(TypeId elem, u32 count)
{
    Type t          = Type::scalar(TypeKind::Array);
    t.count         = count;
    const TypeId m[1] = {elem};
    t.members       = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

TypeId Context::type_tuple(containers::ConstSpan<TypeId> members)
{
    Type t    = Type::scalar(TypeKind::Tuple);
    t.members = members;
    return intern_type(t);
}

TypeId Context::type_struct(containers::StringView name, containers::ConstSpan<TypeId> field_types,
                            containers::ConstSpan<containers::StringView> field_names)
{
    Type t    = Type::scalar(TypeKind::Struct);
    t.name    = name;
    t.members = field_types;
    t.labels  = field_names;
    return intern_type(t);
}

TypeId Context::type_enum(containers::StringView name, containers::ConstSpan<containers::StringView> cases)
{
    Type t   = Type::scalar(TypeKind::Enum);
    t.name   = name;
    t.labels = cases;
    return intern_type(t);
}

TypeId Context::type_variant(containers::ConstSpan<TypeId> alternatives)
{
    Type t    = Type::scalar(TypeKind::Variant);
    t.members = alternatives;
    return intern_type(t);
}

TypeId Context::type_option(TypeId elem)
{
    Type t          = Type::scalar(TypeKind::Option);
    const TypeId m[1] = {elem};
    t.members       = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

TypeId Context::type_result(TypeId ok, TypeId err)
{
    Type t          = Type::scalar(TypeKind::Result);
    const TypeId m[2] = {ok, err};
    t.members       = containers::ConstSpan<TypeId>(m, 2U);
    return intern_type(t);
}

TypeId Context::type_param(containers::StringView name, containers::ConstSpan<TypeId> constraints)
{
    Type t    = Type::scalar(TypeKind::TypeParam);
    t.name    = name;
    t.members = constraints; // the traits this param must satisfy
    return intern_type(t);
}

TypeId Context::type_trait(containers::StringView name, containers::ConstSpan<TypeId> supertraits)
{
    Type t    = Type::scalar(TypeKind::Trait);
    t.name    = name;
    t.members = supertraits;
    return intern_type(t);
}

TypeId Context::type_callable(containers::ConstSpan<TypeId> params, containers::ConstSpan<TypeId> results)
{
    containers::Array<TypeId> all(allocator()); // members = params ++ results; count = param count (results follow)
    for (usize i = 0; i < params.size(); ++i) { all.push_back(params[i]); }
    for (usize i = 0; i < results.size(); ++i) { all.push_back(results[i]); }
    Type t    = Type::scalar(TypeKind::Callable);
    t.count   = static_cast<u32>(params.size());
    t.members = containers::ConstSpan<TypeId>(all.data(), all.size());
    return intern_type(t);
}

bool Context::type_has_params(TypeId id) const noexcept
{
    if (!id.valid()) { return false; }
    // iterative worklist + seen-set — the interned type DAG can share children (tuple<X,X>), so a naive recursion is
    // exponential; visiting each TypeId once is linear in the DAG.
    containers::Array<TypeId>    work(allocator());
    containers::HashMap<u32, u8> seen(allocator());
    work.push_back(id);
    for (usize wi = 0; wi < work.size(); ++wi)
    {
        const TypeId cur = work[wi];
        if (seen.find(cur.value) != nullptr) { continue; }
        seen.insert(cur.value, 1U);
        const Type t = type_of(cur);
        if (t.kind == TypeKind::TypeParam) { return true; }
        for (usize i = 0; i < t.members.size(); ++i) { work.push_back(t.members[i]); }
    }
    return false;
}

void Context::register_conformance(TypeId concrete, TypeId trait)
{
    CRD_ASSERT_MSG(concrete.valid() && trait.valid(), "register_conformance: invalid type id");
    for (usize i = 0; i < m_conformances.size(); ++i) // dedup — a fact registered twice is one fact
    {
        if (m_conformances[i].concrete == concrete.value && m_conformances[i].trait == trait.value) { return; }
    }
    m_conformances.push_back(Conformance{concrete.value, trait.value});
}

namespace
{
// `t` (a Trait) is-a `target` — target is t itself or a transitive supertrait of t. Iterative + seen-set (supertrait
// DAGs can share; acyclic because a supertrait is interned before the trait that names it).
[[nodiscard]] bool trait_conforms(const Context& ctx, TypeId t, TypeId target) noexcept
{
    containers::Array<TypeId>    work(ctx.allocator());
    containers::HashMap<u32, u8> seen(ctx.allocator());
    work.push_back(t);
    for (usize wi = 0; wi < work.size(); ++wi)
    {
        const TypeId cur = work[wi];
        if (cur == target) { return true; }
        if (seen.find(cur.value) != nullptr) { continue; }
        seen.insert(cur.value, 1U);
        const Type tt = ctx.type_of(cur); // cur is a Trait; members = its supertraits
        for (usize i = 0; i < tt.members.size(); ++i) { work.push_back(tt.members[i]); }
    }
    return false;
}
} // namespace

bool Context::satisfies(TypeId concrete, TypeId trait) const noexcept
{
    for (usize i = 0; i < m_conformances.size(); ++i)
    {
        // a registered (concrete -> T) satisfies `trait` if T is `trait` or has `trait` as a transitive supertrait
        if (m_conformances[i].concrete == concrete.value && trait_conforms(*this, TypeId{m_conformances[i].trait}, trait))
        {
            return true;
        }
    }
    return false;
}

namespace
{
// Substitute concrete types for TypeParams in `id`. Memoized (TypeId -> substituted TypeId) so a shared subtree is
// walked once; on a constraint violation sets `err` (ok=false + the offending param/trait) and unwinds. Ground subtrees
// return unchanged (identity — re-interning the same content would yield the same id anyway).
TypeId subst_rec(Context& ctx, TypeId id, containers::ConstSpan<TypeBinding> bindings,
                 containers::HashMap<u32, u32>& memo, SubstResult& err)
{
    if (!err.ok || !id.valid()) { return id; }
    if (const u32* const cached = memo.find(id.value)) { return TypeId{*cached}; }
    const Type t = ctx.type_of(id);
    TypeId     out;
    if (t.kind == TypeKind::TypeParam)
    {
        TypeId bound;
        for (usize i = 0; i < bindings.size(); ++i)
        {
            if (bindings[i].param == id) { bound = bindings[i].concrete; break; }
        }
        if (!bound.valid())
        {
            out = id; // unbound param remains — the result stays generic
        }
        else
        {
            for (usize i = 0; i < t.members.size(); ++i) // the concrete must satisfy every constraint trait
            {
                if (!ctx.satisfies(bound, t.members[i]))
                {
                    err.ok           = false;
                    err.failed_param = id;
                    err.failed_trait = t.members[i];
                    return id;
                }
            }
            out = bound;
        }
    }
    else if (t.members.size() == 0U)
    {
        out = id; // a ground leaf (scalar / empty aggregate) is unchanged
    }
    else
    {
        containers::Array<TypeId> subbed(ctx.allocator());
        bool                      changed = false;
        for (usize i = 0; i < t.members.size(); ++i)
        {
            const TypeId sm = subst_rec(ctx, t.members[i], bindings, memo, err);
            if (!err.ok) { return id; }
            if (sm != t.members[i]) { changed = true; }
            subbed.push_back(sm);
        }
        if (!changed)
        {
            out = id; // ground subtree — identity
        }
        else
        {
            Type nt    = t;
            nt.members = containers::ConstSpan<TypeId>(subbed.data(), subbed.size());
            // Substitution is a FOURTH construction path (beside parser / decoder / factory) — a bound param can land in
            // an underlying position that composition forbids (T -> qty makes qty<qty<...>>; T -> qual makes a qualifier
            // over a qualifier). Such a type is structurally canonical (the intern assert would pass) yet PRINTS-BUT-WONT
            // -REPARSE (the decoder re-checks composition), breaking form-agreement. Re-check the kinds whose composition
            // predicates admit a TypeParam so this path honors the same rules the other three do.
            bool compose_ok = true;
            if (t.kind == TypeKind::Quantity) { compose_ok = ctx.quantity_composition_valid(subbed[0]); }
            else if (t.kind == TypeKind::Qualified) { compose_ok = ctx.qualified_composition_valid(subbed[0]); }
            else if (t.kind == TypeKind::Tensor || t.kind == TypeKind::SparseTensor)
            {
                compose_ok = ctx.tensor_composition_valid(subbed[0], subbed[1]);
            }
            if (!compose_ok)
            {
                err.ok             = false;
                err.failed_compose = ctx.intern_type(nt); // the rebuilt composite — a pointing diagnostic (§16)
                return id;
            }
            out = ctx.intern_type(nt);
        }
    }
    memo.insert(id.value, out.value);
    return out;
}
} // namespace

SubstResult Context::substitute(TypeId id, containers::ConstSpan<TypeBinding> bindings)
{
    containers::HashMap<u32, u32> memo(allocator());
    SubstResult                   r;
    r.ok               = true; // reused as the "no violation yet" flag during the walk
    const TypeId out   = subst_rec(*this, id, bindings, memo, r);
    if (!r.ok) { return r; }   // r carries failed_param / failed_trait
    r.type = out;
    return r;
}

// ── Resources + views (CEIR-3c, §23) ──
TypeId Context::type_buffer(BufferMode mode, TypeId element)
{
    Type t            = Type::scalar(TypeKind::Buffer);
    t.count           = static_cast<u32>(mode);
    const TypeId m[1] = {element};
    if (mode != BufferMode::Raw) { t.members = containers::ConstSpan<TypeId>(m, 1U); } // raw has no element type
    return intern_type(t);
}

TypeId Context::type_image(ImageDim dim, TypeId format)
{
    Type t            = Type::scalar(TypeKind::Image);
    t.count           = static_cast<u32>(dim);
    const TypeId m[1] = {format};
    t.members         = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

TypeId Context::type_sampler(bool comparison)
{
    Type t      = Type::scalar(TypeKind::Sampler);
    t.is_signed = comparison; // comparison sampler?
    return intern_type(t);
}

TypeId Context::type_resource_table(TypeId element)
{
    Type t            = Type::scalar(TypeKind::ResourceTable);
    const TypeId m[1] = {element};
    t.members         = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

TypeId Context::type_accel_struct() { return intern_type(Type::scalar(TypeKind::AccelStruct)); }
TypeId Context::type_video_frame() { return intern_type(Type::scalar(TypeKind::VideoFrame)); }
TypeId Context::type_audio_buffer() { return intern_type(Type::scalar(TypeKind::AudioBuffer)); }
TypeId Context::type_external_resource() { return intern_type(Type::scalar(TypeKind::ExternalResource)); }

TypeId Context::type_view(TypeId underlying, u32 range_mask)
{
    CRD_ASSERT_MSG(view_combination_valid(underlying, range_mask), "type_view: invalid view/resource combination");
    Type t            = Type::scalar(TypeKind::View);
    t.count           = range_mask;
    const TypeId m[1] = {underlying};
    t.members         = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

bool Context::view_combination_valid(TypeId underlying, u32 range_mask) const noexcept
{
    if (!underlying.valid() || (range_mask & ~kViewRangeAll) != 0U) { return false; } // only the defined range bits
    const Type u = type_of(underlying);
    if (u.kind == TypeKind::Buffer) // buffers admit byte + element ranges
    {
        const u32 legal = static_cast<u32>(ViewRange::Byte) | static_cast<u32>(ViewRange::Element);
        return (range_mask & ~legal) == 0U;
    }
    if (u.kind == TypeKind::Image) // images admit mip + layer + aspect ranges
    {
        const u32 legal = static_cast<u32>(ViewRange::Mip) | static_cast<u32>(ViewRange::Layer) |
                          static_cast<u32>(ViewRange::Aspect);
        return (range_mask & ~legal) == 0U;
    }
    return false; // only buffers + images are viewable
}

// ── Shapes + tensors (CEIR-3d, §21/§35) ──
TypeId Context::type_dim_static(u32 extent)
{
    Type t  = Type::scalar(TypeKind::Dim);
    t.cols  = static_cast<u32>(DimKind::Static);
    t.count = extent;
    return intern_type(t);
}

TypeId Context::type_dim_symbolic(containers::StringView name)
{
    CRD_ASSERT_MSG(name != containers::StringView("dyn"), "type_dim_symbolic: 'dyn' is the reserved dynamic keyword");
    Type t = Type::scalar(TypeKind::Dim);
    t.cols = static_cast<u32>(DimKind::Symbolic);
    t.name = name;
    return intern_type(t);
}

TypeId Context::type_dim_dynamic()
{
    Type t = Type::scalar(TypeKind::Dim);
    t.cols = static_cast<u32>(DimKind::Dynamic);
    return intern_type(t);
}

bool Context::shape_members_valid(containers::ConstSpan<TypeId> dims) const noexcept
{
    for (usize i = 0; i < dims.size(); ++i)
    {
        if (!dims[i].valid() || type_of(dims[i]).kind != TypeKind::Dim) { return false; }
    }
    return true;
}

TypeId Context::type_shape(containers::ConstSpan<TypeId> dims)
{
    CRD_ASSERT_MSG(shape_members_valid(dims), "type_shape: every shape member must be a Dim");
    Type t    = Type::scalar(TypeKind::Shape);
    t.members = dims;
    return intern_type(t);
}

bool Context::tensor_composition_valid(TypeId element, TypeId shape) const noexcept
{
    if (!element.valid() || !shape.valid()) { return false; }
    const TypeKind ek = type_of(element).kind;
    if (ek == TypeKind::Dim || ek == TypeKind::Shape) { return false; } // element is a value type, never a dim/shape
    return type_of(shape).kind == TypeKind::Shape;
}

TypeId Context::type_tensor(TypeId element, TypeId shape)
{
    CRD_ASSERT_MSG(tensor_composition_valid(element, shape), "type_tensor: invalid element/shape composition");
    Type         t    = Type::scalar(TypeKind::Tensor);
    const TypeId m[2] = {element, shape};
    t.members         = containers::ConstSpan<TypeId>(m, 2U);
    return intern_type(t);
}

TypeId Context::type_sparse_tensor(TypeId element, TypeId shape)
{
    CRD_ASSERT_MSG(tensor_composition_valid(element, shape), "type_sparse_tensor: invalid element/shape composition");
    Type         t    = Type::scalar(TypeKind::SparseTensor);
    const TypeId m[2] = {element, shape};
    t.members         = containers::ConstSpan<TypeId>(m, 2U);
    return intern_type(t);
}

namespace
{
struct DimInfo
{
    DimKind                kind;
    u32                    extent;
    containers::StringView name;
};
[[nodiscard]] DimInfo dim_info(const Context& ctx, TypeId dim) noexcept
{
    const Type d = ctx.type_of(dim);
    return DimInfo{static_cast<DimKind>(d.cols), d.count, d.name};
}
[[nodiscard]] bool dim_is_one(const DimInfo& d) noexcept { return d.kind == DimKind::Static && d.extent == 1U; }

// One aligned dim pair: `1` broadcasts with anything; two statics agree or clash; two same-named symbolics agree; a
// dynamic / a differing symbolic / a symbolic-vs-non-1-static is statically UNKNOWN (defers to a runtime check).
[[nodiscard]] ShapeCompat broadcast_dim(const DimInfo& a, const DimInfo& b) noexcept
{
    if (dim_is_one(a) || dim_is_one(b)) { return ShapeCompat::Compatible; }
    if (a.kind == DimKind::Static && b.kind == DimKind::Static)
    {
        return a.extent == b.extent ? ShapeCompat::Compatible : ShapeCompat::Incompatible;
    }
    if (a.kind == DimKind::Symbolic && b.kind == DimKind::Symbolic && a.name == b.name) { return ShapeCompat::Compatible; }
    return ShapeCompat::Unknown;
}

// The product of a shape's extents when ALL dims are static, ⛔ overflow-checked (an overflowed product must not compare
// equal to a different overflowed one — the u32-wrap-mod scar family, u64 edition). Returns false ⇒ Unknown.
[[nodiscard]] bool static_extent_product(const Context& ctx, const Type& shape, u64& out) noexcept
{
    const u64 u64_max = ~static_cast<u64>(0U);
    u64       p       = 1U;
    for (usize i = 0; i < shape.members.size(); ++i)
    {
        const DimInfo d = dim_info(ctx, shape.members[i]);
        if (d.kind != DimKind::Static) { return false; } // symbolic / dynamic ⇒ Unknown
        const u64 e = static_cast<u64>(d.extent);
        if (e != 0U && p > (u64_max / e)) { return false; } // overflow ⇒ Unknown
        p *= e;
    }
    out = p;
    return true;
}
} // namespace

BroadcastResult Context::shapes_broadcast(TypeId a, TypeId b) const noexcept
{
    const Type sa = type_of(a);
    const Type sb = type_of(b);
    if (sa.kind != TypeKind::Shape || sb.kind != TypeKind::Shape) { return BroadcastResult{ShapeCompat::Unknown, 0U}; }
    const usize   ra = sa.members.size();
    const usize   rb = sb.members.size();
    const usize   n  = ra > rb ? ra : rb;
    const DimInfo one{DimKind::Static, 1U, {}};
    bool          unknown = false;
    for (usize i = 0; i < n; ++i) // i = right-aligned position (0 = innermost); shorter rank pads with 1
    {
        const DimInfo    da = (i < ra) ? dim_info(*this, sa.members[ra - 1U - i]) : one;
        const DimInfo    db = (i < rb) ? dim_info(*this, sb.members[rb - 1U - i]) : one;
        const ShapeCompat c = broadcast_dim(da, db);
        if (c == ShapeCompat::Incompatible) { return BroadcastResult{ShapeCompat::Incompatible, static_cast<u32>(i)}; }
        if (c == ShapeCompat::Unknown) { unknown = true; }
    }
    return BroadcastResult{unknown ? ShapeCompat::Unknown : ShapeCompat::Compatible, 0U};
}

ShapeCompat Context::shapes_reshape(TypeId a, TypeId b) const noexcept
{
    if (a == b) { return ShapeCompat::Compatible; } // structurally identical
    const Type sa = type_of(a);
    const Type sb = type_of(b);
    if (sa.kind != TypeKind::Shape || sb.kind != TypeKind::Shape) { return ShapeCompat::Unknown; }
    u64 pa = 1U;
    u64 pb = 1U;
    if (!static_extent_product(*this, sa, pa) || !static_extent_product(*this, sb, pb)) { return ShapeCompat::Unknown; }
    return pa == pb ? ShapeCompat::Compatible : ShapeCompat::Incompatible;
}

// ── Physical quantities (CEIR-3e, §17/§18) ──
bool Context::quantity_composition_valid(TypeId underlying) const noexcept
{
    if (!underlying.valid()) { return false; }
    switch (type_of(underlying).kind) // a quantity tags a NUMERIC value type (or a generic param — a generic quantity)
    {
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Index:
    case TypeKind::Vector:
    case TypeKind::Matrix:
    case TypeKind::Complex:
    case TypeKind::Quaternion:
    case TypeKind::TypeParam:
        return true;
    default: // ⛔ not Quantity (no nested tags), not Dim/Shape/resource/aggregate/callable/trait (section 18 "don't over-tag")
        return false;
    }
}

TypeId Context::type_quantity(TypeId underlying, const QuantityDim& dim)
{
    CRD_ASSERT_MSG(quantity_composition_valid(underlying), "type_quantity: underlying must be a numeric value type");
    Type         t    = Type::scalar(TypeKind::Quantity);
    t.count           = pack_dim_count(dim);
    t.cols            = pack_dim_cols(dim);
    const TypeId m[1] = {underlying};
    t.members         = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

QuantityDim Context::quantity_dim_of(TypeId quantity) const noexcept
{
    const Type t = type_of(quantity); // meaningful for a Quantity; unpacks its count/cols
    return unpack_dim(t.count, t.cols);
}

DimMismatch Context::quantity_dimensions_equal(TypeId a, TypeId b) const noexcept
{
    return quantity_dims_equal(quantity_dim_of(a), quantity_dim_of(b));
}

// ── Ownership / lifetime qualifiers + escape analysis (CEIR-3f, §19) ──
bool Context::qualified_composition_valid(TypeId underlying) const noexcept
{
    if (!underlying.valid()) { return false; }
    const TypeKind k = type_of(underlying).kind;
    // ⛔ no double-qualify; not structural machinery (Dim/Shape) or a contract (Trait). Everything else — incl. a generic
    // param and resources (own/borrow of a buffer is the §19 point) — is qualifiable.
    return k != TypeKind::Qualified && k != TypeKind::Dim && k != TypeKind::Shape && k != TypeKind::Trait;
}

TypeId Context::type_qualified(OwnershipKind kind, TypeId underlying)
{
    CRD_ASSERT_MSG(qualified_composition_valid(underlying), "type_qualified: cannot qualify this type");
    Type         t    = Type::scalar(TypeKind::Qualified);
    t.count           = static_cast<u32>(kind);
    const TypeId m[1] = {underlying};
    t.members         = containers::ConstSpan<TypeId>(m, 1U);
    return intern_type(t);
}

namespace
{
// A region CONTAINS another iff `r` is `ancestor` or nested within it (walk up via the owning op's region).
[[nodiscard]] bool region_contains(const Region* ancestor, const Region* r) noexcept
{
    for (const Region* cur = r; cur != nullptr;)
    {
        if (cur == ancestor) { return true; }
        const Operation* const op = cur->parent_op();
        const Block* const     b  = (op != nullptr) ? op->parent_block() : nullptr;
        cur                       = (b != nullptr) ? b->parent_region() : nullptr;
    }
    return false;
}
} // namespace

const Operation* Context::first_escaping_use(const Value* v, const Region* defining) const noexcept
{
    if (v == nullptr || defining == nullptr) { return nullptr; }
    for (const Use* u = v->first_use(); u != nullptr; u = u->next)
    {
        const Operation* const user = u->owner;
        const Block* const     b    = (user != nullptr) ? user->parent_block() : nullptr;
        const Region* const    ur   = (b != nullptr) ? b->parent_region() : nullptr;
        if (ur == nullptr || !region_contains(defining, ur)) { return user; } // a use outside defining's subtree
    }
    return nullptr;
}

bool Context::value_escapes_region(const Value* v, const Region* defining) const noexcept
{
    return first_escaping_use(v, defining) != nullptr;
}

namespace
{
// A borrowed-view value: typed `!qual<borrow,_>` (a Qualified whose ownership category is BorrowedView).
[[nodiscard]] bool is_borrowed_view(const Context& ctx, TypeId t) noexcept
{
    if (!t.valid()) { return false; }
    const Type ty = ctx.type_of(t);
    return ty.kind == TypeKind::Qualified && ty.count == static_cast<u32>(OwnershipKind::BorrowedView);
}

// Pre-order scan of `r` (block args, then each op's results, then recurse into op regions — the printer's SSA-numbering
// order, so the FIRST offender is stable). Every value defined directly in `r` has `r` as its defining region.
[[nodiscard]] BorrowEscape scan_region_for_borrow_escape(const Context& ctx, Region* r) noexcept
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (u32 i = 0; i < b->num_args(); ++i)
        {
            Value* const a = b->arg(i);
            if (is_borrowed_view(ctx, a->type()))
            {
                if (const Operation* const use = ctx.first_escaping_use(a, r)) { return {a, use}; }
            }
        }
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            for (u32 i = 0; i < op->num_results(); ++i)
            {
                Value* const res = op->result(i);
                if (is_borrowed_view(ctx, res->type()))
                {
                    if (const Operation* const use = ctx.first_escaping_use(res, r)) { return {res, use}; }
                }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const BorrowEscape e = scan_region_for_borrow_escape(ctx, op->region(i));
                if (e.value != nullptr) { return e; }
            }
        }
    }
    return {};
}
} // namespace

BorrowEscape Context::find_borrowed_escape(const Module& m) const noexcept
{
    return scan_region_for_borrow_escape(*this, m.body());
}

// ── §27/§28 mode-contract enforcement (CEIR-4b) ──
namespace
{
// Pre-order scan for the FIRST op that violates `mode`'s contract: (1) its determinism class fails the mode (§27 — an
// Unspecified/unregistered op fails any mode stricter than Normal: you cannot certify what was never classified), or
// (2) its `numerics` attr is CORRUPT (a violation in EVERY mode, Normal included — malformed data is not a legal-knob
// question), or (3) its per-instance numerics are legal-but-forbidden by the mode (§28, e.g. fast_math under Certified).
[[nodiscard]] const Operation* scan_region_for_mode(const Context& ctx, Region* r, CompilerMode mode) noexcept
{
    if (r == nullptr) { return nullptr; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (!determinism_satisfies_mode(ctx.op_determinism(op->kind()), mode)) { return op; }
            NumericalSemantics ns;
            if (!ctx.op_numerics(*op, ns)) { return op; }              // corrupt numerics attr — violates ANY mode
            if (!numerics_satisfies_mode(ns, mode)) { return op; }     // legal knob the active mode forbids
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                if (const Operation* const v = scan_region_for_mode(ctx, op->region(i), mode)) { return v; }
            }
        }
    }
    return nullptr;
}
} // namespace

const Operation* Context::find_mode_violation(const Module& m) const noexcept
{
    return scan_region_for_mode(*this, m.body(), m_compiler_mode);
}

void Context::set_numerics(Operation* op, const NumericalSemantics& n)
{
    set_attr(op, "numerics", attr_int(pack_numerics(n))); // rides the existing int-attr machinery — no new surface
}

bool Context::op_numerics(const Operation& op, NumericalSemantics& out) const noexcept
{
    const AttrId id = op.attr("numerics");
    if (!id.valid())
    {
        out = NumericalSemantics{}; // absent ⇒ every knob Inherit (the default)
        return true;
    }
    const AttrValue v = attr_value(id);
    if (v.kind != AttrKind::Int) { return false; } // wrong kind stored under "numerics" ⇒ corrupt
    return unpack_numerics(v.i, out);              // false iff a packed field is out of range
}

// ── §15/§32 region execution tag + domain-legality (CEIR-4c) ──
void Context::set_region_exec(Operation* region_owner, const RegionExec& r)
{
    set_attr(region_owner, "region_exec", attr_int(pack_region_exec(r))); // rides the existing int-attr machinery
}

bool Context::op_region_exec(const Operation& op, RegionExec& out) const noexcept
{
    const AttrId id = op.attr("region_exec");
    if (!id.valid())
    {
        out = RegionExec{}; // absent ⇒ untagged (Unspecified/Unspecified)
        return true;
    }
    const AttrValue v = attr_value(id);
    if (v.kind != AttrKind::Int) { return false; }
    return unpack_region_exec(v.i, out);
}

// ── §34 callee-derived effects (CEIR-5c): the CEIR-4a EffectsFn landing ──
void Context::collect_effective_mask(const Operation& op, const EffectQuery& q, u64& mask) const
{
    const OpInfo* const info = op_info(op.kind());
    // An INSTANCE-dependent hook (func.call) OVERRIDES the static records — but only when we can resolve (a table is
    // present); it may still DECLINE (return false) and fall through to the static family below.
    if (info != nullptr && info->effects_fn != nullptr && q.symbols != nullptr)
    {
        if (info->effects_fn(*this, op, q, mask)) { return; }
    }
    if (info == nullptr)
    {
        mask |= effect_family_bit(EffectFamily::ExternalCall); // ⛔ unregistered = opaque barrier (EMPTY≠UNKNOWN)
        return;
    }
    for (u32 i = 0; i < info->num_effects; ++i) { mask |= effect_family_bit(info->effects[i].family); }
}

void Context::collect_region_effective_mask(const Region& r, const EffectQuery& q, u64& mask) const
{
    for (Block* b = r.first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            collect_effective_mask(*op, q, mask);      // the op's OWN kind-level contribution (a call dispatches its hook)
            for (u32 i = 0; i < op->num_regions(); ++i) // + its NESTED regions' contents (a call inside a core.if counts)
            {
                Region* const sub = op->region(i);
                if (sub != nullptr) { collect_region_effective_mask(*sub, q, mask); }
            }
        }
    }
}

void Context::effective_effects(const Operation& op, const SymbolTable& table, containers::Array<EffectRecord>& out) const
{
    containers::HashMap<const Operation*, u8> visited(allocator()); // the recursion cycle guard (per query)
    const EffectQuery                         q{&table, &visited};
    u64                                       mask = 0U;
    collect_effective_mask(op, q, mask);
    // Emit one AMBIENT record per set family bit (ascending §26 ordinal — deterministic). Resource identity is dropped
    // to whole-class at the call boundary (a callee's operand/result target is meaningless at the call site).
    for (u32 f = 0; f <= static_cast<u32>(kLastEffectFamily); ++f)
    {
        const auto fam = static_cast<EffectFamily>(f);
        if ((mask & effect_family_bit(fam)) != 0U) { out.push_back(EffectRecord{fam, EffectTarget::None, 0U, 0U}); }
    }
}

namespace
{
// Pre-order walk carrying the EFFECTIVE region tag (INNERMOST-wins) + the op that set it. Returns the FIRST op whose
// EFFECTIVE §26 effects (CEIR-5c: a `func.call` resolves to its callee's set via `table`; an UNREGISTERED op is the
// ExternalCall barrier; else its static families) are illegal under that tag; a corrupt `region_exec` on a region-owner
// is itself a violation at the owner.
[[nodiscard]] DomainViolation scan_region_for_domain(const Context& ctx, const SymbolTable& table, Region* r,
                                                     const RegionExec& tag, const Operation* tag_owner)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            // the op's effective families (a call's are its callee's — the EffectsFn hook resolving through `table`).
            // ⛔ ALWAYS computed (no table-present guard): an empty set here would silently no-op the whole §32 verifier.
            containers::Array<EffectRecord> eff(ctx.allocator());
            ctx.effective_effects(*op, table, eff);
            for (u32 i = 0; i < static_cast<u32>(eff.size()); ++i)
            {
                if (!effect_legal_in_region(eff[i].family, tag))
                {
                    return {op, tag_owner, eff[i].family, ctx.op_info(op->kind()) == nullptr};
                }
            }
            // innermost-wins: if THIS op declares a region_exec, its tag replaces the inherited one for its subtree.
            RegionExec       inner       = tag;
            const Operation* inner_owner = tag_owner;
            if (op->attr("region_exec").valid())
            {
                RegionExec own;
                if (!ctx.op_region_exec(*op, own)) { return {op, op, EffectFamily::MemoryRead, false}; } // corrupt tag
                inner       = own;
                inner_owner = op;
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const DomainViolation v = scan_region_for_domain(ctx, table, op->region(i), inner, inner_owner);
                if (v.op != nullptr) { return v; }
            }
        }
    }
    return {};
}
} // namespace

DomainViolation Context::find_domain_violation(const Module& m) const
{
    // resolve calls against the module's own symbol table (single-module scope — cross-module refinement is later).
    const SymbolTable* const syms = m.symbols();
    CRD_ASSERT_MSG(syms != nullptr, "find_domain_violation: module has no symbol table");
    return scan_region_for_domain(*this, *syms, m.body(), RegionExec{}, nullptr);
}

// ── §26/§116 effect-derived ordering hazards (CEIR-4d) ──
namespace
{
// One op effect resolved to a concrete resource: its class + access + the SSA Value it touches (null ⇒ ambient/whole
// class) + its range mask.
struct ResolvedAccess
{
    ResourceClass klass;
    bool          reads;
    bool          writes;
    const Value*  resource;
    u32           mask;
};

// Accesses an op contributes: an UNREGISTERED op = ONE synthetic Universe read+write (maximally effectful, EMPTY≠UNKNOWN);
// a registered op = one per declared effect (a registered EFFECT-FREE op contributes zero ⇒ never hazards).
[[nodiscard]] u32 op_access_count(const Context& ctx, const Operation& op) noexcept
{
    const OpInfo* const info = ctx.op_info(op.kind());
    return info != nullptr ? info->num_effects : 1U;
}
[[nodiscard]] ResolvedAccess op_access_at(const Context& ctx, const Operation& op, u32 i) noexcept
{
    const OpInfo* const info = ctx.op_info(op.kind());
    if (info == nullptr) { return {ResourceClass::Universe, true, true, nullptr, 0U}; } // unknown ⇒ maximally effectful
    const EffectRecord& e = info->effects[i];
    const EffectAccess  a = effect_access(e.family); // read/write is the FAMILY's; the class may be the LOCATION's (8c)
    // ambient (target None) ⇒ whole class; an OUT-OF-RANGE index on a malformed instance also degrades to whole-class
    // (nullptr) — the CONSERVATIVE direction (more hazards, never fewer). CEIR-8c: the built-in location kinds beyond
    // Operand/Result (BufferRange..Net, Extern, EcsComponent, …) carry no per-instance location identity, so they resolve
    // to whole-class here too — a DELIBERATE conservative fallback (safe-but-pessimal), NOT a bug. Per-instance location
    // identity is an unbound FUTURE refinement (⛔ 8d delivered per-OP stable identity, NOT per-location identity — this
    // is not that; do not bind it to a band until an owning slice earns it). Precise per-resource hazards are available
    // TODAY by targeting the resource's SSA Value via `EffectTarget::Operand`/`Result` (the CEIR-9f ECS proof).
    const Value* res = nullptr;
    if (e.target == EffectTarget::Operand && e.index < op.num_operands()) { res = op.operand(e.index); }
    else if (e.target == EffectTarget::Result && e.index < op.num_results()) { res = op.result(e.index); }
    // ⭐ CEIR-13d part 3: NORMALIZE the resource to its view-ROOT, so `write(%buf)` vs `read(view(%buf))` now conflicts (the
    // 12c false-negative, struck below). Identity only — the view's byte range is not tracked here (conservative-safe).
    res = ctx.resource_root(res);
    // CEIR-8c: an Extern location's class comes from its registered descriptor (Universe if UNREGISTERED — EMPTY≠UNKNOWN,
    // maximally conflicting); every other target uses the family's class. `effect_resource_class` encapsulates the rule.
    return {ctx.effect_resource_class(e), a.reads, a.writes, res, e.range_mask};
}
// Two accesses conflict iff both touch a resource, ≥1 writes, the resources overlap (Universe on either side, or the same
// class with aliasing Values — where a null Value is the whole class), and the ranges overlap. ⛔ distinct Values are
// assumed non-aliasing — but op_access_at now NORMALIZES each resource to its view-ROOT (resource_root), so distinct views
// of ONE buffer DO conflict (CEIR-13d part 3 closed the 12c false-negative). A view laundered through a yield/call still
// escapes (its root is the yield/call result, not the buffer) — a deeper alias-model hole.
[[nodiscard]] bool accesses_conflict(const ResolvedAccess& a, const ResolvedAccess& b) noexcept
{
    if (!(a.reads || a.writes) || !(b.reads || b.writes)) { return false; } // an inert access touches nothing
    if (!(a.writes || b.writes)) { return false; }                          // read-read: no ordering needed
    const bool class_ov = a.klass == ResourceClass::Universe || b.klass == ResourceClass::Universe ||
                          (a.klass == b.klass &&
                           (a.resource == b.resource || a.resource == nullptr || b.resource == nullptr));
    return class_ov && range_overlap(a.mask, b.mask);
}
// The hazard kind for a conflicting pair, `a` BEFORE `b` (WAW > RAW > WAR).
[[nodiscard]] HazardKind pair_hazard(const ResolvedAccess& a, const ResolvedAccess& b) noexcept
{
    if (a.writes && b.writes) { return HazardKind::Waw; }
    if (a.writes && b.reads) { return HazardKind::Raw; }
    if (a.reads && b.writes) { return HazardKind::War; }
    return HazardKind::None;
}
// The TABLE-aware per-op access list (CEIR-5c): an op with an EffectsFn hook (a `func.call`) contributes its callee-
// DERIVED families as AMBIENT whole-class accesses; every other op keeps its PRECISE static per-Value accesses (so the
// table overload never LOSES precision for non-calls — it only ADDS it for calls).
void gather_accesses(const Context& ctx, const Operation& op, const SymbolTable& table,
                     containers::Array<ResolvedAccess>& out)
{
    const OpInfo* const info = ctx.op_info(op.kind());
    if (info != nullptr && info->effects_fn != nullptr)
    {
        containers::Array<EffectRecord> eff(ctx.allocator());
        ctx.effective_effects(op, table, eff); // the callee-derived family set (ambient records)
        for (u32 i = 0; i < static_cast<u32>(eff.size()); ++i)
        {
            const EffectAccess a = effect_access(eff[i].family);
            out.push_back({a.klass, a.reads, a.writes, nullptr, 0U}); // ambient ⇒ whole-class, whole-range
        }
        return;
    }
    const u32 n = op_access_count(ctx, op); // static path — precise per-Value identity (unchanged 4d behaviour)
    for (u32 i = 0; i < n; ++i) { out.push_back(op_access_at(ctx, op, i)); }
}
// The shared pairwise-conflict core: the strongest hazard (WAW>RAW>WAR) over `before`'s × `after`'s accesses.
[[nodiscard]] HazardKind strongest_hazard(const containers::Array<ResolvedAccess>& a,
                                          const containers::Array<ResolvedAccess>& b) noexcept
{
    HazardKind strongest = HazardKind::None;
    for (u32 i = 0; i < static_cast<u32>(a.size()); ++i)
    {
        for (u32 j = 0; j < static_cast<u32>(b.size()); ++j)
        {
            if (accesses_conflict(a[i], b[j]))
            {
                const HazardKind k = pair_hazard(a[i], b[j]);
                if (hazard_rank(k) > hazard_rank(strongest)) { strongest = k; }
            }
        }
    }
    return strongest;
}
} // namespace

// ⭐ CEIR-13d part 3 (§78/§116): follow `resource.view` chains to the underlying resource, so a view and its buffer NAME the
// same Value in the hazard walk (op_access_at normalizes every captured resource through here). ONE hop per view op (12a: a
// view's operand(0) is the source resource), loop-guarded against a malformed cycle. Returns `v` unchanged when it is not a
// view (or a block arg — `defining_op()==nullptr). ⛔ IDENTITY only: the view's byte RANGE is not resolved (disjoint views of
// one buffer still collapse to the root ⇒ conservative over-conflict, refinable). ⛔ a view laundered through a region
// yield / call result is NOT chased (its `defining_op` is the yield/call, not the view) — a deeper alias hole (D-007 §116).
const Value* Context::resource_root(const Value* v) const noexcept
{
    for (u32 guard = 0U; v != nullptr && guard < 64U; ++guard)
    {
        const Operation* const def = v->defining_op();
        if (def == nullptr) { break; }
        if (op_name(def->kind()) != containers::StringView("resource.view")) { break; }
        if (def->num_operands() < 1U) { break; }
        v = def->operand(0U);
    }
    return v;
}

HazardKind Context::ops_hazard(const Operation& before, const Operation& after) const noexcept
{
    HazardKind      strongest = HazardKind::None;
    const u32       na        = op_access_count(*this, before);
    const u32       nb        = op_access_count(*this, after);
    for (u32 i = 0; i < na; ++i)
    {
        const ResolvedAccess a = op_access_at(*this, before, i);
        for (u32 j = 0; j < nb; ++j)
        {
            const ResolvedAccess b = op_access_at(*this, after, j);
            if (accesses_conflict(a, b))
            {
                const HazardKind k = pair_hazard(a, b);
                if (hazard_rank(k) > hazard_rank(strongest)) { strongest = k; }
            }
        }
    }
    return strongest;
}

void Context::collect_block_hazards(const Block& b, containers::Array<Hazard>& out) const
{
    // every ordered pair (op a BEFORE op c) in this block's LIST order; each non-None edge is a scheduler constraint.
    for (const Operation* a = b.first_op(); a != nullptr; a = a->next_in_block())
    {
        for (const Operation* c = a->next_in_block(); c != nullptr; c = c->next_in_block())
        {
            const HazardKind k = ops_hazard(*a, *c);
            if (k != HazardKind::None) { out.push_back(Hazard{a, c, k}); }
        }
    }
}

// CEIR-5c: the TABLE-taking overloads use a call's callee-DERIVED effects (a call to a pure func is reorderable; a call
// to a memory-writer hazards). Same conflict core as the no-table baseline — only the per-op effect-fetch differs.
HazardKind Context::ops_hazard(const Operation& before, const Operation& after, const SymbolTable& table) const
{
    containers::Array<ResolvedAccess> a(allocator());
    containers::Array<ResolvedAccess> b(allocator());
    gather_accesses(*this, before, table, a);
    gather_accesses(*this, after, table, b);
    return strongest_hazard(a, b);
}

void Context::collect_block_hazards(const Block& b, const SymbolTable& table, containers::Array<Hazard>& out) const
{
    for (const Operation* a = b.first_op(); a != nullptr; a = a->next_in_block())
    {
        for (const Operation* c = a->next_in_block(); c != nullptr; c = c->next_in_block())
        {
            const HazardKind k = ops_hazard(*a, *c, table);
            if (k != HazardKind::None) { out.push_back(Hazard{a, c, k}); }
        }
    }
}

// ── CEIR-12c §78: the resource ALIAS/LIFETIME analysis (compute_block_lifetimes + the interference/may-alias predicates) ──
// The effect-hazard model above (CEIR-4d) yields ORDERING; this yields per-resource LIVE RANGES the memory planner
// (CEIR-12d) colors. Ports the frame graph's greedy interval model. Reuses the anon-namespace op_access_* helpers above for
// the "over 4d effects" rule (an ambient Memory/Universe touch conservatively extends every prior resource's range).
namespace
{
constexpr u32 kNoRoot = ~0U; // "this Value is not a tracked graph-owned resource (nor a view of one)"

// A Value* → root-resource-index entry: a resource.declare's result maps to its own index; a resource.view's result maps
// to the ROOT of its operand(0) (so a use of a view is a use of the underlying resource — the frame-graph lifetime rule).
struct RootEntry
{
    const Value* v   = nullptr;
    u32          idx = 0;
};
[[nodiscard]] u32 root_lookup(const RootEntry* m, u32 n, const Value* v) noexcept
{
    for (u32 i = 0; i < n; ++i)
    {
        if (m[i].v == v) { return m[i].idx; }
    }
    return kNoRoot;
}
// the §20 lifetime class from the declare's `lifetime` attr (⛔ ABSENT or unrecognized ⇒ Unspecified — find_resource_intent_
// misuse rejects bad values before analysis, but map defensively; Unspecified is NOT aliasable, the conservative direction).
[[nodiscard]] ResourceLifetimeClass read_lifetime_class(const Context& ctx, const Operation* op) noexcept
{
    const AttrId a = op->attr(containers::StringView("lifetime"));
    if (!a.valid()) { return ResourceLifetimeClass::Unspecified; }
    const AttrValue v = ctx.attr_value(a);
    if (v.kind != AttrKind::String) { return ResourceLifetimeClass::Unspecified; }
    if (v.s == containers::StringView("transient")) { return ResourceLifetimeClass::Transient; }
    if (v.s == containers::StringView("persistent")) { return ResourceLifetimeClass::Persistent; }
    if (v.s == containers::StringView("history")) { return ResourceLifetimeClass::History; }
    return ResourceLifetimeClass::Unspecified;
}
[[nodiscard]] i64 read_size_class(const Context& ctx, const Operation* op) noexcept
{
    const AttrId a = op->attr(containers::StringView("size_class"));
    if (!a.valid()) { return 0; }
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::Int ? v.i : 0;
}
[[nodiscard]] bool op_has_ambient_mem_or_universe(const Context& ctx, const Operation& op) noexcept
{
    const u32 n = op_access_count(ctx, op);
    for (u32 i = 0; i < n; ++i)
    {
        const ResolvedAccess a = op_access_at(ctx, op, i); // ambient ⇒ resource == nullptr (whole class)
        if (a.resource == nullptr && (a.reads || a.writes) &&
            (a.klass == ResourceClass::Memory || a.klass == ResourceClass::Universe))
        {
            return true;
        }
    }
    return false;
}
// Visit `op` and its nested regions (recursively), ALL attributed to the CONTAINING top-level `pos` (the "pass"): extend
// every used resource's `last` to `pos`, mark an exported root, and REPORT whether any op in the tree has an ambient
// Memory/Universe effect (the caller applies the at-or-before extension once, at `pos`). ⛔ nested-region uses/exports/
// effects are REAL (produce-outside/consume-inside): wrapping an op in a region must NEVER weaken the analysis — every
// check that fires on a top-level op fires on a nested one too.
[[nodiscard]] bool visit_uses_and_effects(const Context& ctx, const Operation* op, const RootEntry* m, u32 nm,
                                          containers::Array<ResourceLifetime>& out, u32 pos) // NOLINT(misc-no-recursion)
{
    for (u32 i = 0; i < op->num_operands(); ++i)
    {
        const u32 r = root_lookup(m, nm, op->operand(i));
        if (r != kNoRoot && out[r].last < pos) { out[r].last = pos; }
    }
    if (ctx.op_name(op->kind()) == containers::StringView("resource.export") && op->num_operands() >= 1U)
    {
        const u32 r = root_lookup(m, nm, op->operand(0U));
        if (r != kNoRoot) { out[r].exported = true; }
    }
    bool ambient = op_has_ambient_mem_or_universe(ctx, *op);
    for (u32 rg = 0; rg < op->num_regions(); ++rg)
    {
        for (const Block* bb = op->region(rg)->first_block(); bb != nullptr; bb = bb->next_in_region())
        {
            for (const Operation* inner = bb->first_op(); inner != nullptr; inner = inner->next_in_block())
            {
                const bool sub = visit_uses_and_effects(ctx, inner, m, nm, out, pos); // ⛔ ALWAYS recurse (side effects) then OR
                ambient        = ambient || sub;
            }
        }
    }
    return ambient;
}
} // namespace

void Context::compute_block_lifetimes(const Block& b, containers::Array<ResourceLifetime>& out) const
{
    // Pass 1 — positions; collect graph-owned resources (resource.declare) into `out`; build the Value*→root map (declares
    // map to self, resource.view maps to operand(0)'s root). ⛔ resource.import is EXCLUDED — the planner never plans it.
    containers::Array<RootEntry> vmap(allocator());
    u32                          pos = 0;
    for (const Operation* op = b.first_op(); op != nullptr; op = op->next_in_block(), ++pos)
    {
        const containers::StringView nm = op_name(op->kind());
        if (nm == containers::StringView("resource.declare") && op->num_results() >= 1U)
        {
            const Value*     r = op->result(0U);
            ResourceLifetime lt;
            lt.resource   = r;
            lt.declare    = op;
            lt.first      = pos;
            lt.last       = pos;
            lt.lifetime   = read_lifetime_class(*this, op);
            lt.kind       = type_of(r->type()).kind;
            lt.size_class = read_size_class(*this, op);
            const u32 idx = static_cast<u32>(out.size());
            out.push_back(lt);
            vmap.push_back(RootEntry{r, idx});
        }
        else if (nm == containers::StringView("resource.view") && op->num_results() >= 1U && op->num_operands() >= 1U)
        {
            const u32 root = root_lookup(vmap.data(), static_cast<u32>(vmap.size()), op->operand(0U));
            if (root != kNoRoot) { vmap.push_back(RootEntry{op->result(0U), root}); }
        }
    }
    const u32 num_ops = pos;
    if (out.empty()) { return; }
    const RootEntry* const m  = vmap.data();
    const u32              nm = static_cast<u32>(vmap.size());

    // Pass 2 — uses. For each op: extend the ranges of the resources it (or its nested regions) use as operands; mark
    // exported resources; and — the "over 4d effects" rule — let an ambient Memory/Universe touch extend every resource
    // declared at-or-before it (conservative: such an op may alias any live resource).
    pos = 0;
    for (const Operation* op = b.first_op(); op != nullptr; op = op->next_in_block(), ++pos)
    {
        // one recursive walk handles operand uses + export marking + ambient detection for this op AND its nested regions.
        const bool ambient = visit_uses_and_effects(*this, op, m, nm, out, pos);
        if (ambient) // an ambient Memory/Universe touch anywhere under this pass extends every resource declared at-or-before it
        {
            for (u32 i = 0; i < static_cast<u32>(out.size()); ++i)
            {
                if (out[i].first <= pos && out[i].last < pos) { out[i].last = pos; }
            }
        }
    }

    // Pass 3 — pin exported resources to block-END: external code may touch a published resource past any op position.
    const u32 endpos = num_ops == 0U ? 0U : num_ops - 1U;
    for (u32 i = 0; i < static_cast<u32>(out.size()); ++i)
    {
        if (out[i].exported && out[i].last < endpos) { out[i].last = endpos; }
    }
}

bool Context::resources_interfere(const ResourceLifetime& a, const ResourceLifetime& b) noexcept
{
    return !(a.last < b.first || b.last < a.first); // [first,last] closed intervals overlap
}

bool Context::resources_may_alias(const ResourceLifetime& a, const ResourceLifetime& b) noexcept
{
    if (a.resource == b.resource) { return false; }                             // a resource never aliases itself
    if (a.lifetime != ResourceLifetimeClass::Transient || b.lifetime != ResourceLifetimeClass::Transient) { return false; }
    if (a.exported || b.exported) { return false; }                             // a published resource is never poolable
    // same resource KIND + same NON-ZERO size_class bucket. ⛔ size_class 0 (unspecified) never pools — same reasoning as
    // unspecified lifetime: aliasing a resource of unknown size into a same-unknown slot is a correctness gamble, refusing
    // is only a pessimization (12d relaxes per profile once real sizes bind).
    if (a.kind != b.kind || a.size_class == 0 || a.size_class != b.size_class) { return false; }
    return !resources_interfere(a, b);
}

// ── CEIR-12d §78/§162: the memory PLANNER (plan_block_memory) — the greedy interval-coloring port + the inspectable plan ──
containers::StringView plan_profile_name(PlanProfile p) noexcept
{
    switch (p)
    {
    case PlanProfile::Memory: return containers::StringView("memory");
    case PlanProfile::Balanced: return containers::StringView("balanced");
    case PlanProfile::Latency: return containers::StringView("latency");
    case PlanProfile::Deterministic: return containers::StringView("deterministic");
    }
    return containers::StringView("?");
}
containers::StringView slot_reason_name(SlotReason r) noexcept
{
    switch (r)
    {
    case SlotReason::Pooled: return containers::StringView("pooled");
    case SlotReason::NewPoolSlot: return containers::StringView("new-pool-slot");
    case SlotReason::DedicatedLifetime: return containers::StringView("dedicated-lifetime");
    case SlotReason::DedicatedExported: return containers::StringView("dedicated-exported");
    case SlotReason::DedicatedUnsized: return containers::StringView("dedicated-unsized");
    case SlotReason::DedicatedProfile: return containers::StringView("dedicated-profile");
    }
    return containers::StringView("?");
}

void Context::plan_block_memory(const Block& b, PlanProfile profile, MemoryPlan& out) const
{
    out.slots.clear();
    out.assignments.clear();
    out.transient_logical  = 0;
    out.transient_physical = 0;
    out.profile            = profile;

    containers::Array<ResourceLifetime> lts(allocator());
    compute_block_lifetimes(b, lts);
    const u32 n = static_cast<u32>(lts.size());
    if (n == 0U) { return; }

    for (u32 i = 0; i < n; ++i) // assignments parallel to lts (declaration order); slot/reason filled below
    {
        out.assignments.push_back(SlotAssignment{lts[i].resource, 0U, SlotReason::DedicatedLifetime, nullptr});
    }
    containers::Array<const Value*> slot_last_occ(allocator()); // parallel to out.slots — the current end-occupant per slot
    const bool                      aliasing = profile != PlanProfile::Latency; // Memory/Balanced/Deterministic pool; Latency does not

    // Pass A — dedicated slots in DECLARATION order: every non-poolable-eligible resource + (under Latency) every eligible
    // one. A poolable-eligible resource under an aliasing profile is DEFERRED to the interval-coloring pass.
    for (u32 i = 0; i < n; ++i)
    {
        const ResourceLifetime& r        = lts[i];
        const bool eligible = r.lifetime == ResourceLifetimeClass::Transient && !r.exported && r.size_class != 0;
        if (eligible && aliasing) { continue; }
        SlotReason reason = SlotReason::DedicatedLifetime;
        if (r.lifetime != ResourceLifetimeClass::Transient) { reason = SlotReason::DedicatedLifetime; }
        else if (r.exported) { reason = SlotReason::DedicatedExported; }
        else if (r.size_class == 0) { reason = SlotReason::DedicatedUnsized; }
        else { reason = SlotReason::DedicatedProfile; } // eligible but the Latency profile refused to pool it
        // §162: a history<T> ring's depth = its memory MULTIPLE. ⛔ 12b pins "absent under lifetime=history means 1" (the
        // TAA prev-frame case), so History DEFAULTS to depth 1; a present, valid history_length overrides.
        i64 hlen = r.lifetime == ResourceLifetimeClass::History ? 1 : 0;
        if (r.lifetime == ResourceLifetimeClass::History)
        {
            const AttrId a = r.declare->attr(containers::StringView("history_length"));
            if (a.valid())
            {
                const AttrValue v = attr_value(a);
                if (v.kind == AttrKind::Int && v.i > 0) { hlen = v.i; }
            }
        }
        const u32 s = static_cast<u32>(out.slots.size());
        out.slots.push_back(MemorySlot{r.kind, r.size_class, true, r.first, r.last, 1U, hlen});
        slot_last_occ.push_back(r.resource);
        out.assignments[i].slot   = s;
        out.assignments[i].reason = reason;
        if (eligible) { ++out.transient_logical; ++out.transient_physical; } // DedicatedProfile: own slot, counts in both
    }

    if (!aliasing) { return; } // Latency: no interval-coloring pass

    // Pass B — interval-color the poolable-eligible resources in (first asc, decl-index asc) order. First-fit on a
    // start-sorted stream is provably minimal (χ = max concurrent live). ⛔ today lts is ALREADY start-sorted by
    // construction (12c `first` = the declare position, appended in walk order), so this sort is a GUARD for a future
    // first-USE semantics — NOT a claimed improvement over the frame-graph reference.
    containers::Array<u32> order(allocator());
    for (u32 i = 0; i < n; ++i)
    {
        const ResourceLifetime& r = lts[i];
        if (r.lifetime == ResourceLifetimeClass::Transient && !r.exported && r.size_class != 0) { order.push_back(i); }
    }
    for (u32 a = 1; a < static_cast<u32>(order.size()); ++a) // stable insertion sort by `first`
    {
        const u32 key = order[a];
        u32       j   = a;
        while (j > 0U && lts[order[j - 1U]].first > lts[key].first) { order[j] = order[j - 1U]; --j; }
        order[j] = key;
    }
    const u32 no_slot = ~0U;
    for (u32 oi = 0; oi < static_cast<u32>(order.size()); ++oi)
    {
        const u32               idx = order[oi];
        const ResourceLifetime& r   = lts[idx];
        ++out.transient_logical;
        u32 chosen = no_slot; // first non-dedicated same-bucket slot whose end precedes this resource's first (disjoint)
        for (u32 s = 0; s < static_cast<u32>(out.slots.size()); ++s)
        {
            const MemorySlot& sl = out.slots[s];
            if (!sl.dedicated && sl.kind == r.kind && sl.size_class == r.size_class && sl.last < r.first)
            {
                chosen = s;
                break;
            }
        }
        if (chosen != no_slot)
        {
            MemorySlot& sl              = out.slots[chosen];
            out.assignments[idx].slot   = chosen;
            out.assignments[idx].reason = SlotReason::Pooled;
            out.assignments[idx].prior  = slot_last_occ[chosen];
            if (r.first < sl.first) { sl.first = r.first; }
            sl.last = r.last;
            ++sl.occupant_count;
            slot_last_occ[chosen] = r.resource;
        }
        else
        {
            const u32 s = static_cast<u32>(out.slots.size());
            out.slots.push_back(MemorySlot{r.kind, r.size_class, false, r.first, r.last, 1U, 0});
            slot_last_occ.push_back(r.resource);
            out.assignments[idx].slot   = s;
            out.assignments[idx].reason = SlotReason::NewPoolSlot;
            ++out.transient_physical;
        }
    }
}

// ── CEIR-5a structured control flow: the constant-condition `if` fold ──
bool Context::fold_constant_if(Operation* if_op)
{
    if (if_op == nullptr || if_op->kind() != intern_op("core", "if")) { return false; }
    // ⛔ a region-tagged `if` (CEIR-4c region_exec) must NOT be inlined — that would silently delete the region's
    // domain/realtime constraint. Bail before any inspection of the branches.
    if (if_op->attr("region_exec").valid()) { return false; }
    if (if_op->num_operands() < 1U || if_op->num_regions() < 2U) { return false; }

    // the condition must be a CONSTANT arith.const carrying an integer `value` attribute.
    const Operation* const cond_def = if_op->operand(0)->defining_op();
    if (cond_def == nullptr || cond_def->kind() != intern_op("arith", "const")) { return false; }
    const AttrId cond_attr = cond_def->attr("value");
    if (!cond_attr.valid()) { return false; }
    const AttrValue cv = attr_value(cond_attr);
    if (cv.kind != AttrKind::Int) { return false; }

    Region* const  taken = (cv.i != 0) ? if_op->region(0) : if_op->region(1); // nonzero → THEN (region 0)
    Block* const   tb    = taken->first_block();
    if (tb == nullptr || tb->next_in_region() != nullptr) { return false; } // single-block regions only
    Operation* const term = tb->last_op();
    if (term == nullptr || term->kind() != intern_op("core", "yield")) { return false; } // must end with core.yield
    // ⛔ refuse to move an EXECUTION-CONTEXT boundary: if any op in the taken block carries a `region_exec` tag (CEIR-4c),
    // splicing it into the parent changes its enclosing context — bail (relaxing this is 5a-remaining, with the value-
    // producing fold). The 2nd instance of band-4 metadata making a band-5 rewrite unsound: a rewrite must AUDIT the
    // attributes it moves, not only the op it deletes.
    for (const Operation* inner = tb->first_op(); inner != nullptr; inner = inner->next_in_block())
    {
        if (inner->attr("region_exec").valid()) { return false; }
    }
    // ⛔ the yield's operand count MUST match the if's result count, or the RAUW below would index `term->operand(i)`
    // out of bounds — a UAF-shaped bug. Bail on a mismatch (a malformed program the 5b verifier will also reject).
    if (term->num_operands() != if_op->num_results()) { return false; }

    // splice every op of the taken block EXCEPT the yield into the parent block, before the `if`, preserving order.
    Block* const parent = if_op->parent_block();
    if (parent == nullptr) { return false; }
    Operation* cur = tb->first_op();
    while (cur != nullptr && cur != term)
    {
        Operation* const next = cur->next_in_block();
        tb->unlink(cur);
        parent->insert_before(cur, if_op);
        cur = next;
    }
    // VALUE FORWARDING: replace each of the if's results with the taken region's corresponding yielded value (now spliced
    // into the parent, so it dominates every former use of the if's result). RAUW leaves the if's results use-free.
    for (u32 i = 0; i < if_op->num_results(); ++i) { if_op->result(i)->replace_all_uses_with(term->operand(i)); }
    term->erase();   // the now-orphaned yield
    if_op->erase();  // drops the cond use; the non-taken region + emptied taken region leak into the arena (by design)
    return true;
}

// ── CEIR-5b the §115 STRUCTURE-layer verifier ──
containers::StringView structure_error_kind_name(StructureErrorKind k) noexcept
{
    switch (k) // ⛔ no default: a new kind must be named here (a `-Werror=switch` compile error otherwise)
    {
    case StructureErrorKind::None: return containers::StringView("none");
    case StructureErrorKind::UseBeforeDef: return containers::StringView("use-before-def");
    case StructureErrorKind::CaptureThroughIsolation: return containers::StringView("capture-through-isolation");
    case StructureErrorKind::MissingTerminator: return containers::StringView("missing-terminator");
    case StructureErrorKind::TerminatorNotLast: return containers::StringView("terminator-not-last");
    case StructureErrorKind::YieldCountMismatch: return containers::StringView("yield-count-mismatch");
    case StructureErrorKind::FeedbackWithoutState: return containers::StringView("feedback-without-state");
    case StructureErrorKind::StateDepthInvalid: return containers::StringView("state-depth-invalid");
    }
    return containers::StringView("?");
}

namespace
{
// One region-block's visibility scope: the values DEFINED here (block args + results of ops already passed), the
// enclosing scope, and whether the boundary into this scope was an `IsolatedFromAbove` one (which hides the parent).
struct DomScope
{
    containers::HashMap<const Value*, u8>* defs;
    const DomScope*                        parent;
    bool                                   isolated;
};
// Is `v` visible from `s`? Walk the scope chain; `respect_isolation=false` peers THROUGH isolation boundaries (used to
// tell CaptureThroughIsolation from a genuine UseBeforeDef).
[[nodiscard]] bool dom_visible(const Value* v, const DomScope* s, bool respect_isolation) noexcept
{
    for (const DomScope* cur = s; cur != nullptr; cur = cur->parent)
    {
        if (cur->defs->contains(v)) { return true; }
        if (respect_isolation && cur->isolated) { return false; } // this scope cannot see its parent
    }
    return false;
}
// True iff `op` belongs to the `core` structured dialect (the yield↔owner contract applies only to it).
[[nodiscard]] bool is_core_op(const Context& ctx, const Operation& op) noexcept
{
    const Dialect* const d = ctx.dialect_of(op.kind());
    return d != nullptr && d->name() == containers::StringView("core");
}
// The yield↔owner count contract for a `core` region owner's region `ri`: the terminating `core.yield` must have exactly
// `expected` operands (owner result count, or the table override — `core.while` cond region = 1). A 0-`expected` region
// may omit the yield (a statement region); a >0-`expected` region MUST yield.
[[nodiscard]] StructureError check_yield_count(const Context& ctx, Operation& owner, u32 ri) noexcept
{
    Region* const reg = owner.region(ri);
    Block* const  bb  = reg->first_block();
    u32           expected = owner.num_results();
    if (ctx.op_name(owner.kind()) == containers::StringView("core.while") && ri == 0U) { expected = 1U; } // the table
    Operation* const last = (bb != nullptr) ? bb->last_op() : nullptr;
    const bool is_yield   = last != nullptr && ctx.op_name(last->kind()) == containers::StringView("core.yield");
    if (is_yield)
    {
        if (last->num_operands() != expected) { return {last, nullptr, StructureErrorKind::YieldCountMismatch}; }
    }
    else if (expected != 0U)
    {
        return {&owner, nullptr, StructureErrorKind::YieldCountMismatch}; // results/loop-test declared but never yielded
    }
    return {};
}
[[nodiscard]] StructureError walk_region(const Context& ctx, Region* r, const DomScope* parent, bool isolated)
{
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        // terminator rule: an SsaCfg block MUST end with a Terminator-trait op (an empty block fails too).
        if (r->kind() == RegionKind::SsaCfg)
        {
            Operation* const last = b->last_op();
            if (last == nullptr || !ctx.has_trait(last->kind(), OpTrait::Terminator))
            {
                // Point at the offending op when there is one; for an EMPTY block there is none, so point at the
                // region's OWNER via the 3f `parent_op` back-link — a diagnostic must point at something (the 3z
                // house standard). (The outermost module body is Graph, never SsaCfg, so parent_op is non-null here.)
                const Operation* const at = (last != nullptr) ? last : r->parent_op();
                return {at, nullptr, StructureErrorKind::MissingTerminator};
            }
        }
        containers::HashMap<const Value*, u8> defs(ctx.allocator());
        for (u32 i = 0; i < b->num_args(); ++i) { defs.insert(b->arg(i), 1U); }
        // CEIR-5d: an ORDER-FREE set of every value defined in THIS block (args + all op results) — lets the dominance
        // check tell a same-block FEEDBACK edge (defined later HERE) from a genuine UseBeforeDef (defined in a later block
        // / never). Every def-use cycle has ≥1 same-block back-edge; a back-edge is legal ONLY as a StateEdge op's `next`
        // ⇒ every surviving cycle passes through a state op (§20), enforced by the existing walk — no separate SCC pass.
        containers::HashMap<const Value*, u8> all_defs(ctx.allocator());
        for (u32 i = 0; i < b->num_args(); ++i) { all_defs.insert(b->arg(i), 1U); }
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            for (u32 i = 0; i < op->num_results(); ++i) { all_defs.insert(op->result(i), 1U); }
        }
        const DomScope scope{&defs, parent, isolated};
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const bool state_edge = ctx.has_trait(op->kind(), OpTrait::StateEdge);
            // CEIR-5d §20: a StateEdge op's optional `depth` attr (RESERVED vocabulary) must be an Int ≥ 1 when present.
            if (state_edge)
            {
                const AttrId did = op->attr("depth");
                if (did.valid())
                {
                    const AttrValue dv = ctx.attr_value(did);
                    if (dv.kind != AttrKind::Int || dv.i < 1) { return {op, nullptr, StructureErrorKind::StateDepthInvalid}; }
                }
            }
            for (u32 i = 0; i < op->num_operands(); ++i) // every operand must be VISIBLE at this use (dominance)
            {
                const Value* const v = op->operand(i);
                if (dom_visible(v, &scope, /*respect*/ true)) { continue; }
                if (all_defs.contains(v)) // defined LATER in THIS block ⇒ a same-block back-edge (a feedback edge)
                {
                    // ⛔ the ONE legal back-edge (§20): a StateEdge op's LAST operand (its `next`/feedback). Any other
                    // same-block back-edge is a combinational feedback cycle. (The state op must HEAD its cycle in list
                    // order — an equivalently-cyclic program listed otherwise must be re-ordered; a canonicalization
                    // concern, not a semantics loss.)
                    if (state_edge && i + 1U == op->num_operands()) { continue; }
                    return {op, v, StructureErrorKind::FeedbackWithoutState};
                }
                const StructureErrorKind k = dom_visible(v, &scope, /*respect*/ false)
                                                 ? StructureErrorKind::CaptureThroughIsolation
                                                 : StructureErrorKind::UseBeforeDef;
                return {op, v, k};
            }
            if (ctx.has_trait(op->kind(), OpTrait::Terminator) && op != b->last_op())
            {
                return {op, nullptr, StructureErrorKind::TerminatorNotLast};
            }
            const bool sub_isolated = ctx.has_trait(op->kind(), OpTrait::IsolatedFromAbove);
            const bool core         = is_core_op(ctx, *op);
            for (u32 ri = 0; ri < op->num_regions(); ++ri)
            {
                if (core) // the yield↔owner contract binds the core structured ops only
                {
                    const StructureError ye = check_yield_count(ctx, *op, ri);
                    if (ye.kind != StructureErrorKind::None) { return ye; }
                }
                // capture visibility: a nested region sees defs up to (but not incl.) THIS op's results — and nothing
                // above an IsolatedFromAbove owner.
                const StructureError e = walk_region(ctx, op->region(ri), &scope, sub_isolated);
                if (e.kind != StructureErrorKind::None) { return e; }
            }
            for (u32 i = 0; i < op->num_results(); ++i) { defs.insert(op->result(i), 1U); } // now this op's results are visible
        }
    }
    return {};
}
} // namespace

StructureError Context::find_structure_error(const Module& m) const
{
    return walk_region(*this, m.body(), nullptr, /*isolated*/ false); // the module body sees nothing above it
}

// ── CEIR-6a §116 async-token-misuse verifier ──
containers::StringView token_misuse_kind_name(TokenMisuseKind k) noexcept
{
    switch (k) // ⛔ no default: a new kind must be named here (a -Werror=switch compile error otherwise)
    {
    case TokenMisuseKind::None: return containers::StringView("none");
    case TokenMisuseKind::Unconsumed: return containers::StringView("unconsumed");
    case TokenMisuseKind::MultiplyConsumed: return containers::StringView("multiply-consumed");
    case TokenMisuseKind::ConsumedByNonConsumer: return containers::StringView("consumed-by-non-consumer");
    }
    return containers::StringView("?");
}

namespace
{
struct TokenScan
{
    const Context&                          ctx;
    containers::HashMap<const Value*, u32>& consumed;  // a registered token → its consuming-operand-slot count
    containers::Array<const Operation*>&    producers; // TokenProducer ops, in pre-order (for the post-walk Unconsumed pass)
};
// Pre-order walk: check each op's operands (uses) BEFORE registering its results (defs) — a token dominates its uses, so a
// producer is always seen first. A token used by a TokenConsumer counts a consuming slot (2nd ⇒ MultiplyConsumed); used by
// any other op ⇒ ConsumedByNonConsumer. Returns the first use-misuse or `{}` (the Unconsumed pass is post-walk).
[[nodiscard]] TokenMisuse scan_tokens(TokenScan& s, Region* r)
{
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const bool is_consumer = s.ctx.has_trait(op->kind(), OpTrait::TokenConsumer);
            for (u32 i = 0; i < op->num_operands(); ++i)
            {
                const Value* const v = op->operand(i);
                u32* const         c = s.consumed.find(v); // non-null ⇔ v is a registered token
                if (c == nullptr) { continue; }
                if (!is_consumer) { return {v, op, TokenMisuseKind::ConsumedByNonConsumer}; }
                if (++(*c) >= 2U) { return {v, op, TokenMisuseKind::MultiplyConsumed}; } // count SLOTS: join(t,t) = 2
            }
            if (s.ctx.has_trait(op->kind(), OpTrait::TokenProducer)) // EVERY result of a producer is a token
            {
                for (u32 j = 0; j < op->num_results(); ++j) { s.consumed.insert(op->result(j), 0U); }
                s.producers.push_back(op);
            }
            for (u32 ri = 0; ri < op->num_regions(); ++ri)
            {
                const TokenMisuse e = scan_tokens(s, op->region(ri));
                if (e.kind != TokenMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

TokenMisuse Context::find_token_misuse(const Module& m) const
{
    containers::HashMap<const Value*, u32> consumed(allocator());
    containers::Array<const Operation*>    producers(allocator());
    TokenScan                              s{*this, consumed, producers};
    const TokenMisuse                      use_err = scan_tokens(s, m.body());
    if (use_err.kind != TokenMisuseKind::None) { return use_err; }
    // post-walk: the FIRST (pre-order) token that was never consumed — a leaked/dropped async op (no await XOR cancel).
    for (u32 i = 0; i < static_cast<u32>(producers.size()); ++i)
    {
        const Operation* const p = producers[i];
        for (u32 j = 0; j < p->num_results(); ++j)
        {
            const Value* const r = p->result(j);
            const u32* const   c = consumed.find(r);
            if (c != nullptr && *c == 0U) { return {r, p, TokenMisuseKind::Unconsumed}; }
        }
    }
    return {};
}

// ── CEIR-12a §36: the resource-dialect type-system enforcement (find_resource_misuse) ──
namespace
{
// a CEIR-3c resource TYPE — the kinds a resource.view operand / export operand / declare|import result must be.
[[nodiscard]] bool ceir_is_resource_kind(TypeKind k) noexcept
{
    switch (k)
    {
    case TypeKind::Buffer:
    case TypeKind::Image:
    case TypeKind::Tensor:
    case TypeKind::SparseTensor:
    case TypeKind::Sampler:
    case TypeKind::ResourceTable:
    case TypeKind::AccelStruct:
    case TypeKind::VideoFrame:
    case TypeKind::AudioBuffer:
    case TypeKind::ExternalResource:
    case TypeKind::View:
        return true;
    default: // ⛔ a NEW resource TypeKind (§23 growth) MUST be added above — else views/exports over it read as non-resource
        return false;
    }
}
// popcount over the 5 ViewRange bits (the mask is bounded by kViewRangeAll = 0x1F).
[[nodiscard]] crd::u32 ceir_popcount5(crd::u32 mask) noexcept
{
    crd::u32 n = 0U;
    for (crd::u32 b = 0; b < 5U; ++b) { n += (mask >> b) & 1U; }
    return n;
}
// the pre-order walk — the FIRST resource-op misuse, or {None}. ⛔ The per-op check ORDER is CONTRACTUAL (the negative
// tests pin the exact kind, so a reorder would silently re-label): view = result-is-View → operand-viewable → underlying
// == operand → mask-valid → arity; export = operand-resource; declare/import = result-resource.
ResourceMisuse scan_resources(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm = ctx.op_name(op->kind());
            if (nm == containers::StringView("resource.view"))
            {
                if (op->num_results() >= 1U && op->num_operands() >= 1U)
                {
                    const Value* const vv = op->result(0U);
                    const Type         vt = ctx.type_of(vv->type());
                    if (vt.kind != TypeKind::View) { return {vv, op, ResourceMisuseKind::ViewResultNotView}; }
                    const Value* const res = op->operand(0U);
                    const TypeKind     rk  = ctx.type_of(res->type()).kind;
                    if (rk != TypeKind::Buffer && rk != TypeKind::Image) // only Buffer/Image are viewable (view_combination_valid)
                    {
                        return {res, op, ResourceMisuseKind::ViewOperandNotViewable}; // ⛔ rejects view-of-view + non-viewable resources
                    }
                    if (vt.members.size() < 1U || !(vt.members[0] == res->type()))
                    {
                        return {vv, op, ResourceMisuseKind::ViewUnderlyingMismatch};
                    }
                    const crd::u32 mask = vt.count; // View's `count` = the ViewRange presence mask
                    if (!ctx.view_combination_valid(res->type(), mask))
                    {
                        return {vv, op, ResourceMisuseKind::ViewMaskInvalid};
                    }
                    if (op->num_operands() != 1U + 2U * ceir_popcount5(mask))
                    {
                        return {vv, op, ResourceMisuseKind::ViewRangeArity}; // offset/size PAIRS per masked dim
                    }
                }
            }
            else if (nm == containers::StringView("resource.export"))
            {
                if (op->num_operands() >= 1U && !ceir_is_resource_kind(ctx.type_of(op->operand(0U)->type()).kind))
                {
                    return {op->operand(0U), op, ResourceMisuseKind::ExportOperandNotResource};
                }
            }
            else if (nm == containers::StringView("resource.declare") || nm == containers::StringView("resource.import"))
            {
                if (op->num_results() >= 1U && !ceir_is_resource_kind(ctx.type_of(op->result(0U)->type()).kind))
                {
                    return {op->result(0U), op, ResourceMisuseKind::DeclImportResultNotResource};
                }
            }
            for (crd::u32 i = 0; i < op->num_regions(); ++i)
            {
                const ResourceMisuse e = scan_resources(ctx, op->region(i));
                if (e.kind != ResourceMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

containers::StringView resource_misuse_kind_name(ResourceMisuseKind k) noexcept
{
    switch (k)
    {
    case ResourceMisuseKind::None: return containers::StringView("none");
    case ResourceMisuseKind::ViewOperandNotViewable: return containers::StringView("view-operand-not-viewable");
    case ResourceMisuseKind::ViewResultNotView: return containers::StringView("view-result-not-view");
    case ResourceMisuseKind::ViewUnderlyingMismatch: return containers::StringView("view-underlying-mismatch");
    case ResourceMisuseKind::ViewMaskInvalid: return containers::StringView("view-mask-invalid");
    case ResourceMisuseKind::ViewRangeArity: return containers::StringView("view-range-arity");
    case ResourceMisuseKind::ExportOperandNotResource: return containers::StringView("export-operand-not-resource");
    case ResourceMisuseKind::DeclImportResultNotResource: return containers::StringView("decl-import-result-not-resource");
    }
    return containers::StringView("?");
}

ResourceMisuse Context::find_resource_misuse(const Module& m) const noexcept { return scan_resources(*this, m.body()); }

// ── CEIR-12b §24/§25: the resource planning-INTENT attribute-vocabulary enforcement (find_resource_intent_misuse) ──
// A SEPARATE module walk from find_resource_misuse (12a, the CEIR-3c TYPE contract). The intent attrs are the §20 lifetime
// + §24 memory-domain + §25 residency + export-direction vocabulary — a different layer than the typing — so this stays a
// distinct verifier: 12a's contractual check order + its 8 pinned negatives never move (the widen-enum-audit scar). ⛔ A
// wrong VALUE and a wrong attr-KIND fold into ONE kind per attr (the state-depth precedent at the top of this file:
// `dv.kind != Int || dv.i < 1` → StateDepthInvalid). OPEN tags (streaming_priority/budget_class) are unchecked.
namespace
{
constexpr containers::StringView kLifetimeVocab[] = {
    containers::StringView("transient"), containers::StringView("persistent"), containers::StringView("history")};
constexpr containers::StringView kMemoryDomainVocab[] = {
    containers::StringView("host"), containers::StringView("pinned_host"), containers::StringView("device_local"),
    containers::StringView("host_visible_device"), containers::StringView("unified"), containers::StringView("upload"),
    containers::StringView("readback"), containers::StringView("sparse"), containers::StringView("external"),
    containers::StringView("peer_visible"), containers::StringView("distributed")};
constexpr containers::StringView kResidencyVocab[] = {
    containers::StringView("resident"), containers::StringView("streamable"), containers::StringView("evictable")};
constexpr containers::StringView kDirectionVocab[] = {
    containers::StringView("read"), containers::StringView("readwrite")};
// the declare-ONLY planning-intent attr names — their presence on a resource.import is IntentAttrOnImport.
constexpr containers::StringView kIntentAttrNames[] = {
    containers::StringView("lifetime"), containers::StringView("history_length"),
    containers::StringView("memory_domain"), containers::StringView("residency"),
    containers::StringView("streaming_priority"), containers::StringView("budget_class"),
    containers::StringView("size_class")}; // CEIR-12c added: declare-only planning input, so import must reject it too

[[nodiscard]] bool ceir_sv_in(containers::StringView s, const containers::StringView* set, crd::u32 n) noexcept
{
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (s == set[i]) { return true; }
    }
    return false;
}
// A CLOSED-vocabulary STRING attr: ABSENT ⇒ ok (unspecified is a valid state, not a misuse); else it must be a String
// value whose text is in `set`. A wrong KIND (a non-String stored under the name) folds into "not ok" — one kind.
[[nodiscard]] bool ceir_intent_string_ok(const Context& ctx, const Operation* op, containers::StringView name,
                                         const containers::StringView* set, crd::u32 n) noexcept
{
    const AttrId a = op->attr(name);
    if (!a.valid()) { return true; }
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::String && ceir_sv_in(v.s, set, n);
}
// the pre-order walk — the FIRST intent misuse, or {None}. Per-attr check order on declare is CONTRACTUAL (negatives pin
// the exact kind): lifetime → history_length(value) → history_length(without-history) → memory_domain → residency.
ResourceIntentMisuse scan_resource_intent(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm = ctx.op_name(op->kind());
            if (nm == containers::StringView("resource.declare"))
            {
                if (!ceir_intent_string_ok(ctx, op, containers::StringView("lifetime"), kLifetimeVocab, 3U))
                {
                    return {op, ResourceIntentMisuseKind::LifetimeValueInvalid};
                }
                const AttrId hl = op->attr(containers::StringView("history_length"));
                if (hl.valid())
                {
                    const AttrValue hv = ctx.attr_value(hl);
                    if (hv.kind != AttrKind::Int || hv.i < 1) { return {op, ResourceIntentMisuseKind::HistoryLengthInvalid}; }
                    // lifetime is absent-or-valid here (a bad lifetime already returned above), so "history" iff String=="history".
                    const AttrId lf         = op->attr(containers::StringView("lifetime"));
                    bool         is_history = false;
                    if (lf.valid())
                    {
                        const AttrValue lv = ctx.attr_value(lf);
                        is_history = lv.kind == AttrKind::String && lv.s == containers::StringView("history");
                    }
                    if (!is_history) { return {op, ResourceIntentMisuseKind::HistoryLengthWithoutHistory}; }
                }
                if (!ceir_intent_string_ok(ctx, op, containers::StringView("memory_domain"), kMemoryDomainVocab, 11U))
                {
                    return {op, ResourceIntentMisuseKind::MemoryDomainValueInvalid};
                }
                if (!ceir_intent_string_ok(ctx, op, containers::StringView("residency"), kResidencyVocab, 3U))
                {
                    return {op, ResourceIntentMisuseKind::ResidencyValueInvalid};
                }
                // streaming_priority / budget_class: OPEN tags (alias_group-style) — unchecked; the 12d planner consumes them.
            }
            else if (nm == containers::StringView("resource.export"))
            {
                if (!ceir_intent_string_ok(ctx, op, containers::StringView("direction"), kDirectionVocab, 2U))
                {
                    return {op, ResourceIntentMisuseKind::DirectionValueInvalid};
                }
            }
            else if (nm == containers::StringView("resource.import"))
            {
                for (const containers::StringView an : kIntentAttrNames)
                {
                    if (op->has_attr(an)) { return {op, ResourceIntentMisuseKind::IntentAttrOnImport}; }
                }
            }
            for (crd::u32 i = 0; i < op->num_regions(); ++i)
            {
                const ResourceIntentMisuse e = scan_resource_intent(ctx, op->region(i));
                if (e.kind != ResourceIntentMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

containers::StringView resource_intent_misuse_kind_name(ResourceIntentMisuseKind k) noexcept
{
    switch (k)
    {
    case ResourceIntentMisuseKind::None: return containers::StringView("none");
    case ResourceIntentMisuseKind::LifetimeValueInvalid: return containers::StringView("lifetime-value-invalid");
    case ResourceIntentMisuseKind::HistoryLengthInvalid: return containers::StringView("history-length-invalid");
    case ResourceIntentMisuseKind::HistoryLengthWithoutHistory: return containers::StringView("history-length-without-history");
    case ResourceIntentMisuseKind::MemoryDomainValueInvalid: return containers::StringView("memory-domain-value-invalid");
    case ResourceIntentMisuseKind::ResidencyValueInvalid: return containers::StringView("residency-value-invalid");
    case ResourceIntentMisuseKind::DirectionValueInvalid: return containers::StringView("direction-value-invalid");
    case ResourceIntentMisuseKind::IntentAttrOnImport: return containers::StringView("intent-attr-on-import");
    }
    return containers::StringView("?");
}

ResourceIntentMisuse Context::find_resource_intent_misuse(const Module& m) const noexcept
{
    return scan_resource_intent(*this, m.body());
}

// ── CEIR-13a §42: the ceir.compute dispatch well-formedness enforcement (find_dispatch_misuse) ──
namespace
{
// A CEIR-3c args buffer: an indirect dispatch's operand 0 must be a Buffer, or a View whose underlying is a Buffer.
[[nodiscard]] bool ceir_is_buffer_or_view_of_buffer(const Context& ctx, TypeId t) noexcept
{
    const Type ty = ctx.type_of(t);
    if (ty.kind == TypeKind::Buffer) { return true; }
    if (ty.kind == TypeKind::View && ty.members.size() >= 1U) { return ctx.type_of(ty.members[0]).kind == TypeKind::Buffer; }
    return false;
}
// Parse the `access` string: comma-separated tokens, each EXACTLY "r" | "w" | "rw", one per binding in operand order (an
// empty string = zero bindings). Sets `count` and returns false on any malformed/empty token. ⛔ no StringView slicing —
// compare the token bytes directly (r/w len 1, rw len 2), so no (ptr,len) StringView ctor dependency.
[[nodiscard]] bool ceir_parse_access(containers::StringView s, crd::u32& count) noexcept
{
    count = 0;
    if (s.size() == 0U) { return true; }
    crd::usize start = 0;
    for (crd::usize i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == ',')
        {
            const crd::usize len = i - start;
            const char*      t   = s.data() + start;
            const bool       ok  = (len == 1U && (t[0] == 'r' || t[0] == 'w')) || (len == 2U && t[0] == 'r' && t[1] == 'w');
            if (!ok) { return false; }
            ++count;
            start = i + 1U;
        }
    }
    return true;
}
// the pre-order walk — the FIRST dispatch misuse, or {None}. ⛔ per-op check ORDER is CONTRACTUAL (negatives pin the exact
// kind): dispatch = grid-is-index → access(kind-fold → tokens → arity) → bindings-resource; dispatch_indirect =
// args-is-buffer → access(kind-fold → tokens → arity) → bindings-resource.
DispatchMisuse scan_dispatch(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm     = ctx.op_name(op->kind());
            const bool                   direct  = nm == containers::StringView("compute.dispatch");
            const bool                   indirect = nm == containers::StringView("compute.dispatch_indirect");
            if (direct || indirect)
            {
                // ⛔ IDENTITY before contract (CEIR-13c): the @kernel must be a readable Symbol — a raw/deserialized dispatch
                // with an absent or non-symbol `kernel` is KernelNotSymbol (the access-fold consistency; standalone-robust).
                if (ctx.attr_value(op->attr(containers::StringView("kernel"))).kind != AttrKind::SymbolRef)
                {
                    return {nullptr, op, DispatchMisuseKind::KernelNotSymbol};
                }
                const crd::u32 fixed    = direct ? 3U : 1U;                 // grid(3) vs args(1)
                const crd::u32 bindings = op->num_operands() >= fixed ? op->num_operands() - fixed : 0U;
                if (direct)
                {
                    for (crd::u32 i = 0; i < 3U && i < op->num_operands(); ++i)
                    {
                        if (ctx.type_of(op->operand(i)->type()).kind != TypeKind::Index)
                        {
                            return {op->operand(i), op, DispatchMisuseKind::GridNotIndex};
                        }
                    }
                }
                else if (op->num_operands() >= 1U && !ceir_is_buffer_or_view_of_buffer(ctx, op->operand(0U)->type()))
                {
                    return {op->operand(0U), op, DispatchMisuseKind::ArgsNotBuffer};
                }
                crd::u32        tokens = 0;
                const AttrValue av     = ctx.attr_value(op->attr(containers::StringView("access")));
                // ⛔ a wrong-KIND (or ABSENT -> attr_value yields Int) `access` folds into AccessTokenInvalid, the 12b fold
                // doctrine — NOT silently skipped: a deserialized module is built RAW (graceful-reject), so per-op verify
                // may not have run before this standalone walk, and a false-clean here would be a real path.
                if (av.kind != AttrKind::String) { return {nullptr, op, DispatchMisuseKind::AccessTokenInvalid}; }
                if (!ceir_parse_access(av.s, tokens)) { return {nullptr, op, DispatchMisuseKind::AccessTokenInvalid}; }
                if (tokens != bindings) { return {nullptr, op, DispatchMisuseKind::AccessArityMismatch}; }
                for (crd::u32 i = fixed; i < op->num_operands(); ++i)
                {
                    if (!ceir_is_resource_kind(ctx.type_of(op->operand(i)->type()).kind))
                    {
                        return {op->operand(i), op, DispatchMisuseKind::BindingNotResource};
                    }
                }
            }
            for (crd::u32 i = 0; i < op->num_regions(); ++i)
            {
                const DispatchMisuse e = scan_dispatch(ctx, op->region(i));
                if (e.kind != DispatchMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

containers::StringView dispatch_misuse_kind_name(DispatchMisuseKind k) noexcept
{
    switch (k)
    {
    case DispatchMisuseKind::None: return containers::StringView("none");
    case DispatchMisuseKind::GridNotIndex: return containers::StringView("grid-not-index");
    case DispatchMisuseKind::AccessTokenInvalid: return containers::StringView("access-token-invalid");
    case DispatchMisuseKind::AccessArityMismatch: return containers::StringView("access-arity-mismatch");
    case DispatchMisuseKind::BindingNotResource: return containers::StringView("binding-not-resource");
    case DispatchMisuseKind::ArgsNotBuffer: return containers::StringView("args-not-buffer");
    case DispatchMisuseKind::KernelNotSymbol: return containers::StringView("kernel-not-symbol");
    }
    return containers::StringView("?");
}

DispatchMisuse Context::find_dispatch_misuse(const Module& m) const noexcept { return scan_dispatch(*this, m.body()); }

// ── CEIR-13b §50: the ceir.transfer well-formedness enforcement (find_transfer_misuse) ──
namespace
{
// the EFFECTIVE resource kind: for a View, its underlying (members[0]); else the type's own kind (the 12a one-hop rule —
// views are flat, so one hop is complete). Used to resolve transferability + mip_gen-is-image + clear-on-image.
[[nodiscard]] TypeKind ceir_effective_kind(const Context& ctx, TypeId t) noexcept
{
    const Type ty = ctx.type_of(t);
    if (ty.kind == TypeKind::View && ty.members.size() >= 1U) { return ctx.type_of(ty.members[0]).kind; }
    return ty.kind;
}
// a TRANSFERABLE operand: a Buffer or Image, or a View of one. ⛔ opaque ExternalResource is OUT (kind unknowable).
[[nodiscard]] bool ceir_is_transferable(const Context& ctx, TypeId t) noexcept
{
    const TypeKind k = ceir_effective_kind(ctx, t);
    return k == TypeKind::Buffer || k == TypeKind::Image;
}
// the pre-order walk — the FIRST transfer misuse, or {None}. ⛔ per-op check ORDER is CONTRACTUAL (negatives pin the exact
// kind): copy = dst-transferable → src-transferable → src!=dst; upload/readback = operand-transferable; clear =
// dst-transferable → value(kind-fold → on-image); mip_gen = is-image.
TransferMisuse scan_transfer(const Context& ctx, const Region* r) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm = ctx.op_name(op->kind());
            if (nm == containers::StringView("transfer.copy"))
            {
                if (op->num_operands() >= 1U && !ceir_is_transferable(ctx, op->operand(0U)->type()))
                {
                    return {op->operand(0U), op, TransferMisuseKind::OperandNotTransferable};
                }
                if (op->num_operands() >= 2U && !ceir_is_transferable(ctx, op->operand(1U)->type()))
                {
                    return {op->operand(1U), op, TransferMisuseKind::OperandNotTransferable};
                }
                if (op->num_operands() >= 2U && op->operand(0U) == op->operand(1U))
                {
                    return {op->operand(0U), op, TransferMisuseKind::CopySrcIsDst}; // ⛔ distinct views of one root are LEGAL
                }
            }
            else if (nm == containers::StringView("transfer.upload") || nm == containers::StringView("transfer.readback"))
            {
                if (op->num_operands() >= 1U && !ceir_is_transferable(ctx, op->operand(0U)->type()))
                {
                    return {op->operand(0U), op, TransferMisuseKind::OperandNotTransferable};
                }
            }
            else if (nm == containers::StringView("transfer.clear"))
            {
                if (op->num_operands() >= 1U && !ceir_is_transferable(ctx, op->operand(0U)->type()))
                {
                    return {op->operand(0U), op, TransferMisuseKind::OperandNotTransferable};
                }
                const AttrId a = op->attr(containers::StringView("value"));
                if (a.valid())
                {
                    const AttrValue av = ctx.attr_value(a);
                    if (av.kind != AttrKind::Int) { return {nullptr, op, TransferMisuseKind::ClearValueInvalid}; }
                    if (op->num_operands() >= 1U && ceir_effective_kind(ctx, op->operand(0U)->type()) == TypeKind::Image)
                    {
                        return {nullptr, op, TransferMisuseKind::ClearValueOnImage};
                    }
                }
            }
            else if (nm == containers::StringView("transfer.mip_gen"))
            {
                if (op->num_operands() >= 1U && ceir_effective_kind(ctx, op->operand(0U)->type()) != TypeKind::Image)
                {
                    return {op->operand(0U), op, TransferMisuseKind::MipGenNotImage};
                }
            }
            for (crd::u32 i = 0; i < op->num_regions(); ++i)
            {
                const TransferMisuse e = scan_transfer(ctx, op->region(i));
                if (e.kind != TransferMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

containers::StringView transfer_misuse_kind_name(TransferMisuseKind k) noexcept
{
    switch (k)
    {
    case TransferMisuseKind::None: return containers::StringView("none");
    case TransferMisuseKind::OperandNotTransferable: return containers::StringView("operand-not-transferable");
    case TransferMisuseKind::CopySrcIsDst: return containers::StringView("copy-src-is-dst");
    case TransferMisuseKind::MipGenNotImage: return containers::StringView("mip-gen-not-image");
    case TransferMisuseKind::ClearValueInvalid: return containers::StringView("clear-value-invalid");
    case TransferMisuseKind::ClearValueOnImage: return containers::StringView("clear-value-on-image");
    }
    return containers::StringView("?");
}

TransferMisuse Context::find_transfer_misuse(const Module& m) const noexcept { return scan_transfer(*this, m.body()); }

// ── CEIR-14a §40/§41: the ceir.render well-formedness enforcement (find_render_misuse) ──
namespace
{
// The render per-attachment CLOSED vocabularies (find_render_misuse enforces them, the resource-intent precedent).
constexpr containers::StringView kLoadVocab[]  = {containers::StringView("clear"), containers::StringView("load"),
                                                 containers::StringView("dontcare")};
constexpr containers::StringView kStoreVocab[] = {containers::StringView("store"), containers::StringView("dontcare")};
constexpr containers::StringView kClearKindVocab[] = {containers::StringView("float"), containers::StringView("uint")};
constexpr containers::StringView kBlendVocab[]     = {containers::StringView("opaque"), containers::StringView("alpha"),
                                                  containers::StringView("additive"),
                                                  containers::StringView("premultiplied")};
constexpr containers::StringView kCompareVocab[]   = {
    containers::StringView("never"),      containers::StringView("less"),          containers::StringView("equal"),
    containers::StringView("less_equal"), containers::StringView("greater"),       containers::StringView("not_equal"),
    containers::StringView("greater_equal"), containers::StringView("always")};

// A CLOSED-vocabulary STRING attr on a render op: ABSENT ⇒ ok (unspecified is a valid default); else it must be a String
// whose text is in `set` (a wrong KIND folds into "not ok" — one kind per attr). Mirrors ceir_intent_string_ok; reuses
// the file-local ceir_sv_in.
[[nodiscard]] bool ceir_render_string_ok(const Context& ctx, const Operation* op, containers::StringView name,
                                         const containers::StringView* set, crd::u32 n) noexcept
{
    const AttrId a = op->attr(name);
    if (!a.valid()) { return true; }
    const AttrValue v = ctx.attr_value(a);
    return v.kind == AttrKind::String && ceir_sv_in(v.s, set, n);
}
// An attachment operand type is an Image, or a View one-hop over an Image (the §41 mip/layer/aspect subresource case).
[[nodiscard]] bool ceir_is_image_or_view_of_image(const Context& ctx, TypeId t) noexcept
{
    const Type ty = ctx.type_of(t);
    if (ty.kind == TypeKind::Image) { return true; }
    if (ty.kind == TypeKind::View && ty.members.size() >= 1U) { return ctx.type_of(ty.members[0]).kind == TypeKind::Image; }
    return false;
}
// The underlying image FORMAT element type of an attachment operand (unwrap View one-hop → Image → members[0]); an
// invalid TypeId if not an image. For the RAH-1a.1 typed-clear-vs-format check.
[[nodiscard]] TypeId ceir_image_format(const Context& ctx, TypeId t) noexcept
{
    Type ty = ctx.type_of(t);
    if (ty.kind == TypeKind::View && ty.members.size() >= 1U) { ty = ctx.type_of(ty.members[0]); }
    if (ty.kind == TypeKind::Image && ty.members.size() >= 1U) { return ty.members[0]; }
    return TypeId{};
}
// Is `fmt` an UNSIGNED-integer format? A uint typed-clear needs one (RAH-1a.1). An unsigned Int element.
[[nodiscard]] bool ceir_is_uint_format(const Context& ctx, TypeId fmt) noexcept
{
    if (!fmt.valid()) { return false; }
    const Type f = ctx.type_of(fmt);
    return f.kind == TypeKind::Int && !f.is_signed;
}
enum class RAtt : crd::u8 { None = 0, Color, Depth };
// Which render attachment class is `t`? An Extern type of render.color_attachment / render.depth_attachment.
[[nodiscard]] RAtt ceir_attachment_class(const Context& ctx, TypeId t) noexcept
{
    const Type ty = ctx.type_of(t);
    if (ty.kind != TypeKind::Extern) { return RAtt::None; }
    const containers::StringView n = ctx.type_class_name(ty.type_class);
    if (n == containers::StringView("render.color_attachment")) { return RAtt::Color; }
    if (n == containers::StringView("render.depth_attachment")) { return RAtt::Depth; }
    return RAtt::None;
}
[[nodiscard]] bool ceir_is_pow2_1_64(crd::i64 v) noexcept { return v >= 1 && v <= 64 && (v & (v - 1)) == 0; }
// Does op-kind `k` declare a §26 GPUCommand effect? (a compute.dispatch / transfer / draw — a real command submission).
[[nodiscard]] bool ceir_op_is_gpu_command(const Context& ctx, OpId k) noexcept
{
    const containers::ConstSpan<EffectRecord> fx = ctx.op_effects(k);
    const EffectRecord* const                 d  = fx.data();
    for (crd::u32 i = 0; i < static_cast<crd::u32>(fx.size()); ++i)
    {
        if (d[i].family == EffectFamily::GPUCommand) { return true; }
    }
    return false;
}
// The CEIR-14b draw contract (render.draw / render.draw_indexed): identity (program symbol) BEFORE contract, then counts
// (operands 0-1) Index-typed, the index_buffer (indexed, operand 2) a Buffer/View, the `access` tokens + arity, bindings
// resource-kinded. Returns the FIRST misuse or {None}. Reuses the file-local ceir_parse_access /
// ceir_is_buffer_or_view_of_buffer / ceir_is_resource_kind (the scan_dispatch helpers, same TU).
// CEIR-14b/14c: a draw op's operand SHAPE — `n_counts` leading Index-typed count operands, then `n_buffers` Buffer/View
// operands (each with its OWN misuse kind), then the variadic bindings tail; `has_max_draws` gates the indirect DrawIndex-
// range check (the REN-40 scar's IR-side half — max_draws >= 1; the executor-pushes-the-row assertion is CEIR-14z).
struct DrawShape
{
    crd::u32         n_counts      = 0;
    crd::u32         n_buffers     = 0;
    RenderMisuseKind buf_err[2]    = {RenderMisuseKind::None, RenderMisuseKind::None};
    bool             has_max_draws = false;
};
// The draw-op shape for `nm`, or false if `nm` is not a render draw-family op. ⛔ THE authoritative draw-op name list —
// scan_render_region routes every draw here; the lowering (lower_scope_body) uses an EFFECT predicate (any GPUCommand op in
// a scope is a draw, guaranteed by find_render_misuse), so there is NO second name list to keep in sync (the 14c fragility).
[[nodiscard]] bool draw_shape_of(containers::StringView nm, DrawShape& out)
{
    if (nm == containers::StringView("render.draw")) { out = {2U, 0U, {RenderMisuseKind::None, RenderMisuseKind::None}, false}; return true; }
    if (nm == containers::StringView("render.draw_indexed")) { out = {2U, 1U, {RenderMisuseKind::DrawIndexBufferNotBuffer, RenderMisuseKind::None}, false}; return true; }
    if (nm == containers::StringView("render.draw_indirect")) { out = {0U, 1U, {RenderMisuseKind::IndirectArgsNotBuffer, RenderMisuseKind::None}, true}; return true; }
    if (nm == containers::StringView("render.draw_indirect_count")) { out = {0U, 2U, {RenderMisuseKind::IndirectArgsNotBuffer, RenderMisuseKind::IndirectCountNotBuffer}, true}; return true; }
    if (nm == containers::StringView("render.mesh_dispatch")) { out = {3U, 0U, {RenderMisuseKind::None, RenderMisuseKind::None}, false}; return true; }
    if (nm == containers::StringView("render.mesh_dispatch_indirect")) { out = {0U, 1U, {RenderMisuseKind::IndirectArgsNotBuffer, RenderMisuseKind::None}, false}; return true; }
    return false;
}
// The CEIR-14b/14c draw contract for op-shape `sh`: identity (program symbol) BEFORE contract, then counts Index-typed, the
// buffer operands each Buffer/View, max_draws >= 1 (indirect), the `access` tokens + arity, bindings resource-kinded.
[[nodiscard]] RenderMisuse ceir_check_draw(const Context& ctx, Operation* op, const DrawShape& sh)
{
    if (ctx.attr_value(op->attr(containers::StringView("program"))).kind != AttrKind::SymbolRef)
    {
        return {nullptr, op, RenderMisuseKind::ProgramNotSymbol};
    }
    for (crd::u32 i = 0; i < sh.n_counts && i < op->num_operands(); ++i)
    {
        if (ctx.type_of(op->operand(i)->type()).kind != TypeKind::Index)
        {
            return {op->operand(i), op, RenderMisuseKind::DrawCountNotIndex};
        }
    }
    for (crd::u32 j = 0; j < sh.n_buffers; ++j)
    {
        const crd::u32 idx = sh.n_counts + j;
        if (op->num_operands() > idx && !ceir_is_buffer_or_view_of_buffer(ctx, op->operand(idx)->type()))
        {
            return {op->operand(idx), op, sh.buf_err[j]};
        }
    }
    if (sh.has_max_draws)
    {
        const AttrId md = op->attr(containers::StringView("max_draws"));
        if (md.valid())
        {
            const AttrValue mv = ctx.attr_value(md);
            if (mv.kind != AttrKind::Int || mv.i < 1) { return {nullptr, op, RenderMisuseKind::MaxDrawsInvalid}; }
        }
    }
    const crd::u32  fixed    = sh.n_counts + sh.n_buffers;
    const crd::u32  bindings = op->num_operands() >= fixed ? op->num_operands() - fixed : 0U;
    crd::u32        tokens   = 0;
    const AttrValue av       = ctx.attr_value(op->attr(containers::StringView("access")));
    if (av.kind != AttrKind::String) { return {nullptr, op, RenderMisuseKind::DrawAccessInvalid}; }
    if (!ceir_parse_access(av.s, tokens)) { return {nullptr, op, RenderMisuseKind::DrawAccessInvalid}; }
    if (tokens != bindings) { return {nullptr, op, RenderMisuseKind::DrawAccessArity}; }
    for (crd::u32 i = fixed; i < op->num_operands(); ++i)
    {
        if (!ceir_is_resource_kind(ctx.type_of(op->operand(i)->type()).kind))
        {
            return {op->operand(i), op, RenderMisuseKind::DrawBindingNotResource};
        }
    }
    return {};
}

// the pre-order walk — the FIRST render misuse, or {None}. `in_scope` = are we inside a render.scope region? Per-op check
// ORDER is CONTRACTUAL (negatives pin the exact kind): color/depth_attachment = image-operand → load → store → (color:
// clear_kind → blend → clear-vs-format | depth: compare); scope = NOT-nested → operands-are-attachments → at-most-one-depth
// → width/height → sample_count, then recurse its region IN-SCOPE; draw = in-scope → program → counts → index_buffer →
// access → bindings; a non-render GPUCommand op inside a scope is ComputeInRenderScope.
RenderMisuse scan_render_region(const Context& ctx, const Region* r, bool in_scope) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm    = ctx.op_name(op->kind());
            const bool                   color = nm == containers::StringView("render.color_attachment");
            const bool                   depth = nm == containers::StringView("render.depth_attachment");
            DrawShape                    dsh;
            const bool                   draw = draw_shape_of(nm, dsh); // CEIR-14b/14c: the authoritative draw-op registry
            if (color || depth)
            {
                if (op->num_operands() >= 1U && !ceir_is_image_or_view_of_image(ctx, op->operand(0U)->type()))
                {
                    return {op->operand(0U), op, RenderMisuseKind::AttachmentNotImage};
                }
                if (!ceir_render_string_ok(ctx, op, containers::StringView("load"), kLoadVocab, 3U))
                {
                    return {nullptr, op, RenderMisuseKind::LoadOpInvalid};
                }
                if (!ceir_render_string_ok(ctx, op, containers::StringView("store"), kStoreVocab, 2U))
                {
                    return {nullptr, op, RenderMisuseKind::StoreOpInvalid};
                }
                if (color)
                {
                    if (!ceir_render_string_ok(ctx, op, containers::StringView("clear_kind"), kClearKindVocab, 2U))
                    {
                        return {nullptr, op, RenderMisuseKind::ClearKindInvalid};
                    }
                    if (!ceir_render_string_ok(ctx, op, containers::StringView("blend"), kBlendVocab, 4U))
                    {
                        return {nullptr, op, RenderMisuseKind::BlendInvalid};
                    }
                    // RAH-1a.1: a uint typed-clear needs a uint-format attachment (the scar lifted to the IR).
                    const AttrId ck = op->attr(containers::StringView("clear_kind"));
                    if (ck.valid())
                    {
                        const AttrValue ckv = ctx.attr_value(ck);
                        if (ckv.kind == AttrKind::String && ckv.s == containers::StringView("uint")
                            && op->num_operands() >= 1U
                            && !ceir_is_uint_format(ctx, ceir_image_format(ctx, op->operand(0U)->type())))
                        {
                            return {op->operand(0U), op, RenderMisuseKind::ClearKindFormatMismatch};
                        }
                    }
                }
                else if (!ceir_render_string_ok(ctx, op, containers::StringView("compare"), kCompareVocab, 8U))
                {
                    return {nullptr, op, RenderMisuseKind::CompareInvalid};
                }
            }
            else if (nm == containers::StringView("render.scope"))
            {
                if (in_scope) { return {nullptr, op, RenderMisuseKind::NestedRenderScope}; }
                crd::u32 depths = 0;
                for (crd::u32 i = 0; i < op->num_operands(); ++i)
                {
                    const RAtt a = ceir_attachment_class(ctx, op->operand(i)->type());
                    if (a == RAtt::None) { return {op->operand(i), op, RenderMisuseKind::ScopeOperandNotAttachment}; }
                    if (a == RAtt::Depth) { ++depths; }
                }
                if (depths > 1U) { return {nullptr, op, RenderMisuseKind::MultipleDepthAttachments}; }
                // width/height are REQUIRED Int >= 1 (a wrong-kind/absent folds into RenderAreaInvalid — standalone-robust).
                const AttrValue w = ctx.attr_value(op->attr(containers::StringView("width")));
                const AttrValue h = ctx.attr_value(op->attr(containers::StringView("height")));
                if (w.kind != AttrKind::Int || w.i < 1 || h.kind != AttrKind::Int || h.i < 1)
                {
                    return {nullptr, op, RenderMisuseKind::RenderAreaInvalid};
                }
                const AttrId sc = op->attr(containers::StringView("sample_count"));
                if (sc.valid())
                {
                    const AttrValue scv = ctx.attr_value(sc);
                    if (scv.kind != AttrKind::Int || !ceir_is_pow2_1_64(scv.i))
                    {
                        return {nullptr, op, RenderMisuseKind::SampleCountInvalid};
                    }
                }
                // ⭐ recurse the scope's region IN-SCOPE (the draws live here), then skip the generic recursion below.
                for (crd::u32 i = 0; i < op->num_regions(); ++i)
                {
                    const RenderMisuse e = scan_render_region(ctx, op->region(i), true);
                    if (e.kind != RenderMisuseKind::None) { return e; }
                }
                continue;
            }
            else if (draw)
            {
                if (!in_scope) { return {nullptr, op, RenderMisuseKind::DrawOutsideScope}; }
                const RenderMisuse e = ceir_check_draw(ctx, op, dsh);
                if (e.kind != RenderMisuseKind::None) { return e; }
            }
            else if (in_scope && ceir_op_is_gpu_command(ctx, op->kind()))
            {
                // a non-render command submission (compute.dispatch / transfer) inside a render pass is illegal.
                return {nullptr, op, RenderMisuseKind::ComputeInRenderScope};
            }
            // generic recursion into non-scope op regions (structured control flow propagates the current in_scope).
            for (crd::u32 i = 0; i < op->num_regions(); ++i)
            {
                const RenderMisuse e = scan_render_region(ctx, op->region(i), in_scope);
                if (e.kind != RenderMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

containers::StringView render_misuse_kind_name(RenderMisuseKind k) noexcept
{
    switch (k)
    {
    case RenderMisuseKind::None: return containers::StringView("none");
    case RenderMisuseKind::AttachmentNotImage: return containers::StringView("attachment-not-image");
    case RenderMisuseKind::LoadOpInvalid: return containers::StringView("load-op-invalid");
    case RenderMisuseKind::StoreOpInvalid: return containers::StringView("store-op-invalid");
    case RenderMisuseKind::ClearKindInvalid: return containers::StringView("clear-kind-invalid");
    case RenderMisuseKind::BlendInvalid: return containers::StringView("blend-invalid");
    case RenderMisuseKind::CompareInvalid: return containers::StringView("compare-invalid");
    case RenderMisuseKind::ClearKindFormatMismatch: return containers::StringView("clear-kind-format-mismatch");
    case RenderMisuseKind::ScopeOperandNotAttachment: return containers::StringView("scope-operand-not-attachment");
    case RenderMisuseKind::MultipleDepthAttachments: return containers::StringView("multiple-depth-attachments");
    case RenderMisuseKind::RenderAreaInvalid: return containers::StringView("render-area-invalid");
    case RenderMisuseKind::SampleCountInvalid: return containers::StringView("sample-count-invalid");
    case RenderMisuseKind::DrawOutsideScope: return containers::StringView("draw-outside-scope");
    case RenderMisuseKind::NestedRenderScope: return containers::StringView("nested-render-scope");
    case RenderMisuseKind::ComputeInRenderScope: return containers::StringView("compute-in-render-scope");
    case RenderMisuseKind::ProgramNotSymbol: return containers::StringView("program-not-symbol");
    case RenderMisuseKind::DrawCountNotIndex: return containers::StringView("draw-count-not-index");
    case RenderMisuseKind::DrawAccessInvalid: return containers::StringView("draw-access-invalid");
    case RenderMisuseKind::DrawAccessArity: return containers::StringView("draw-access-arity");
    case RenderMisuseKind::DrawBindingNotResource: return containers::StringView("draw-binding-not-resource");
    case RenderMisuseKind::DrawIndexBufferNotBuffer: return containers::StringView("draw-index-buffer-not-buffer");
    case RenderMisuseKind::IndirectArgsNotBuffer: return containers::StringView("indirect-args-not-buffer");
    case RenderMisuseKind::IndirectCountNotBuffer: return containers::StringView("indirect-count-not-buffer");
    case RenderMisuseKind::MaxDrawsInvalid: return containers::StringView("max-draws-invalid");
    }
    return containers::StringView("?");
}

RenderMisuse Context::find_render_misuse(const Module& m) const noexcept { return scan_render_region(*this, m.body(), false); }

u32 Context::register_file(containers::StringView path)
{
    if (path.empty()) { return 0U; }
    for (usize i = 0; i < m_files.size(); ++i)
    {
        if (m_files[i] == path) { return static_cast<u32>(i + 1U); } // dedup by path
    }
    m_files.push_back(intern_symbol(path)); // arena-copy so the id is stable for the Context's life
    return static_cast<u32>(m_files.size()); // index + 1 (0 = unknown)
}

containers::StringView Context::file_path(u32 file_id) const noexcept
{
    if (file_id == 0U || file_id > m_files.size()) { return {}; }
    return m_files[file_id - 1U];
}

Region* Context::create_region(RegionKind kind)
{
    Region* const r = memory::construct<Region>(m_arena);
    r->m_kind       = kind;
    return r;
}

void Context::set_region_kind(Region* r, RegionKind kind) noexcept
{
    if (r != nullptr) { r->m_kind = kind; }
}

Block* Context::create_block(u32 num_args, TypeId arg_type)
{
    Block* const b = memory::construct<Block>(m_arena);
    b->m_num_args  = num_args;
    b->m_args      = memory::construct_array<Value>(m_arena, num_args);
    for (u32 i = 0; i < num_args; ++i) { b->m_args[i].init(arg_type, ValueKind::BlockArg, i, b); }
    return b;
}

Operation* Context::create_operation(OpId kind, containers::ConstSpan<Value*> operands, u32 num_results,
                                     TypeId result_type, u32 num_regions)
{
    Operation* const op = memory::construct<Operation>(m_arena);
    op->m_kind          = kind;

    op->m_num_results = num_results;
    op->m_results     = memory::construct_array<Value>(m_arena, num_results);
    for (u32 i = 0; i < num_results; ++i) { op->m_results[i].init(result_type, ValueKind::OpResult, i, op); }

    op->m_num_regions = num_regions;
    op->m_regions     = memory::construct_array<Region*>(m_arena, num_regions); // trivially-init; overwritten below
    for (u32 i = 0; i < num_regions; ++i)
    {
        op->m_regions[i]           = create_region();
        op->m_regions[i]->m_parent = op; // wire the region->op back-link so an upward region walk terminates at the owner
    }

    const auto num_operands = static_cast<u32>(operands.size());
    op->m_num_operands      = num_operands;
    op->m_operands          = memory::construct_array<Use>(m_arena, num_operands);
    for (u32 i = 0; i < num_operands; ++i) { op->set_operand(i, operands[i]); } // wires the def-use lists

    return op;
}
} // namespace crd::ceir
