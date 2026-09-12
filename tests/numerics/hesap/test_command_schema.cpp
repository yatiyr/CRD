#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/cli/command_schema.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <utility>

using crd::hesap::cli::Capability;
using crd::hesap::cli::CommandSchema;
using crd::hesap::cli::DeprecationStatus;
using crd::hesap::cli::OutputKind;
using crd::hesap::cli::ParamKind;
using crd::hesap::cli::ParamSchema;
using crd::hesap::cli::SchemaVersion;

TEST_CASE("CommandSchema constructs with allocator and default version 1.0", "[hesap][cli][schema]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandSchema s{&alloc};
    REQUIRE(s.version.major == 1);
    REQUIRE(s.version.minor == 0);
    REQUIRE(s.deprecation == DeprecationStatus::Active);
    REQUIRE(s.output.kind == OutputKind::Void);
    REQUIRE_FALSE(s.idempotent);
    REQUIRE_FALSE(s.reversible);
}

TEST_CASE("CommandSchema accepts params and outputs", "[hesap][cli][schema]")
{
    crd::memory::TlsfAllocator alloc(64 * 1024);
    CommandSchema s{&alloc};
    s.name = crd::containers::String{"hesap.dense.matrix.create", &alloc};
    s.description = crd::containers::String{"Create a dense matrix", &alloc};
    s.output.kind = OutputKind::MatrixId;
    s.required_caps.bits = Capability::kHesapWrite;
    s.idempotent = false;

    {
        ParamSchema p{&alloc};
        p.name = crd::containers::String{"rows", &alloc};
        p.kind = ParamKind::U32;
        p.required = true;
        s.params.push_back(std::move(p));
    }
    {
        ParamSchema p{&alloc};
        p.name = crd::containers::String{"cols", &alloc};
        p.kind = ParamKind::U32;
        p.required = true;
        s.params.push_back(std::move(p));
    }
    {
        ParamSchema p{&alloc};
        p.name = crd::containers::String{"layout", &alloc};
        p.kind = ParamKind::Enum;
        p.enum_values = crd::containers::String{"row|col", &alloc};
        p.default_value = crd::containers::String{"row", &alloc};
        p.required = false;
        s.params.push_back(std::move(p));
    }

    REQUIRE(s.params.size() == 3);
    REQUIRE(s.params[0].kind == ParamKind::U32);
    REQUIRE(s.params[2].kind == ParamKind::Enum);
    REQUIRE_FALSE(s.params[2].required);
}

TEST_CASE("SchemaVersion equality is fieldwise", "[hesap][cli][schema]")
{
    REQUIRE(SchemaVersion{1, 0} == SchemaVersion{1, 0});
    REQUIRE_FALSE(SchemaVersion{1, 0} == SchemaVersion{1, 1});
    REQUIRE_FALSE(SchemaVersion{2, 0} == SchemaVersion{1, 0});
}

TEST_CASE("Capability::subset_of follows bitset semantics", "[hesap][cli][schema]")
{
    const Capability read{Capability::kHesapRead};
    const Capability read_write{Capability::kHesapRead | Capability::kHesapWrite};
    REQUIRE(read.subset_of(read_write));
    REQUIRE_FALSE(read_write.subset_of(read));
}
