// v13-z — CLI registration for the trajectory-generation cluster (hesap.motion.*). The kinematic state + limits are
// DATA (scalars), so the agent reaches the generators directly. The headline is the arbitrary-state Ruckig-class OTG.
//
//   hesap.motion.otg.f64 : single-DoF arbitrary-state time-optimal OTG (the Ruckig third-order solver).
//     p0,v0,a0 : initial pos/vel/acc.  pf,vf,af : target pos/vel/acc.  vmax,amax,jmax : symmetric limits.
//     samples  : optional # trajectory samples (default 0).
//     Out blob = [valid(1/0), duration, nsamp, (t,pos,vel,acc) * nsamp].
//
//   hesap.motion.scurve.f64 : rest-to-rest jerk-limited S-curve profile (one DoF).
//     p0,pT : start/target.  vmax,amax,jmax : limits.
//     Out blob = [valid(1/0), total, tj, ta, tc]  (jerk / accel / cruise phase durations).
// Anchor: register_motion_cli_anchor().

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/motion/motion.hpp>

#include <utility>

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
namespace motion = crd::hesap::motion;
using crd::containers::ConstSpan;

CommandResult error_result(crd::memory::IAllocator* alloc, const char* msg)
{
    CommandResult r{alloc};
    r.ok = false;
    ResultError e{alloc};
    e.error_kind = crd::containers::String{"InvalidArgument", alloc};
    e.error_message = crd::containers::String{msg, alloc};
    r.value = std::move(e);
    return r;
}

CommandResult blob_f64_result(crd::memory::IAllocator* alloc, ConstSpan<crd::f64> values)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultBinaryBlob blob{alloc};
    const auto* raw = reinterpret_cast<const crd::u8*>(values.data());
    const crd::usize n_bytes = values.size() * sizeof(crd::f64);
    blob.bytes.reserve(n_bytes);
    for (crd::usize i = 0; i < n_bytes; ++i)
    {
        blob.bytes.push_back(raw[i]);
    }
    r.value = std::move(blob);
    return r;
}

void add_param(CommandSchema& s, crd::memory::IAllocator* alloc, const char* name, const char* desc, ParamKind kind,
               bool required)
{
    ParamSchema p{alloc};
    p.name = crd::containers::String{name, alloc};
    p.description = crd::containers::String{desc, alloc};
    p.kind = kind;
    p.required = required;
    s.params.push_back(std::move(p));
}

CommandSchema make_otg_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.motion.otg.f64", alloc};
    s.description = crd::containers::String{
        "Single-DoF arbitrary-state time-optimal OTG (Ruckig). Out = [valid,duration,nsamp,(t,p,v,a)*].", alloc};
    add_param(s, alloc, "p0", "initial position", ParamKind::F64, true);
    add_param(s, alloc, "v0", "initial velocity", ParamKind::F64, true);
    add_param(s, alloc, "a0", "initial acceleration", ParamKind::F64, true);
    add_param(s, alloc, "pf", "target position", ParamKind::F64, true);
    add_param(s, alloc, "vf", "target velocity", ParamKind::F64, true);
    add_param(s, alloc, "af", "target acceleration", ParamKind::F64, true);
    add_param(s, alloc, "vmax", "velocity limit (> 0)", ParamKind::F64, true);
    add_param(s, alloc, "amax", "acceleration limit (> 0)", ParamKind::F64, true);
    add_param(s, alloc, "jmax", "jerk limit (> 0)", ParamKind::F64, true);
    add_param(s, alloc, "samples", "number of trajectory samples to emit (default 0)", ParamKind::I64, false);
    return s;
}

