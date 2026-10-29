// The Eigen peer's sparse objects and operations (eigen_peer.hpp).

#include "eigen_peer_objects.hpp"

#include <Eigen/Sparse>
#include <vector>

namespace crd_peer::eigen
{
struct SpF32
{
    Eigen::SparseMatrix<float, Eigen::RowMajor> a;
};
struct SpF64
{
    Eigen::SparseMatrix<double, Eigen::RowMajor> a;
};
struct SpColF64
{
    Eigen::SparseMatrix<double> a;
};
struct RowMatF64
{
    Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> m;
};

namespace
{
template <typename Sparse> void assemble(Sparse& a, Index rows, Index cols, const Entry* entries, Index count)
{
    using Scalar = typename Sparse::Scalar;
    std::vector<Eigen::Triplet<Scalar>> triplets;
    triplets.reserve(static_cast<std::size_t>(count));
    for (Index k = 0; k < count; ++k)
    {
        triplets.emplace_back(entries[k].row, entries[k].col, static_cast<Scalar>(entries[k].value));
    }
    a.resize(static_cast<Eigen::Index>(rows), static_cast<Eigen::Index>(cols));
    a.setFromTriplets(triplets.begin(), triplets.end());
    a.makeCompressed();
}
} // namespace

SpF32* csr_f32(Index rows, Index cols, const Entry* entries, Index count)
{
    auto* object = new SpF32;
    assemble(object->a, rows, cols, entries, count);
    return object;
}

SpF64* csr_f64(Index rows, Index cols, const Entry* entries, Index count)
{
    auto* object = new SpF64;
    assemble(object->a, rows, cols, entries, count);
    return object;
}

SpColF64* csc_f64(Index rows, Index cols, const Entry* entries, Index count)
{
    auto* object = new SpColF64;
    assemble(object->a, rows, cols, entries, count);
    return object;
}

RowMatF64* row_mat(Index rows, Index cols, const double* data)
{
    auto* object = new RowMatF64;
    if (data == nullptr)
    {
        object->m.setZero(rows, cols);
    }
    else
    {
        object->m =
            Eigen::Map<const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(data, rows, cols);
    }
    return object;
}

void destroy(SpF32* object) noexcept
{
    delete object;
}

void destroy(SpF64* object) noexcept
{
    delete object;
}

void destroy(SpColF64* object) noexcept
{
    delete object;
}

void destroy(RowMatF64* object) noexcept
{
    delete object;
}

Index nnz(const SpF64* a) noexcept
{
    return a->a.nonZeros();
}

Index nnz(const SpColF64* a) noexcept
{
    return a->a.nonZeros();
}

void spmv(const SpF32* a, const VecF32* x, VecF32* y)
{
    y->v.noalias() = a->a * x->v;
}

void spmv(const SpF64* a, const VecF64* x, VecF64* y)
{
    y->v.noalias() = a->a * x->v;
}

void spgemm(const SpF64* a, const SpF64* b, SpF64* c)
{
    c->a = a->a * b->a;
}

void spmm(const SpF64* a, const RowMatF64* b, RowMatF64* c)
{
    c->m.noalias() = a->a * b->m;
}

void sampled_dots(const RowMatF64* x, const RowMatF64* y, Index rows, const std::uint32_t* outer,
                  const std::uint32_t* inner, double* out)
{
    for (Index i = 0; i < rows; ++i)
    {
        for (std::uint32_t k = outer[i]; k < outer[i + 1]; ++k)
        {
            out[k] = x->m.row(i).dot(y->m.row(inner[k]));
        }
    }
}

double masked_dense_sum(const RowMatF64* x, const RowMatF64* y, Index rows, const std::uint32_t* outer,
                        const std::uint32_t* inner)
{
    const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> d = x->m * y->m.transpose();
    double sum = 0.0;
    for (Index i = 0; i < rows; ++i)
    {
        for (std::uint32_t k = outer[i]; k < outer[i + 1]; ++k)
        {
            sum += d(i, inner[k]);
        }
    }
    return sum;
}
} // namespace crd_peer::eigen
