// The Eigen peer's sparse operations (eigen_peer.hpp).

#include "eigen_peer_objects.hpp"

#include <Eigen/Sparse>
#include <complex>
#include <vector>

namespace crd_peer::eigen
{
namespace
{
template <typename Scalar> Scalar value_of(const Entry& e)
{
    return static_cast<Scalar>(e.value);
}

template <typename Scalar> Scalar value_of(const ComplexEntry& e)
{
    using Part = typename Scalar::value_type;
    return Scalar{static_cast<Part>(e.re), static_cast<Part>(e.im)};
}

template <typename Sparse, typename Coordinate>
void assemble(Sparse& a, Index rows, Index cols, const Coordinate* entries, Index count)
{
    using Scalar = typename Sparse::Scalar;
    std::vector<Eigen::Triplet<Scalar>> triplets;
    triplets.reserve(static_cast<std::size_t>(count));
    for (Index k = 0; k < count; ++k)
    {
        triplets.emplace_back(entries[k].row, entries[k].col, value_of<Scalar>(entries[k]));
    }
    a.resize(static_cast<Eigen::Index>(rows), static_cast<Eigen::Index>(cols));
    a.setFromTriplets(triplets.begin(), triplets.end());
    a.makeCompressed();
}

template <typename Vector, typename Part> Vector* complex_vector(Index size, const Part* data)
{
    auto* object = new Vector;
    if (data == nullptr)
    {
        object->v.setZero(size);
    }
    else
    {
        object->v = Eigen::Map<const Eigen::Matrix<std::complex<Part>, Eigen::Dynamic, 1>>(
            reinterpret_cast<const std::complex<Part>*>(data), size);
    }
    return object;
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

SpC32* csr_c32(Index rows, Index cols, const ComplexEntry* entries, Index count)
{
    auto* object = new SpC32;
    assemble(object->a, rows, cols, entries, count);
    return object;
}

SpC64* csr_c64(Index rows, Index cols, const ComplexEntry* entries, Index count)
{
    auto* object = new SpC64;
    assemble(object->a, rows, cols, entries, count);
    return object;
}

VecC32* vec_c32(Index size, const float* data)
{
    return complex_vector<VecC32>(size, data);
}

VecC64* vec_c64(Index size, const double* data)
{
    return complex_vector<VecC64>(size, data);
}

SpColF64* csc_f64(Index rows, Index cols, const Entry* entries, Index count)
{
    auto* object = new SpColF64;
    assemble(object->a, rows, cols, entries, count);
    return object;
}

SpColF64* csc_f64(Index rows, Index cols, const std::uint32_t* outer, const std::uint32_t* inner, const double* values)
{
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(outer[rows]);
    for (Index r = 0; r < rows; ++r)
    {
        for (std::uint32_t k = outer[r]; k < outer[r + 1]; ++k)
        {
            triplets.emplace_back(static_cast<int>(r), static_cast<int>(inner[k]), values[k]);
        }
    }
    auto* object = new SpColF64;
    object->a.resize(static_cast<Eigen::Index>(rows), static_cast<Eigen::Index>(cols));
    object->a.setFromTriplets(triplets.begin(), triplets.end());
    object->a.makeCompressed();
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

void destroy(SpC32* object) noexcept
{
    delete object;
}

void destroy(SpC64* object) noexcept
{
    delete object;
}

void destroy(VecC32* object) noexcept
{
    delete object;
}

void destroy(VecC64* object) noexcept
{
    delete object;
}

const float* data(const VecC32* v) noexcept
{
    return reinterpret_cast<const float*>(v->v.data());
}

const double* data(const VecC64* v) noexcept
{
    return reinterpret_cast<const double*>(v->v.data());
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

void spmv(const SpC32* a, const VecC32* x, VecC32* y)
{
    y->v.noalias() = a->a * x->v;
}

void spmv(const SpC64* a, const VecC64* x, VecC64* y)
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