CommandResult impl_otg(const CommandArgs& args)
{
    const auto p0 = args.get_f64("p0");
    const auto v0 = args.get_f64("v0");
    const auto a0 = args.get_f64("a0");
    const auto pf = args.get_f64("pf");
    const auto vf = args.get_f64("vf");
    const auto af = args.get_f64("af");
    const auto vmax = args.get_f64("vmax");
    const auto amax = args.get_f64("amax");
    const auto jmax = args.get_f64("jmax");
    if (!p0 || !v0 || !a0 || !pf || !vf || !af || !vmax || !amax || !jmax)
    {
        return error_result(args.alloc, "motion.otg: p0,v0,a0,pf,vf,af,vmax,amax,jmax are all required");
    }
    if (*vmax <= 0.0 || *amax <= 0.0 || *jmax <= 0.0)
    {
        return error_result(args.alloc, "motion.otg: vmax, amax, jmax must be > 0");
    }
    const motion::OtgProfile<crd::f64> prof =
        motion::plan_otg<crd::f64>(*p0, *v0, *a0, *pf, *vf, *af, *vmax, *amax, *jmax);

    crd::i64 nsamp = args.get_i64("samples").value_or(0);
    if (nsamp < 0)
    {
        nsamp = 0;
    }
    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(prof.valid ? 1.0 : 0.0);
    out.push_back(prof.duration);
    out.push_back(static_cast<crd::f64>(nsamp));
    for (crd::i64 k = 0; k < nsamp; ++k)
    {
        const crd::f64 t =
            (nsamp > 1) ? prof.duration * static_cast<crd::f64>(k) / static_cast<crd::f64>(nsamp - 1) : 0.0;
        crd::f64 p = 0.0;
        crd::f64 v = 0.0;
        crd::f64 a = 0.0;
        prof.eval(t, p, v, a);
        out.push_back(t);
        out.push_back(p);
        out.push_back(v);
        out.push_back(a);
    }
    return blob_f64_result(args.alloc, ConstSpan<crd::f64>(out.data(), out.size()));
}

CommandSchema make_scurve_schema(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.motion.scurve.f64", alloc};
    s.description = crd::containers::String{
        "Rest-to-rest jerk-limited S-curve profile (one DoF). Out = [valid,total,tj,ta,tc].", alloc};
    add_param(s, alloc, "p0", "start position", ParamKind::F64, true);
    add_param(s, alloc, "pT", "target position", ParamKind::F64, true);
    add_param(s, alloc, "vmax", "velocity limit (> 0)", ParamKind::F64, true);
    add_param(s, alloc, "amax", "acceleration limit (> 0)", ParamKind::F64, true);
    add_param(s, alloc, "jmax", "jerk limit (> 0)", ParamKind::F64, true);
    return s;
}

CommandResult impl_scurve(const CommandArgs& args)
{
    const auto p0 = args.get_f64("p0");
    const auto pt = args.get_f64("pT");
    const auto vmax = args.get_f64("vmax");
    const auto amax = args.get_f64("amax");
    const auto jmax = args.get_f64("jmax");
    if (!p0 || !pt || !vmax || !amax || !jmax)
    {
        return error_result(args.alloc, "motion.scurve: p0,pT,vmax,amax,jmax are all required");
    }
    if (*vmax <= 0.0 || *amax <= 0.0 || *jmax <= 0.0)
    {
        return error_result(args.alloc, "motion.scurve: vmax, amax, jmax must be > 0");
    }
    const motion::ScurveProfile<crd::f64> pr = motion::plan_scurve<crd::f64>(*p0, *pt, *vmax, *amax, *jmax);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.push_back(pr.valid ? 1.0 : 0.0);
    out.push_back(pr.total);
    out.push_back(pr.tj);
    out.push_back(pr.ta);
    out.push_back(pr.tc);
    return blob_f64_result(args.alloc, ConstSpan<crd::f64>(out.data(), out.size()));
}
} // namespace

namespace crd::hesap::motion
{
void register_motion_cli_anchor() noexcept {}
} // namespace crd::hesap::motion

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(make_otg_schema(alloc), &impl_otg);
        reg.register_command(make_scurve_schema(alloc), &impl_scurve);
    });
