// CEIR cook contract fatals (crd-ceir-cook-fatal-* CTests, tests/execution/ceir-cook). The reload set and the plan
// cache allocate what they construct or copy from the caller's allocator; one that refuses (a sub-budget allocator
// returns nullptr) leaves them out of memory, fatal in every build, never a null object constructed into or a null
// buffer filled (DIAG.3a; both wrote through the refused pointer before). One mode per run (argv[1]); see
// expected_fatal.hpp for the exit protocol:
//   reload-cook-context-refused        ReloadSet::add_source: the transient cook Context is refused
//   reload-generation-refused          ReloadSet::add of a cooked blob: the generation is refused
//   reload-generation-context-refused  ReloadSet::add of a cooked blob: the generation's own Context is refused
//   plan-artifact-refused              PlanCache::put: the artifact copy is refused
//   plan-deps-refused                  PlanCache::put: the dependency copy is refused
// Each mode grants its setup, then arms the refusal that makes its allocation the run's first refused one
// (refusing_allocator.hpp).

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/cook/plan_cache.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/print.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>
#include <utility> // std::move

namespace
{
namespace ce = crd::ceir;
namespace cook = crd::ceir::cook;
using crd_diag_fatal::RefusingAllocator;

constexpr crd::u64 kAssetId = 4242U;

void registrar(ce::Context& c, void* /*user*/)
{
    (void)ce::arith::register_arith_ops(c);
    (void)ce::core::register_core_ops(c);
    (void)ce::func::register_dialect(c);
}

// @leaf() -> i32 { return 5 }, built in `c`.
ce::Module* build_leaf(ce::Context& c)
{
    registrar(c, nullptr);
    ce::Module* const m = c.create_module();
    m->body()->append(c.create_block(0U));
    ce::Operation* const f =
        ce::func::create_func(c, *m, crd::containers::StringView{"leaf", 4U}, ce::Visibility::Public, 0U, c.type_i32());
    if (f == nullptr)
    {
        CRD_FATAL("ceir-cook-fatal: create_func refused the specimen's function");
    }
    m->body()->first_block()->append(f);
    ce::Block& body = ce::func::func_body(*f);
    ce::Operation* const k = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_i32());
    c.set_attr(k, "value", c.attr_int(5));
    body.append(k);
    ce::Value* ret[1] = {k->result(0U)};
    body.append(ce::func::create_return(c, crd::containers::ConstSpan<ce::Value*>(ret, 1U)));
    return m;
}

crd::containers::String leaf_source(crd::memory::IAllocator* alloc)
{
    ce::Context c(alloc);
    const ce::Module* const m = build_leaf(c);
    return ce::print(c, *m, alloc);
}

crd::containers::Array<crd::u8> leaf_blob(crd::memory::IAllocator* alloc)
{
    ce::Context c(alloc);
    const ce::Module* const m = build_leaf(c);
    cook::CookResult r = cook::cook_program(c, *m, kAssetId, alloc, alloc);
    if (!r.ok())
    {
        CRD_FATAL("ceir-cook-fatal: the specimen's leaf did not cook");
    }
    return std::move(r.blob);
}

crd::u64 resolve_none(cook::AssetId /*id*/, void* /*user*/)
{
    return 1U;
}

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// The reload set's modes. Returns false for an unknown mode.
[[nodiscard]] bool run_reload_mode(const char* mode)
{
    if (is(mode, "reload-cook-context-refused"))
    {
        crd_diag_fatal::arm(mode, cook::detail::kReloadCookContextOutOfMemory);
        RefusingAllocator alloc("ceir-cook-fatal-refusing");
        const crd::containers::String source = leaf_source(&alloc);
        cook::ReloadSet set(&alloc, &registrar, nullptr);
        alloc.refuse_all(); // add_source's first allocation is the cook Context
        (void)set.add_source(cook::AssetId{kAssetId}, crd::containers::StringView{source.c_str(), source.size()});
        return true;
    }
    if (is(mode, "reload-generation-refused"))
    {
        crd_diag_fatal::arm(mode, cook::detail::kReloadGenerationOutOfMemory);
        RefusingAllocator alloc("ceir-cook-fatal-refusing");
        const crd::containers::Array<crd::u8> blob = leaf_blob(&alloc);
        cook::ReloadSet set(&alloc, &registrar, nullptr);
        alloc.refuse_all(); // add's first allocation is the generation
        (void)set.add(cook::AssetId{kAssetId}, crd::containers::ConstSpan<crd::u8>{blob.data(), blob.size()});
        return true;
    }
    if (is(mode, "reload-generation-context-refused"))
    {
        crd_diag_fatal::arm(mode, cook::detail::kReloadGenerationContextOutOfMemory);
        RefusingAllocator alloc("ceir-cook-fatal-refusing");
        const crd::containers::Array<crd::u8> blob = leaf_blob(&alloc);
        cook::ReloadSet set(&alloc, &registrar, nullptr);
        // add cooks nothing, so the first request shaped like a Context is the generation's own.
        alloc.refuse_shape(sizeof(ce::Context), alignof(ce::Context));
        (void)set.add(cook::AssetId{kAssetId}, crd::containers::ConstSpan<crd::u8>{blob.data(), blob.size()});
        return true;
    }
    return false;
}

// The plan cache's modes. Returns false for an unknown mode.
[[nodiscard]] bool run_plan_mode(const char* mode)
{
    const bool artifact = is(mode, "plan-artifact-refused");
    const bool deps = is(mode, "plan-deps-refused");
    if (!artifact && !deps)
    {
        return false;
    }
    crd_diag_fatal::arm(mode, artifact ? cook::detail::kPlanCacheArtifactOutOfMemory
                                       : cook::detail::kPlanCacheDepsOutOfMemory);
    RefusingAllocator alloc("ceir-cook-fatal-refusing");
    cook::PlanCache cache(&alloc, &resolve_none, nullptr);
    const crd::u8 bytes[4] = {1U, 2U, 3U, 4U};
    const cook::PlanDep dep{cook::AssetId{7U}, 1U};
    // A warm-up entry gives the entry table its first capacity, so the next put appends without growing it.
    cache.put(cook::AssetId{1U}, cook::PlanKey{1U, 1U, 1U}, crd::containers::ConstSpan<crd::u8>{bytes, 4U},
              crd::containers::ConstSpan<cook::PlanDep>{&dep, 1U});
    alloc.refuse_all();
    // The artifact mode copies an artifact first; the deps mode stores none, so its first copy is the dependency.
    cache.put(cook::AssetId{2U}, cook::PlanKey{2U, 1U, 1U},
              artifact ? crd::containers::ConstSpan<crd::u8>{bytes, 4U} : crd::containers::ConstSpan<crd::u8>{},
              crd::containers::ConstSpan<cook::PlanDep>{&dep, 1U});
    return true;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    return run_reload_mode(mode) || run_plan_mode(mode);
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
