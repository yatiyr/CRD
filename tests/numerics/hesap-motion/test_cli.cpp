// v13-z — CLI registration + invocation tests for hesap.motion.*. Pulls the module anchor so the static-init
// registration block survives the static-lib link, then drives the arbitrary-state OTG (reaches the target) and the
// jerk-limited S-curve through the command registry, cross-checking that a rest-to-rest OTG equals the S-curve time.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/cli/command_result.hpp>
#include <crd/hesap/motion/cli_anchor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <variant>

namespace cli = crd::hesap::cli;
using crd::f64;
using crd::usize;

namespace
{
const bool kPullMotion = (crd::hesap::motion::register_motion_cli_anchor(), true);

const cli::ResultBinaryBlob* as_blob(const cli::CommandResult& r) { return std::get_if<cli::ResultBinaryBlob>(&r.value); }

struct Decoded
{
    const f64* v;
    usize len;
};

Decoded invoke(cli::CommandArgs& args, const char* name, cli::CommandResult& store)
{
    const auto* rec = cli::CommandRegistry::global().find(name);
    REQUIRE(rec != nullptr);
    store = rec->impl(args);
    const auto* blob = as_blob(store);
    REQUIRE(blob != nullptr);
    return {reinterpret_cast<const f64*>(blob->bytes.data()), blob->bytes.size() / sizeof(f64)};
}

void set_otg(cli::CommandArgs& args, f64 p0, f64 v0, f64 a0, f64 pf, f64 vf, f64 af, f64 vmax, f64 amax, f64 jmax)
{
    args.set_f64("p0", p0);
    args.set_f64("v0", v0);
    args.set_f64("a0", a0);
    args.set_f64("pf", pf);
    args.set_f64("vf", vf);
    args.set_f64("af", af);
    args.set_f64("vmax", vmax);
    args.set_f64("amax", amax);
    args.set_f64("jmax", jmax);
}
} // namespace

TEST_CASE("CLI motion: hesap.motion.* commands are registered", "[motion][cli]")
{
    REQUIRE(kPullMotion);
    REQUIRE(cli::CommandRegistry::global().find("hesap.motion.otg.f64") != nullptr);
    REQUIRE(cli::CommandRegistry::global().find("hesap.motion.scurve.f64") != nullptr);
}

TEST_CASE("CLI motion: OTG reaches the target state", "[motion][cli]")
{
    REQUIRE(kPullMotion);
    crd::memory::TlsfAllocator alloc(1U << 18);
    cli::CommandArgs args{&alloc};
    set_otg(args, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 2.0, 1.5, 3.0); // rest-to-rest, 10 m move
    args.set_i64("samples", 21);
    cli::CommandResult store{&alloc};
    const Decoded d = invoke(args, "hesap.motion.otg.f64", store);
    REQUIRE(d.len >= 3);
    CHECK(d.v[0] == 1.0);   // valid
    CHECK(d.v[1] > 0.0);    // duration
    const usize nsamp = static_cast<usize>(d.v[2]);
    REQUIRE(nsamp == 21);
    // last sample = (t,p,v,a) reaching the target: p≈10, v≈0, a≈0
    const usize last = 3 + 4 * (nsamp - 1);
    REQUIRE(d.len == last + 4);
    CHECK(std::abs(d.v[last + 1] - 10.0) < 1e-6); // position
    CHECK(std::abs(d.v[last + 2] - 0.0) < 1e-6);  // velocity
    CHECK(std::abs(d.v[last + 3] - 0.0) < 1e-6);  // acceleration
    // first sample sits at the start
    CHECK(std::abs(d.v[3 + 1] - 0.0) < 1e-9);
}

TEST_CASE("CLI motion: a rest-to-rest OTG equals the jerk-limited S-curve time", "[motion][cli]")
{
    REQUIRE(kPullMotion);
    crd::memory::TlsfAllocator alloc(1U << 18);
    // OTG rest-to-rest
    cli::CommandArgs otg{&alloc};
    set_otg(otg, 0.0, 0.0, 0.0, 7.5, 0.0, 0.0, 2.0, 1.5, 3.0);
    cli::CommandResult otg_store{&alloc};
    const Decoded od = invoke(otg, "hesap.motion.otg.f64", otg_store);
    REQUIRE(od.len >= 2);
    CHECK(od.v[0] == 1.0);
    const f64 otg_dur = od.v[1];

    // S-curve, same move + limits
    cli::CommandArgs sc{&alloc};
    sc.set_f64("p0", 0.0);
    sc.set_f64("pT", 7.5);
    sc.set_f64("vmax", 2.0);
    sc.set_f64("amax", 1.5);
    sc.set_f64("jmax", 3.0);
    cli::CommandResult sc_store{&alloc};
    const Decoded sd = invoke(sc, "hesap.motion.scurve.f64", sc_store);
    REQUIRE(sd.len == 5);
    CHECK(sd.v[0] == 1.0);      // valid
    const f64 total = sd.v[1];  // total
    CHECK(total > 0.0);
    CHECK(sd.v[2] >= 0.0); // tj (jerk-phase duration)
    CHECK(sd.v[3] >= 0.0); // ta (accel-phase duration)
    CHECK(sd.v[4] >= 0.0); // tc (cruise duration)
    // the two min-time rest-to-rest computations agree
    CHECK(std::abs(otg_dur - total) < 1e-6);
}
