#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/linear_op.hpp>

namespace
{
// Identity matrix as a tiny concrete LinearOp — exercises the interface.
template <typename T>
class IdentityLinearOp final : public crd::hesap::LinearOp<T>
{
public:
    explicit IdentityLinearOp(crd::usize n) noexcept
        : crd::hesap::LinearOp<T>(/*has_transpose=*/true, /*has_adjoint=*/true), m_n(n)
    {
    }

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        if (x.size() != m_n || y.size() != m_n)
        {
            return false;
        }
        for (crd::usize i = 0; i < m_n; ++i)
        {
            y[i] = x[i];
        }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    [[nodiscard]] bool apply_transpose(
        crd::containers::ConstSpan<T> x,
        crd::containers::Span<T> y) const override
    {
        return apply(x, y);
    }

    [[nodiscard]] bool apply_adjoint(
        crd::containers::ConstSpan<T> x,
        crd::containers::Span<T> y) const override
    {
        return apply(x, y);
    }

private:
    crd::usize m_n;
};

// Minimal LinearOp that does NOT implement transpose / adjoint — exercises
// the default returns-false behaviour from the base class.
template <typename T>
class ZeroOnlyOp final : public crd::hesap::LinearOp<T>
{
public:
    ZeroOnlyOp(crd::usize r, crd::usize c) noexcept : m_r(r), m_c(c) {}

    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        if (x.size() != m_c || y.size() != m_r)
        {
            return false;
        }
        for (crd::usize i = 0; i < m_r; ++i)
        {
            y[i] = T(0);
        }
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_r; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_c; }

private:
    crd::usize m_r;
    crd::usize m_c;
};
} // namespace

TEST_CASE("LinearOp identity apply round-trips", "[hesap][linear_op]")
{
    IdentityLinearOp<double> op(4);
    const double x[4] = {1.5, 2.5, 3.5, 4.5};
    double y[4] = {0.0, 0.0, 0.0, 0.0};
    REQUIRE(op.apply(crd::containers::ConstSpan<double>{x, 4}, crd::containers::Span<double>{y, 4}));
    for (crd::usize i = 0; i < 4; ++i)
    {
        REQUIRE(y[i] == x[i]);
    }
    REQUIRE(op.n_rows() == 4);
    REQUIRE(op.n_cols() == 4);
    REQUIRE(op.is_square());
    REQUIRE(op.has_transpose());
    REQUIRE(op.has_adjoint());
}

TEST_CASE("LinearOp default transpose / adjoint return false when unimplemented", "[hesap][linear_op]")
{
    ZeroOnlyOp<double> op(3, 5);
    const double x[5] = {0, 0, 0, 0, 0};
    double y[3] = {99, 99, 99};
    REQUIRE_FALSE(op.has_transpose());
    REQUIRE_FALSE(op.has_adjoint());
    REQUIRE_FALSE(op.apply_transpose(crd::containers::ConstSpan<double>{x, 5}, crd::containers::Span<double>{y, 3}));
    REQUIRE_FALSE(op.apply_adjoint(crd::containers::ConstSpan<double>{x, 5}, crd::containers::Span<double>{y, 3}));
    REQUIRE(op.n_rows() == 3);
    REQUIRE(op.n_cols() == 5);
    REQUIRE_FALSE(op.is_square());
}

TEST_CASE("LinearOp size-mismatched apply returns false", "[hesap][linear_op]")
{
    IdentityLinearOp<double> op(4);
    const double x[3] = {1, 2, 3};
    double y[4] = {0, 0, 0, 0};
    REQUIRE_FALSE(op.apply(crd::containers::ConstSpan<double>{x, 3}, crd::containers::Span<double>{y, 4}));
}
