#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Matrix<T, Layout> — owning dense matrix. Body for the v0a shell.
//
// Phase 3.1.6 v0c (2026-05-19): the BLAS L2 surface (gemv / ger / symv /
// hemv / trmv / trsv / ...) consumes Matrix<T, L>. Layout is a non-type
// template parameter (RowMajor by default per the v0a phase-doc choice)
// so the microkernel choice is compile-time per layout (D21).
//
// Indexing:
//   RowMajor: at(i, j) = data[i * ld + j], ld = num_cols
//   ColMajor: at(i, j) = data[j * ld + i], ld = num_rows
//
// `ld()` returns the leading dimension. For a contiguous Matrix, this is
// num_cols (RowMajor) or num_rows (ColMajor). MatrixView<T, L> sub-views
// preserve the parent's `ld` so column / row strides are correct.
//
// Move-only (D15 from Vector<T>). Explicit clone() for deep copy.
// -----------------------------------------------------------------------

template <typename T, Layout L = Layout::RowMajor>
class MatrixView;  // forward declaration

template <typename T, Layout L = Layout::RowMajor>
class Matrix
{
public:
    using value_type = T;
    static constexpr Layout layout = L;

    explicit Matrix(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    Matrix(crd::memory::IAllocator* alloc, crd::usize rows, crd::usize cols)
        : m_alloc(alloc), m_rows(rows), m_cols(cols)
    {
        const crd::usize n = rows * cols;
        if (n > 0)
        {
            const crd::usize align = alignof(T) > 16 ? alignof(T) : 16;
            m_data = static_cast<T*>(m_alloc->allocate(n * sizeof(T), align));
            if constexpr (std::is_trivially_constructible_v<T>)
            {
                std::memset(m_data, 0, n * sizeof(T));
            }
            else
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    new (m_data + i) T{};
                }
            }
        }
    }

    Matrix(crd::memory::IAllocator* alloc, crd::usize rows, crd::usize cols, std::initializer_list<T> il)
        : Matrix(alloc, rows, cols)
    {
        CRD_ASSERT_MSG(il.size() == rows * cols, "Matrix initializer_list size != rows*cols");
        crd::usize i = 0;
        for (const T& v : il)
        {
            m_data[i++] = v;
        }
    }

    ~Matrix() { release(); }

    Matrix(const Matrix&) = delete;
    Matrix& operator=(const Matrix&) = delete;

    Matrix(Matrix&& other) noexcept
        : m_alloc(other.m_alloc), m_data(other.m_data), m_rows(other.m_rows), m_cols(other.m_cols)
    {
        other.m_data = nullptr;
        other.m_rows = 0;
        other.m_cols = 0;
    }

    Matrix& operator=(Matrix&& other) noexcept
    {
        if (this != &other)
        {
            release();
            m_alloc = other.m_alloc;
            m_data = other.m_data;
            m_rows = other.m_rows;
            m_cols = other.m_cols;
            other.m_data = nullptr;
            other.m_rows = 0;
            other.m_cols = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize rows() const noexcept { return m_rows; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_cols; }
    [[nodiscard]] crd::usize size() const noexcept { return m_rows * m_cols; }
    [[nodiscard]] crd::usize ld() const noexcept
    {
        if constexpr (L == Layout::RowMajor)
        {
            return m_cols;
        }
        else
        {
            return m_rows;
        }
    }
    [[nodiscard]] bool is_square() const noexcept { return m_rows == m_cols; }
    [[nodiscard]] T* data() noexcept { return m_data; }
    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

    [[nodiscard]] T& at(crd::usize i, crd::usize j) noexcept
    {
        CRD_ASSERT_MSG(i < m_rows && j < m_cols, "Matrix::at out of range");
        if constexpr (L == Layout::RowMajor)
        {
            return m_data[i * m_cols + j];
        }
        else
        {
            return m_data[j * m_rows + i];
        }
    }

    [[nodiscard]] const T& at(crd::usize i, crd::usize j) const noexcept
    {
        CRD_ASSERT_MSG(i < m_rows && j < m_cols, "Matrix::at out of range");
        if constexpr (L == Layout::RowMajor)
        {
            return m_data[i * m_cols + j];
        }
        else
        {
            return m_data[j * m_rows + i];
        }
    }

    [[nodiscard]] T& operator()(crd::usize i, crd::usize j) noexcept { return at(i, j); }
    [[nodiscard]] const T& operator()(crd::usize i, crd::usize j) const noexcept { return at(i, j); }

    [[nodiscard]] Matrix clone(crd::memory::IAllocator* dst_alloc = nullptr) const
    {
        Matrix out(dst_alloc != nullptr ? dst_alloc : m_alloc, m_rows, m_cols);
        const crd::usize n = m_rows * m_cols;
        for (crd::usize i = 0; i < n; ++i)
        {
            out.m_data[i] = m_data[i];
        }
        return out;
    }

    void fill(const T& v) noexcept
    {
        const crd::usize n = m_rows * m_cols;
        for (crd::usize i = 0; i < n; ++i)
        {
            m_data[i] = v;
        }
    }

    void set_zero() noexcept { fill(T{}); }

    // Square only; sets A = I.
    void set_identity() noexcept
    {
        CRD_ASSERT_MSG(m_rows == m_cols, "set_identity: matrix must be square");
        set_zero();
        for (crd::usize i = 0; i < m_rows; ++i)
        {
            at(i, i) = T{1};
        }
    }

    [[nodiscard]] MatrixView<T, L> view() noexcept;
    [[nodiscard]] MatrixView<const T, L> view() const noexcept;
    // Const-view accessor. Always returns `MatrixView<const T, L>` regardless of
    // the parent's const-ness. Use this when passing a Matrix to a function
    // that takes `MatrixView<const T, L>` (e.g. gemv) — template-arg deduction
    // does not implicitly cast through the non-const→const converting ctor.
    [[nodiscard]] MatrixView<const T, L> cview() const noexcept;

private:
    void release() noexcept
    {
        if (m_data != nullptr)
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                const crd::usize n = m_rows * m_cols;
                for (crd::usize i = 0; i < n; ++i)
                {
                    m_data[i].~T();
                }
            }
            m_alloc->deallocate(m_data);
            m_data = nullptr;
        }
    }

    crd::memory::IAllocator* m_alloc = nullptr;
    T* m_data = nullptr;
    crd::usize m_rows = 0;
    crd::usize m_cols = 0;
};

