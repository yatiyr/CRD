// crd-sandbox -- the CEIR dialects of the sandbox's diagnostic commands. Declared in sandbox_dialects.hpp.

#include "sandbox_dialects.hpp"

#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/async_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/ceir/gen/input_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gen/task_ops.hpp>

namespace crd::sandbox
{
void register_sandbox_dialects(ceir::Context& ctx, void* /*user*/)
{
    (void)ceir::arith::register_arith_ops(ctx);
    (void)ceir::core::register_core_ops(ctx);
    (void)ceir::task::register_task_ops(ctx); // DIAG.9a: host programs (replay.record executor=host)
    (void)ceir::async::register_async_ops(ctx);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::input::register_input_ops(ctx);       // DIAG.9a: host inputs, recorded by replay.record
    (void)ceir::resource::register_resource_ops(ctx); // DIAG.9a: device programs (replay.record executor=device)
    (void)ceir::compute::register_compute_ops(ctx);
}
} // namespace crd::sandbox
