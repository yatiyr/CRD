// CEIR host-provider contract fatals (crd-ceir-host-fatal-* CTests, tests/execution/ceir-host). A pooled launch
// allocates its token from the provider's allocator after it has committed to the launch (the op's result is the
// token's handle); an allocator that refuses it (a sub-budget allocator returns nullptr) leaves the provider out of
// memory, fatal in every build, never a null token written through (DIAG.3a; it was written through before). One mode
// per run (argv[1]); see expected_fatal.hpp for the exit protocol:
//   pool-token-refused  HostProvider::execute of a pool-eligible async.launch: the launch's token is refused
// The allocator refuses exactly the token's request shape (refusing_allocator.hpp), so every other allocation of the
// run is granted and the token is the run's first refused one.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/ceir/ceir.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/host/host_provider.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>

namespace
{
namespace ce = crd::ceir;

// @main() -> i32 { %t = async.launch { %c = arith.const 10; core.yield %c }; %v = async.await %t; return %v }: a pure,
// self-contained launch body, so the provider pools it.
ce::Module* build_launch(ce::Context& c)
{
    (void)ce::arith::register_arith_ops(c);
    (void)ce::core::register_core_ops(c);
    (void)ce::async::register_async_ops(c);
    (void)ce::func::register_dialect(c);
    ce::Module* const m = c.create_module();
    m->body()->append(c.create_block(0U));
    ce::Operation* const f =
        ce::func::create_func(c, *m, crd::containers::StringView{"main", 4U}, ce::Visibility::Public, 0U, c.type_i32());
    if (f == nullptr)
    {
        CRD_FATAL("ceir-host-fatal: create_func refused the specimen's function");
    }
    m->body()->first_block()->append(f);
    ce::Block& body = ce::func::func_body(*f);

    ce::Operation* const launch = c.create_operation(c.intern_op("async", "launch"), {}, 1U, c.type_i32(), 1U);
    ce::Block* const launch_body = c.create_block(0U);
    launch->region(0)->append(launch_body);
    ce::Operation* const k = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_i32());
    c.set_attr(k, "value", c.attr_int(10));
    launch_body->append(k);
    ce::Value* yielded[1] = {k->result(0U)};
    launch_body->append(
        c.create_operation(c.intern_op("core", "yield"), crd::containers::ConstSpan<ce::Value*>(yielded, 1U), 0U));
    body.append(launch);

    ce::Value* token[1] = {launch->result(0U)};
    ce::Operation* const awaited = c.create_operation(
        c.intern_op("async", "await"), crd::containers::ConstSpan<ce::Value*>(token, 1U), 1U, c.type_i32());
    body.append(awaited);
    ce::Value* ret[1] = {awaited->result(0U)};
    body.append(ce::func::create_return(c, crd::containers::ConstSpan<ce::Value*>(ret, 1U)));
    return m;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    if (std::strcmp(mode, "pool-token-refused") != 0)
    {
        return false;
    }
    crd_diag_fatal::arm(mode, ce::host::detail::kPooledTokenOutOfMemory);
    crd::jobs::init(crd::jobs::Config{.num_threads = 2});
    ce::Context ctx(crd::memory::default_allocator());
    const ce::Module* const m = build_launch(ctx);
    crd_diag_fatal::RefusingAllocator alloc("ceir-host-fatal-refusing");
    ce::host::HostProvider provider(&alloc, 2U);
    alloc.refuse_shape(ce::host::detail::pooled_token_size(), ce::host::detail::pooled_token_alignment());
    (void)provider.execute(ctx, *m, crd::containers::StringView{"main", 4U}, {});
    crd::jobs::shutdown();
    return true;
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