// -----------------------------------------------------------------------
// MatrixView<T, Layout> — non-owning sub-view.
// Carries m_ld so sub-views preserve the parent's stride.
// -----------------------------------------------------------------------
template <typename T, Layout L>
class MatrixView
{
public:
    using value_type = T;
    static constexpr Layout layout = L;

    constexpr MatrixView() noexcept = default;

    constexpr MatrixView(T* data, crd::usize rows, crd::usize cols, crd::usize ld) noexcept
        : m_data(data), m_rows(rows), m_cols(cols), m_ld(ld)
    {
    }

    // Implicit conversion from MatrixView<NonConstT, L> to MatrixView<const NonConstT, L>.
    // Lets `gemv(MatrixView<const T, L>)` accept `Matrix::view()` results which
    // are `MatrixView<T, L>` when the Matrix is non-const.
    template <typename U,
              typename = std::enable_if_t<std::is_const_v<T> && std::is_same_v<U, std::remove_const_t<T>>>>
    constexpr MatrixView(MatrixView<U, L> other) noexcept
        : m_data(other.data()), m_rows(other.rows()), m_cols(other.cols()), m_ld(other.ld())
    {
    }

    [[nodiscard]] T* data() noexcept { return m_data; }
    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] crd::usize rows() const noexcept { return m_rows; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_cols; }
    [[nodiscard]] crd::usize ld() const noexcept { return m_ld; }
    [[nodiscard]] bool is_square() const noexcept { return m_rows == m_cols; }

    [[nodiscard]] T& at(crd::usize i, crd::usize j) const noexcept
    {
        CRD_ASSERT_MSG(i < m_rows && j < m_cols, "MatrixView::at out of range");
        if constexpr (L == Layout::RowMajor)
        {
            return m_data[i * m_ld + j];
        }
        else
        {
            return m_data[j * m_ld + i];
        }
    }

    [[nodiscard]] T& operator()(crd::usize i, crd::usize j) const noexcept { return at(i, j); }

    // Sub-view that preserves the underlying stride.
    [[nodiscard]] MatrixView sub_view(crd::usize r0, crd::usize c0, crd::usize nr, crd::usize nc) const noexcept
    {
        CRD_ASSERT_MSG(r0 + nr <= m_rows && c0 + nc <= m_cols, "MatrixView::sub_view out of range");
        T* p;
        if constexpr (L == Layout::RowMajor)
        {
            p = m_data + r0 * m_ld + c0;
        }
        else
        {
            p = m_data + c0 * m_ld + r0;
        }
        return MatrixView{p, nr, nc, m_ld};
    }

private:
    T* m_data = nullptr;
    crd::usize m_rows = 0;
    crd::usize m_cols = 0;
    crd::usize m_ld = 0;
};

// Deferred Matrix::view definitions (MatrixView is now complete).
template <typename T, Layout L>
MatrixView<T, L> Matrix<T, L>::view() noexcept
{
    return MatrixView<T, L>{m_data, m_rows, m_cols, ld()};
}

template <typename T, Layout L>
MatrixView<const T, L> Matrix<T, L>::view() const noexcept
{
    return MatrixView<const T, L>{m_data, m_rows, m_cols, ld()};
}

template <typename T, Layout L>
MatrixView<const T, L> Matrix<T, L>::cview() const noexcept
{
    return MatrixView<const T, L>{m_data, m_rows, m_cols, ld()};
}

} // namespace crd::hesap::dense
