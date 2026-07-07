#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-l: tensor I/O + interop.
//
//   - .npy read/write   — the NumPy array format (numpy.lib.format): magic
//     "\x93NUMPY", version 1.0 (u16 header len) / 2.0+ (u32 header len), a
//     python-dict header {'descr','fortran_order','shape'} space-padded so
//     the payload starts on a 64-byte boundary, then raw C-order bytes.
//     Write emits v1.0 and upgrades to v2.0 exactly when the header cannot
//     fit u16 (numpy's own rule). fortran_order=True is rejected with
//     Unsupported (C-order contract), big-endian descr likewise.
//   - .npz read/write   — a zip container of .npy members (numpy convention:
//     key 'a' → member 'a.npy'). WRITE emits STORED members — byte-for-byte
//     what np.savez does. READ accepts STORED and DEFLATE members
//     (np.savez_compressed) via the RFC 1951 inflate in detail/io_zip.hpp;
//     CRC-32 is verified on every member read. No zip64 (Unsupported past
//     4 GiB — np.savez has the same container limits).
//   - safetensors read/write — the huggingface format: u64-LE header length,
//     a JSON header {"name":{"dtype","shape","data_offsets"},...} with
//     optional "__metadata__" (string→string), then the raw byte buffer.
//     Write mirrors the reference library: metadata first, tensors
//     name-sorted with contiguous offsets from 0, header space-padded to an
//     8-byte multiple. bf16/f16/fp8 ride the v14-a dtypes.hpp storage types.
//   - philox_fill_uniform — counter-RNG tensor fills, reproducible BY
//     CONSTRUCTION: element at canonical logical index k gets the k-th draw
//     of PhiloxRng(seed, stream) — a pure function of (seed, stream, k), so
//     any worker count / chunking / striding produces identical bits.
//
// Error contract (ADR-0095 pillars): every fallible operation returns
// TensorStatus and is noexcept — file errors are status codes, never
// exceptions. Mapping: BadInput = malformed bytes (magic/CRC/truncation/
// JSON) or a filesystem failure; Unsupported = legal-but-out-of-scope
// (fortran_order, big-endian, zip64, unknown/unsupported dtype);
// RankOverflow = rank > kMaxRank; ShapeMismatch = typed read against a
// different dtype/shape; NotContiguous = typed write of a strided view;
// AllocFailed = allocator refusal.
//
// Lifetime (the engine-wide Span discipline — no borrowed-lifetime members
// hiding behind owning types): NpyView / SafetensorsFile / NpzReader hand
// out spans that VIEW the parsed source bytes — the caller keeps the byte
// buffer alive while any view is used. Typed reads (npy_read<T>, ::read<T>)
// copy into an owning Tensor and have no residual dependency.
//
// File I/O rides the house platform API (crd::platform::fs — the same
// read_file_binary/write_file_binary the asset cooker uses).
// ---------------------------------------------------------------------------

#include "tensor.hpp"

#include "detail/io_zip.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/stats/philox.hpp> // include-only (header-only constexpr) — NO link edge (ADR-0096 §1)
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <cstdio> // streaming file paths (single-pass, no staging buffer) — house precedent: perf capture, log sinks
#include <cstring>

#if defined(_WIN32)
#include <io.h> // _chsize_s / _fileno (truncate-in-place on the overwrite path)
#else
#include <unistd.h> // ftruncate
#endif

namespace crd::hesap::tensor
{

// =======================================================================
// IoDtype — the interchange dtype tag. PINNED + APPEND-ONLY (the ADR-0084
// posture): values are stored in cooked TNSR artifacts; a renumber silently
// mis-types every artifact on disk. New dtypes append at the end.
// =======================================================================
enum class IoDtype : crd::u8
{
    F32 = 0,
    F64 = 1,
    F16 = 2,   // storage type — u16 bit pattern (dtypes.hpp convert APIs)
    Bf16 = 3,  // storage type — u16 bit pattern
    I8 = 4,
    I16 = 5,
    I32 = 6,
    I64 = 7,
    U8 = 8,
    U16 = 9,
    U32 = 10,
    U64 = 11,
    Bool = 12, // 1 byte per element, 0/1 (the array-library convention)
    C32 = 13,  // complex<f32> interleaved
    C64 = 14,  // complex<f64> interleaved
    Fp8E4m3 = 15, // OCP e4m3fn (crd::math convention — NO inf, single NaN)
    Fp8E5m2 = 16,
};

[[nodiscard]] constexpr crd::u32 io_dtype_size(IoDtype d) noexcept
{
    switch (d)
    {
    case IoDtype::F32:
        return 4U;
    case IoDtype::F64:
        return 8U;
    case IoDtype::F16:
    case IoDtype::Bf16:
        return 2U;
    case IoDtype::I8:
    case IoDtype::U8:
    case IoDtype::Bool:
    case IoDtype::Fp8E4m3:
    case IoDtype::Fp8E5m2:
        return 1U;
    case IoDtype::I16:
    case IoDtype::U16:
        return 2U;
    case IoDtype::I32:
    case IoDtype::U32:
        return 4U;
    case IoDtype::I64:
    case IoDtype::U64:
        return 8U;
    case IoDtype::C32:
        return 8U;
    case IoDtype::C64:
        return 16U;
    }
    return 0U;
}

[[nodiscard]] constexpr const char* io_dtype_name(IoDtype d) noexcept
{
    switch (d)
    {
    case IoDtype::F32:
        return "f32";
    case IoDtype::F64:
        return "f64";
    case IoDtype::F16:
        return "f16";
    case IoDtype::Bf16:
        return "bf16";
    case IoDtype::I8:
        return "i8";
    case IoDtype::I16:
        return "i16";
    case IoDtype::I32:
        return "i32";
    case IoDtype::I64:
        return "i64";
    case IoDtype::U8:
        return "u8";
    case IoDtype::U16:
        return "u16";
    case IoDtype::U32:
        return "u32";
    case IoDtype::U64:
        return "u64";
    case IoDtype::Bool:
        return "bool";
    case IoDtype::C32:
        return "c32";
    case IoDtype::C64:
        return "c64";
    case IoDtype::Fp8E4m3:
        return "fp8_e4m3";
    case IoDtype::Fp8E5m2:
        return "fp8_e5m2";
    }
    return "?";
}

namespace iodetail
{
template <typename> inline constexpr bool kAlwaysFalseIo = false;
} // namespace iodetail

// Map a compute/index scalar type to its interchange tag. The storage-only
// tags (F16/Bf16/Bool/Fp8*) have no unique C++ type — use the *_as APIs with
// their bit-carrier (u16/u8) and an explicit IoDtype.
template <typename T> [[nodiscard]] constexpr IoDtype io_dtype_of() noexcept
{
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        return IoDtype::F32;
    }
    else if constexpr (std::is_same_v<T, crd::f64>)
    {
        return IoDtype::F64;
    }
    else if constexpr (std::is_same_v<T, crd::i8>)
    {
        return IoDtype::I8;
    }
    else if constexpr (std::is_same_v<T, crd::i16>)
    {
        return IoDtype::I16;
    }
    else if constexpr (std::is_same_v<T, crd::i32>)
    {
        return IoDtype::I32;
    }
    else if constexpr (std::is_same_v<T, crd::i64>)
    {
        return IoDtype::I64;
    }
    else if constexpr (std::is_same_v<T, crd::u8>)
    {
        return IoDtype::U8;
    }
    else if constexpr (std::is_same_v<T, crd::u16>)
    {
        return IoDtype::U16;
    }
    else if constexpr (std::is_same_v<T, crd::u32>)
    {
        return IoDtype::U32;
    }
    else if constexpr (std::is_same_v<T, crd::u64>)
    {
        return IoDtype::U64;
    }
    else if constexpr (std::is_same_v<T, c32>)
    {
        return IoDtype::C32;
    }
    else if constexpr (std::is_same_v<T, c64>)
    {
        return IoDtype::C64;
    }
    else
    {
        static_assert(iodetail::kAlwaysFalseIo<T>, "io_dtype_of: no interchange tag for this T");
        return IoDtype::F32;
    }
}

// =======================================================================
// shared detail
// =======================================================================
namespace iodetail
{

// Element count with overflow guard (8 u64 dims can overflow — BadInput).
[[nodiscard]] inline bool shape_elems(crd::containers::ConstSpan<crd::u64> shape, crd::u64 elem_size,
                                      crd::u64& elems, crd::u64& bytes) noexcept
{
    crd::u64 n = 1;
    for (const crd::u64 s : shape)
    {
        if (s != 0U && n > (~crd::u64{0}) / s)
        {
            return false;
        }
        n *= s;
    }
    if (elem_size != 0U && n > (~crd::u64{0}) / elem_size)
    {
        return false;
    }
    elems = n;
    bytes = n * elem_size;
    return true;
}

// Append raw bytes to an Array<u8> without CRD_FATAL on OOM (status path).
[[nodiscard]] inline bool append_bytes(crd::containers::Array<crd::u8>& dst, const void* src, crd::usize n) noexcept
{
    if (n == 0U)
    {
        return true;
    }
    const crd::usize old = dst.size();
    if (!dst.try_reserve(old + n))
    {
        return false;
    }
    dst.resize_uninitialized(old + n);
    std::memcpy(dst.data() + old, src, n);
    return true;
}

[[nodiscard]] inline bool append_cstr(crd::containers::Array<crd::u8>& dst, const char* s) noexcept
{
    return append_bytes(dst, s, std::strlen(s));
}

[[nodiscard]] inline bool append_u64_dec(crd::containers::Array<crd::u8>& dst, crd::u64 v) noexcept
{
    char buf[20];
    crd::u32 n = 0;
    do
    {
        buf[n++] = static_cast<char>('0' + static_cast<char>(v % 10U));
        v /= 10U;
    } while (v != 0U);
    char rev[20];
    for (crd::u32 i = 0; i < n; ++i)
    {
        rev[i] = buf[n - 1U - i];
    }
    return append_bytes(dst, rev, n);
}

// RAII stdio handle for the streaming typed file paths. The general-purpose
// byte-level helpers ride crd::platform::fs; these hot paths stream payloads
// directly between the file and tensor storage (single pass — the staging
// Array would double every multi-GB weight copy).
// HOME-> flag: crd::platform::fs has no read-into-caller-span primitive; when
// it grows one (read_file_into/write_file_from), re-point this detail there.
class CFile
{
public:
    // Whole-file overwrite tag: open "r+b" (in-place rewrite) and fall back
    // to "wb". Truncate-open evicts the file's page-cache pages, and
    // re-allocating them costs ~3x on the write path (measured 2.0 vs 7.2
    // GB/s on a cached 512 MB rewrite); in-place rewrite reuses them. The
    // caller MUST truncate_here() after the last byte so stale tails vanish
    // — the resulting bytes are identical either way (deterministic).
    struct Overwrite
    {
    };

    // fopen_s on Windows (the perf/capture.cpp precedent — C4996-clean), fopen
    // elsewhere; needs a NUL-terminated native path (UTF-8 on POSIX)
    [[nodiscard]] static std::FILE* open_native(const char* p, const char* mode) noexcept
    {
#if defined(_WIN32)
        std::FILE* f = nullptr;
        return fopen_s(&f, p, mode) == 0 ? f : nullptr;
#else
        return std::fopen(p, mode);
#endif
    }

    CFile(crd::containers::StringView path, const char* mode, crd::memory::IAllocator* alloc) noexcept
    {
        crd::containers::String p(alloc);
        if (!p.try_reserve(path.size()))
        {
            return;
        }
        p.append(path.data(), path.size());
        m_f = open_native(p.c_str(), mode);
    }

    CFile(crd::containers::StringView path, Overwrite, crd::memory::IAllocator* alloc) noexcept
    {
        crd::containers::String p(alloc);
        if (!p.try_reserve(path.size()))
        {
            return;
        }
        p.append(path.data(), path.size());
        m_f = open_native(p.c_str(), "r+b"); // in-place rewrite when the file exists
        if (m_f == nullptr)
        {
            m_f = open_native(p.c_str(), "wb");
        }
    }

    // Flush and cut the file at the current position (the Overwrite tail rule).
    [[nodiscard]] bool truncate_here() const noexcept
    {
        if (std::fflush(m_f) != 0)
        {
            return false;
        }
#if defined(_WIN32)
        const long long pos = _ftelli64(m_f);
        return pos >= 0 && _chsize_s(_fileno(m_f), pos) == 0;
#else
        const long pos = std::ftell(m_f);
        return pos >= 0 && ::ftruncate(::fileno(m_f), pos) == 0;
#endif
    }
    CFile(const CFile&) = delete;
    CFile& operator=(const CFile&) = delete;
    ~CFile()
    {
        if (m_f != nullptr)
        {
            std::fclose(m_f);
        }
    }
    [[nodiscard]] bool ok() const noexcept { return m_f != nullptr; }
    [[nodiscard]] std::FILE* get() const noexcept { return m_f; }

    [[nodiscard]] bool read_exact(void* dst, crd::u64 n) const noexcept
    {
        return n == 0U || std::fread(dst, 1U, static_cast<crd::usize>(n), m_f) == n;
    }
    [[nodiscard]] bool write_exact(const void* src, crd::u64 n) const noexcept
    {
        return n == 0U || std::fwrite(src, 1U, static_cast<crd::usize>(n), m_f) == n;
    }
    [[nodiscard]] bool at_eof() const noexcept { return std::fgetc(m_f) == EOF; }
    [[nodiscard]] bool seek(crd::u64 pos) const noexcept
    {
#if defined(_WIN32)
        return _fseeki64(m_f, static_cast<long long>(pos), SEEK_SET) == 0; // long is 32-bit on Windows
#else
        return std::fseek(m_f, static_cast<long>(pos), SEEK_SET) == 0;
#endif
    }

private:
    std::FILE* m_f = nullptr;
};

// Build an owning Tensor<T> from a dtype-checked payload (the typed-read tail
// shared by npy / npz / safetensors / TNSR).
template <typename T>
[[nodiscard]] TensorStatus materialize(crd::memory::IAllocator* alloc, IoDtype dtype, IoDtype expected,
                                       crd::containers::ConstSpan<crd::u64> shape,
                                       crd::containers::ConstSpan<crd::u8> payload, Tensor<T>& out) noexcept
{
    if (dtype != expected || io_dtype_size(dtype) != sizeof(T))
    {
        return TensorStatus::ShapeMismatch;
    }
    Tensor<T> t(alloc);
    const TensorStatus st = t.resize(shape);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    if (payload.size() != t.size() * sizeof(T))
    {
        return TensorStatus::BadInput;
    }
    if (!payload.empty())
    {
        std::memcpy(t.data(), payload.data(), payload.size());
    }
    out = static_cast<Tensor<T>&&>(t);
    return TensorStatus::Ok;
}

} // namespace iodetail

// =======================================================================
// .npy — the NumPy array format
// =======================================================================

// Zero-copy parse result: shape header + a payload view INTO the source
// bytes (keep the source alive while the view is used).
struct NpyView
{
    IoDtype dtype = IoDtype::F32;
    crd::u32 rank = 0;
    crd::u64 shape[kMaxRank] = {};
    crd::containers::ConstSpan<crd::u8> payload{};

    [[nodiscard]] crd::containers::ConstSpan<crd::u64> shape_span() const noexcept { return {shape, rank}; }
};

namespace iodetail
{

// numpy descr string <-> IoDtype ('<' little / '|' none / '=' native accepted;
// '>' big-endian → caller maps to Unsupported).
[[nodiscard]] inline bool npy_descr_to_dtype(crd::containers::StringView descr, IoDtype& out) noexcept
{
    if (descr.size() < 2U || descr.size() > 4U)
    {
        return false;
    }
    const char order = descr[0];
    const crd::containers::StringView code = descr.substr(1);
    const bool one_byte = code == "i1" || code == "u1" || code == "b1";
    if (order != '<' && order != '=' && !(order == '|' && one_byte))
    {
        return false; // '>' and malformed orders fail here — caller decides the status
    }
    struct Row
    {
        const char* code;
        IoDtype dtype;
    };
    static constexpr Row rows[] = {
        {"f2", IoDtype::F16}, {"f4", IoDtype::F32},  {"f8", IoDtype::F64}, {"i1", IoDtype::I8},
        {"i2", IoDtype::I16}, {"i4", IoDtype::I32},  {"i8", IoDtype::I64}, {"u1", IoDtype::U8},
        {"u2", IoDtype::U16}, {"u4", IoDtype::U32},  {"u8", IoDtype::U64}, {"b1", IoDtype::Bool},
        {"c8", IoDtype::C32}, {"c16", IoDtype::C64},
    };
    for (const Row& r : rows)
    {
        if (code == r.code)
        {
            out = r.dtype;
            return true;
        }
    }
    return false;
}

// IoDtype -> the exact descr numpy itself writes (nullptr = not expressible
// in the npy spec: bf16/fp8 have no NumPy-native descr — they ride
// safetensors / DLPack / TNSR).
[[nodiscard]] constexpr const char* npy_descr_of(IoDtype d) noexcept
{
    switch (d)
    {
    case IoDtype::F16:
        return "<f2";
    case IoDtype::F32:
        return "<f4";
    case IoDtype::F64:
        return "<f8";
    case IoDtype::I8:
        return "|i1";
    case IoDtype::I16:
        return "<i2";
    case IoDtype::I32:
        return "<i4";
    case IoDtype::I64:
        return "<i8";
    case IoDtype::U8:
        return "|u1";
    case IoDtype::U16:
        return "<u2";
    case IoDtype::U32:
        return "<u4";
    case IoDtype::U64:
        return "<u8";
    case IoDtype::Bool:
        return "|b1";
    case IoDtype::C32:
        return "<c8";
    case IoDtype::C64:
        return "<c16";
    case IoDtype::Bf16:
    case IoDtype::Fp8E4m3:
    case IoDtype::Fp8E5m2:
        return nullptr;
    }
    return nullptr;
}

// Minimal parser over the python-dict npy header (numpy emits exactly the
// three keys 'descr'/'fortran_order'/'shape'; any other shape of header is
// not a numpy file we accept).
class NpyHeaderParser
{
public:
    NpyHeaderParser(const char* begin, const char* end) noexcept : m_p(begin), m_end(end) {}

    void skip_ws() noexcept
    {
        while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r'))
        {
            ++m_p;
        }
    }

    [[nodiscard]] bool accept(char c) noexcept
    {
        skip_ws();
        if (m_p < m_end && *m_p == c)
        {
            ++m_p;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool quoted(crd::containers::StringView& out) noexcept
    {
        skip_ws();
        if (m_p >= m_end || (*m_p != '\'' && *m_p != '"'))
        {
            return false;
        }
        const char q = *m_p++;
        const char* start = m_p;
        while (m_p < m_end && *m_p != q)
        {
            ++m_p;
        }
        if (m_p >= m_end)
        {
            return false;
        }
        out = crd::containers::StringView{start, static_cast<crd::usize>(m_p - start)};
        ++m_p;
        return true;
    }

    [[nodiscard]] bool literal(const char* s) noexcept
    {
        skip_ws();
        const crd::usize n = std::strlen(s);
        if (static_cast<crd::usize>(m_end - m_p) < n || std::memcmp(m_p, s, n) != 0)
        {
            return false;
        }
        m_p += n;
        return true;
    }

    [[nodiscard]] bool number(crd::u64& out) noexcept
    {
        skip_ws();
        if (m_p >= m_end || *m_p < '0' || *m_p > '9')
        {
            return false;
        }
        crd::u64 v = 0;
        while (m_p < m_end && *m_p >= '0' && *m_p <= '9')
        {
            const crd::u64 digit = static_cast<crd::u64>(*m_p - '0');
            if (v > ((~crd::u64{0}) - digit) / 10U)
            {
                return false;
            }
            v = v * 10U + digit;
            ++m_p;
        }
        out = v;
        return true;
    }

    [[nodiscard]] bool peek(char c) noexcept
    {
        skip_ws();
        return m_p < m_end && *m_p == c;
    }

private:
    const char* m_p;
    const char* m_end;
};

// Parse the npy header dict text -> dtype/rank/shape (shared by the
// in-memory parser and the streaming file reader).
[[nodiscard]] inline TensorStatus npy_parse_header_dict(const char* h, crd::u64 header_len, IoDtype& out_dtype,
                                                        crd::u32& out_rank, crd::u64* out_shape) noexcept
{
    NpyHeaderParser p(h, h + header_len);
    crd::containers::StringView descr{};
    bool fortran = false;
    crd::u64 shape[kMaxRank] = {};
    crd::u32 rank = 0;
    bool have_descr = false;
    bool have_fortran = false;
    bool have_shape = false;

    if (!p.accept('{'))
    {
        return TensorStatus::BadInput;
    }
    while (!p.peek('}'))
    {
        crd::containers::StringView key{};
        if (!p.quoted(key) || !p.accept(':'))
        {
            return TensorStatus::BadInput;
        }
        if (key == "descr")
        {
            if (have_descr || !p.quoted(descr))
            {
                return TensorStatus::BadInput; // structured descr lists land here too
            }
            have_descr = true;
        }
        else if (key == "fortran_order")
        {
            if (have_fortran)
            {
                return TensorStatus::BadInput;
            }
            if (p.literal("False"))
            {
                fortran = false;
            }
            else if (p.literal("True"))
            {
                fortran = true;
            }
            else
            {
                return TensorStatus::BadInput;
            }
            have_fortran = true;
        }
        else if (key == "shape")
        {
            if (have_shape || !p.accept('('))
            {
                return TensorStatus::BadInput;
            }
            while (!p.peek(')'))
            {
                crd::u64 dim = 0;
                if (!p.number(dim))
                {
                    return TensorStatus::BadInput;
                }
                if (rank >= kMaxRank)
                {
                    return TensorStatus::RankOverflow;
                }
                shape[rank++] = dim;
                if (!p.accept(','))
                {
                    break; // last dim without trailing comma
                }
            }
            if (!p.accept(')'))
            {
                return TensorStatus::BadInput;
            }
            have_shape = true;
        }
        else
        {
            return TensorStatus::BadInput; // not a numpy-written header
        }
        if (!p.accept(','))
        {
            break; // final entry without trailing comma
        }
    }
    if (!p.accept('}') || !have_descr || !have_fortran || !have_shape)
    {
        return TensorStatus::BadInput;
    }
    if (fortran)
    {
        return TensorStatus::Unsupported; // C-order contract (see header comment)
    }
    if (!npy_descr_to_dtype(descr, out_dtype))
    {
        return TensorStatus::Unsupported; // big-endian or non-basic descr
    }
    out_rank = rank;
    for (crd::u32 d = 0; d < rank; ++d)
    {
        out_shape[d] = shape[d];
    }
    return TensorStatus::Ok;
}

// The npy preamble + padded header dict (numpy's exact bytes: v1.0, or v2.0
// exactly when the dict cannot fit u16; payload starts 64-byte aligned).
[[nodiscard]] inline TensorStatus npy_build_header(crd::memory::IAllocator* alloc, IoDtype dtype,
                                                   crd::containers::ConstSpan<crd::u64> shape,
                                                   crd::containers::Array<crd::u8>& out) noexcept
{
    const char* descr = npy_descr_of(dtype);
    if (descr == nullptr)
    {
        return TensorStatus::Unsupported; // bf16/fp8 are not expressible in npy
    }
    if (shape.size() > kMaxRank)
    {
        return TensorStatus::RankOverflow;
    }
    crd::containers::Array<crd::u8> dict(alloc);
    bool ok = append_cstr(dict, "{'descr': '");
    ok = ok && append_cstr(dict, descr);
    ok = ok && append_cstr(dict, "', 'fortran_order': False, 'shape': (");
    for (crd::usize d = 0; d < shape.size(); ++d)
    {
        ok = ok && append_u64_dec(dict, shape[d]);
        if (shape.size() == 1U || d + 1U < shape.size())
        {
            ok = ok && append_cstr(dict, d + 1U < shape.size() ? ", " : ",");
        }
    }
    ok = ok && append_cstr(dict, "), }");
    if (!ok)
    {
        return TensorStatus::AllocFailed;
    }

    // v1.0 preamble is 10 bytes; pad dict + '\n' so the total is 64-aligned
    crd::u64 preamble = 10U;
    crd::u64 unpadded = preamble + dict.size() + 1U;
    crd::u64 header_len = ((unpadded + 63U) / 64U) * 64U - preamble;
    crd::u8 major = 1U;
    if (header_len > 0xFFFFU) // numpy's v2.0 upgrade rule
    {
        major = 2U;
        preamble = 12U;
        unpadded = preamble + dict.size() + 1U;
        header_len = ((unpadded + 63U) / 64U) * 64U - preamble;
    }
    crd::containers::Array<crd::u8> hdr(alloc);
    if (!hdr.try_reserve(static_cast<crd::usize>(preamble + header_len)))
    {
        return TensorStatus::AllocFailed;
    }
    hdr.resize_uninitialized(static_cast<crd::usize>(preamble + header_len));
    crd::u8* w = hdr.data();
    static constexpr crd::u8 magic[6] = {0x93U, 'N', 'U', 'M', 'P', 'Y'};
    std::memcpy(w, magic, 6U);
    w[6] = major;
    w[7] = 0U;
    if (major == 1U)
    {
        wr_u16(w + 8U, static_cast<crd::u16>(header_len));
    }
    else
    {
        wr_u32(w + 8U, static_cast<crd::u32>(header_len));
    }
    std::memcpy(w + preamble, dict.data(), dict.size());
    for (crd::u64 i = preamble + dict.size(); i < preamble + header_len - 1U; ++i)
    {
        w[i] = ' ';
    }
    w[preamble + header_len - 1U] = '\n';
    out = static_cast<crd::containers::Array<crd::u8>&&>(hdr);
    return TensorStatus::Ok;
}

} // namespace iodetail

// Parse an in-memory .npy file. Zero-copy: out.payload views `bytes`.
[[nodiscard]] inline TensorStatus npy_parse(crd::containers::ConstSpan<crd::u8> bytes, NpyView& out) noexcept
{
    static constexpr crd::u8 magic[6] = {0x93U, 'N', 'U', 'M', 'P', 'Y'};
    if (bytes.size() < 10U || std::memcmp(bytes.data(), magic, 6U) != 0)
    {
        return TensorStatus::BadInput;
    }
    const crd::u8 major = bytes[6];
    crd::u64 header_len = 0;
    crd::u64 header_off = 0;
    if (major == 1U)
    {
        header_len = iodetail::rd_u16(bytes.data() + 8U);
        header_off = 10U;
    }
    else if (major == 2U || major == 3U)
    {
        if (bytes.size() < 12U)
        {
            return TensorStatus::BadInput;
        }
        header_len = iodetail::rd_u32(bytes.data() + 8U);
        header_off = 12U;
    }
    else
    {
        return TensorStatus::Unsupported;
    }
    if (header_off + header_len > bytes.size())
    {
        return TensorStatus::BadInput;
    }
    const char* h = reinterpret_cast<const char*>(bytes.data() + header_off);
    IoDtype dtype{};
    crd::u32 rank = 0;
    crd::u64 shape[kMaxRank] = {};
    const TensorStatus hst = iodetail::npy_parse_header_dict(h, header_len, dtype, rank, shape);
    if (hst != TensorStatus::Ok)
    {
        return hst;
    }
    crd::u64 elems = 0;
    crd::u64 payload_bytes = 0;
    if (!iodetail::shape_elems({shape, rank}, io_dtype_size(dtype), elems, payload_bytes))
    {
        return TensorStatus::BadInput;
    }
    if (bytes.size() - header_off - header_len != payload_bytes)
    {
        return TensorStatus::BadInput; // truncated or trailing junk
    }
    out.dtype = dtype;
    out.rank = rank;
    for (crd::u32 d = 0; d < rank; ++d)
    {
        out.shape[d] = shape[d];
    }
    out.payload = bytes.subspan(static_cast<crd::usize>(header_off + header_len));
    return TensorStatus::Ok;
}

// Encode an .npy file from a dtype tag + shape + raw C-order payload.
// Produces byte-for-byte what numpy writes (v1.0 header, v2.0 exactly when
// the dict cannot fit u16 — 64-byte payload alignment, trailing '\n').
[[nodiscard]] inline TensorStatus npy_encode(crd::memory::IAllocator* alloc, IoDtype dtype,
                                             crd::containers::ConstSpan<crd::u64> shape,
                                             crd::containers::ConstSpan<crd::u8> payload,
                                             crd::containers::Array<crd::u8>& out) noexcept
{
    crd::u64 elems = 0;
    crd::u64 payload_bytes = 0;
    if (!iodetail::shape_elems(shape, io_dtype_size(dtype), elems, payload_bytes))
    {
        return TensorStatus::BadInput;
    }
    if (payload.size() != payload_bytes)
    {
        return TensorStatus::BadInput;
    }
    crd::containers::Array<crd::u8> hdr(alloc);
    const TensorStatus st = iodetail::npy_build_header(alloc, dtype, shape, hdr);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    crd::containers::Array<crd::u8> file(alloc);
    if (!file.try_reserve(hdr.size() + payload.size()))
    {
        return TensorStatus::AllocFailed;
    }
    file.resize_uninitialized(hdr.size() + payload.size());
    std::memcpy(file.data(), hdr.data(), hdr.size());
    if (!payload.empty())
    {
        std::memcpy(file.data() + hdr.size(), payload.data(), payload.size());
    }
    out = static_cast<crd::containers::Array<crd::u8>&&>(file);
    return TensorStatus::Ok;
}

// Typed read: parse + exact-dtype check + copy into an owning Tensor<T>.
template <typename T>
[[nodiscard]] TensorStatus npy_read(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::u8> bytes,
                                    Tensor<T>& out) noexcept
{
    NpyView v;
    const TensorStatus st = npy_parse(bytes, v);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    return iodetail::materialize(alloc, v.dtype, io_dtype_of<T>(), v.shape_span(), v.payload, out);
}

// Bits-carrier read for the storage dtypes (f16/bf16/bool/fp8): T is the bit
// pattern type (u16/u8) and `expected` names the on-disk dtype.
template <typename T>
[[nodiscard]] TensorStatus npy_read_as(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::u8> bytes,
                                       IoDtype expected, Tensor<T>& out) noexcept
{
    NpyView v;
    const TensorStatus st = npy_parse(bytes, v);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    return iodetail::materialize(alloc, v.dtype, expected, v.shape_span(), v.payload, out);
}

// Typed write (C-contiguous view; strided views return NotContiguous).
template <typename T>
[[nodiscard]] TensorStatus npy_write(crd::memory::IAllocator* alloc, const TensorView<const T>& view,
                                     crd::containers::Array<crd::u8>& out) noexcept
{
    if (!view.is_contiguous())
    {
        return TensorStatus::NotContiguous;
    }
    const crd::containers::ConstSpan<crd::u8> payload{reinterpret_cast<const crd::u8*>(view.data()),
                                                      static_cast<crd::usize>(view.size()) * sizeof(T)};
    return npy_encode(alloc, io_dtype_of<T>(), view.shape(), payload, out);
}

template <typename T>
[[nodiscard]] TensorStatus npy_write_as(crd::memory::IAllocator* alloc, IoDtype dtype,
                                        crd::containers::ConstSpan<crd::u64> shape,
                                        crd::containers::ConstSpan<T> bits,
                                        crd::containers::Array<crd::u8>& out) noexcept
{
    if (io_dtype_size(dtype) != sizeof(T))
    {
        return TensorStatus::BadInput;
    }
    const crd::containers::ConstSpan<crd::u8> payload{reinterpret_cast<const crd::u8*>(bits.data()),
                                                      bits.size() * sizeof(T)};
    return npy_encode(alloc, dtype, shape, payload, out);
}

// =======================================================================
// file helpers (crd::platform::fs — the house binary file API)
// =======================================================================

[[nodiscard]] inline TensorStatus io_read_file(crd::containers::StringView path,
                                               crd::containers::Array<crd::u8>& out) noexcept
{
    if (!crd::platform::fs::read_file_binary(crd::platform::fs::Path{path}, out))
    {
        return TensorStatus::BadInput; // open/read failure (see the status-mapping doc above)
    }
    return TensorStatus::Ok;
}

[[nodiscard]] inline TensorStatus io_write_file(crd::containers::StringView path,
                                                crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    if (!crd::platform::fs::write_file_binary(crd::platform::fs::Path{path}, bytes))
    {
        return TensorStatus::BadInput;
    }
    return TensorStatus::Ok;
}

// Streaming typed read: header via small buffered reads, payload fread
// DIRECTLY into the owned tensor — single pass, no staging buffer (the
// staging Array would double every multi-GB weight copy).
template <typename T>
[[nodiscard]] TensorStatus npy_read_file(crd::memory::IAllocator* alloc, crd::containers::StringView path,
                                         Tensor<T>& out) noexcept
{
    const iodetail::CFile f(path, "rb", alloc);
    if (!f.ok())
    {
        return TensorStatus::BadInput;
    }
    crd::u8 pre[12];
    if (!f.read_exact(pre, 10U))
    {
        return TensorStatus::BadInput;
    }
    static constexpr crd::u8 magic[6] = {0x93U, 'N', 'U', 'M', 'P', 'Y'};
    if (std::memcmp(pre, magic, 6U) != 0)
    {
        return TensorStatus::BadInput;
    }
    const crd::u8 major = pre[6];
    crd::u64 header_len = 0;
    if (major == 1U)
    {
        header_len = iodetail::rd_u16(pre + 8U);
    }
    else if (major == 2U || major == 3U)
    {
        if (!f.read_exact(pre + 10U, 2U))
        {
            return TensorStatus::BadInput;
        }
        header_len = iodetail::rd_u32(pre + 8U);
    }
    else
    {
        return TensorStatus::Unsupported;
    }
    if (header_len > (1ULL << 24U)) // defensive sanity — numpy headers are tiny
    {
        return TensorStatus::BadInput;
    }
    crd::containers::Array<crd::u8> hdr(alloc);
    if (!hdr.try_reserve(static_cast<crd::usize>(header_len)))
    {
        return TensorStatus::AllocFailed;
    }
    hdr.resize_uninitialized(static_cast<crd::usize>(header_len));
    if (!f.read_exact(hdr.data(), header_len))
    {
        return TensorStatus::BadInput;
    }
    IoDtype dtype{};
    crd::u32 rank = 0;
    crd::u64 shape[kMaxRank] = {};
    const TensorStatus hst = iodetail::npy_parse_header_dict(reinterpret_cast<const char*>(hdr.data()),
                                                             header_len, dtype, rank, shape);
    if (hst != TensorStatus::Ok)
    {
        return hst;
    }
    if (dtype != io_dtype_of<T>() || io_dtype_size(dtype) != sizeof(T))
    {
        return TensorStatus::ShapeMismatch;
    }
    Tensor<T> t(alloc);
    const TensorStatus rst = t.resize({shape, rank});
    if (rst != TensorStatus::Ok)
    {
        return rst;
    }
    if (!f.read_exact(t.data(), t.size() * sizeof(T)) || !f.at_eof())
    {
        return TensorStatus::BadInput; // truncated or trailing junk
    }
    out = static_cast<Tensor<T>&&>(t);
    return TensorStatus::Ok;
}

// Streaming typed write: header + payload fwrite straight from the view's
// storage — single pass (byte-identical to npy_write + io_write_file).
template <typename T>
[[nodiscard]] TensorStatus npy_write_file(crd::memory::IAllocator* alloc, crd::containers::StringView path,
                                          const TensorView<const T>& view) noexcept
{
    if (!view.is_contiguous())
    {
        return TensorStatus::NotContiguous;
    }
    crd::containers::Array<crd::u8> hdr(alloc);
    const TensorStatus st = iodetail::npy_build_header(alloc, io_dtype_of<T>(), view.shape(), hdr);
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    const iodetail::CFile f(path, iodetail::CFile::Overwrite{}, alloc);
    if (!f.ok())
    {
        return TensorStatus::BadInput;
    }
    if (!f.write_exact(hdr.data(), hdr.size()) ||
        !f.write_exact(view.data(), static_cast<crd::u64>(view.size()) * sizeof(T)) || !f.truncate_here())
    {
        return TensorStatus::BadInput;
    }
    return TensorStatus::Ok;
}

// =======================================================================
// .npz — zip container of .npy members
// =======================================================================

// Writer: STORED members (exactly np.savez), deterministic bytes — fixed
// 1980-01-01 DOS stamp, no extra fields, members in add() order.
class NpzWriter
{
public:
    explicit NpzWriter(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc), m_members(alloc) {}

    // `name` is the numpy key; the zip member becomes "<name>.npy".
    [[nodiscard]] TensorStatus add(crd::containers::StringView name, IoDtype dtype,
                                   crd::containers::ConstSpan<crd::u64> shape,
                                   crd::containers::ConstSpan<crd::u8> payload) noexcept
    {
        Member m(m_alloc);
        const TensorStatus st = npy_encode(m_alloc, dtype, shape, payload, m.npy_bytes);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        if (!m.name.try_reserve(name.size() + 4U))
        {
            return TensorStatus::AllocFailed;
        }
        m.name.append(name.data(), name.size());
        m.name.append(".npy");
        m_members.push_back(static_cast<Member&&>(m));
        return TensorStatus::Ok;
    }

    template <typename T>
    [[nodiscard]] TensorStatus add(crd::containers::StringView name, const TensorView<const T>& view) noexcept
    {
        if (!view.is_contiguous())
        {
            return TensorStatus::NotContiguous;
        }
        const crd::containers::ConstSpan<crd::u8> payload{reinterpret_cast<const crd::u8*>(view.data()),
                                                          static_cast<crd::usize>(view.size()) * sizeof(T)};
        return add(name, io_dtype_of<T>(), view.shape(), payload);
    }

    [[nodiscard]] TensorStatus finish(crd::containers::Array<crd::u8>& out) noexcept
    {
        // exact size: locals (30 + name + data) + centrals (46 + name) + EOCD 22
        crd::u64 total = 22U;
        for (const Member& m : m_members)
        {
            total += 30U + m.name.size() + m.npy_bytes.size() + 46U + m.name.size();
        }
        if (total > 0xFFFFFFFFULL || m_members.size() > 0xFFFFU)
        {
            return TensorStatus::Unsupported; // no zip64 (matches np.savez limits)
        }
        crd::containers::Array<crd::u8> file(m_alloc);
        if (!file.try_reserve(static_cast<crd::usize>(total)))
        {
            return TensorStatus::AllocFailed;
        }
        file.resize_uninitialized(static_cast<crd::usize>(total));
        crd::u8* base = file.data();
        crd::usize w = 0;
        // local headers + data
        for (Member& m : m_members)
        {
            m.crc = iodetail::crc32(crd::containers::as_const_span(m.npy_bytes));
            m.local_offset = static_cast<crd::u32>(w);
            iodetail::wr_u32(base + w, 0x04034B50U);
            iodetail::wr_u16(base + w + 4U, 20U);  // version needed: 2.0
            iodetail::wr_u16(base + w + 6U, 0U);   // flags
            iodetail::wr_u16(base + w + 8U, 0U);   // method: STORED
            iodetail::wr_u16(base + w + 10U, 0U);  // DOS time 00:00:00
            iodetail::wr_u16(base + w + 12U, 0x21U); // DOS date 1980-01-01
            iodetail::wr_u32(base + w + 14U, m.crc);
            iodetail::wr_u32(base + w + 18U, static_cast<crd::u32>(m.npy_bytes.size()));
            iodetail::wr_u32(base + w + 22U, static_cast<crd::u32>(m.npy_bytes.size()));
            iodetail::wr_u16(base + w + 26U, static_cast<crd::u16>(m.name.size()));
            iodetail::wr_u16(base + w + 28U, 0U); // extra len
            w += 30U;
            std::memcpy(base + w, m.name.data(), m.name.size());
            w += m.name.size();
            if (m.npy_bytes.size() > 0U)
            {
                std::memcpy(base + w, m.npy_bytes.data(), m.npy_bytes.size());
            }
            w += m.npy_bytes.size();
        }
        // central directory
        const crd::usize cd_start = w;
        for (const Member& m : m_members)
        {
            iodetail::wr_u32(base + w, 0x02014B50U);
            iodetail::wr_u16(base + w + 4U, 20U); // version made by
            iodetail::wr_u16(base + w + 6U, 20U); // version needed
            iodetail::wr_u16(base + w + 8U, 0U);  // flags
            iodetail::wr_u16(base + w + 10U, 0U); // method
            iodetail::wr_u16(base + w + 12U, 0U); // time
            iodetail::wr_u16(base + w + 14U, 0x21U);
            iodetail::wr_u32(base + w + 16U, m.crc);
            iodetail::wr_u32(base + w + 20U, static_cast<crd::u32>(m.npy_bytes.size()));
            iodetail::wr_u32(base + w + 24U, static_cast<crd::u32>(m.npy_bytes.size()));
            iodetail::wr_u16(base + w + 28U, static_cast<crd::u16>(m.name.size()));
            iodetail::wr_u16(base + w + 30U, 0U); // extra
            iodetail::wr_u16(base + w + 32U, 0U); // comment
            iodetail::wr_u16(base + w + 34U, 0U); // disk
            iodetail::wr_u16(base + w + 36U, 0U); // internal attrs
            iodetail::wr_u32(base + w + 38U, 0U); // external attrs
            iodetail::wr_u32(base + w + 42U, m.local_offset);
            w += 46U;
            std::memcpy(base + w, m.name.data(), m.name.size());
            w += m.name.size();
        }
        // EOCD
        iodetail::wr_u32(base + w, 0x06054B50U);
        iodetail::wr_u16(base + w + 4U, 0U);
        iodetail::wr_u16(base + w + 6U, 0U);
        iodetail::wr_u16(base + w + 8U, static_cast<crd::u16>(m_members.size()));
        iodetail::wr_u16(base + w + 10U, static_cast<crd::u16>(m_members.size()));
        iodetail::wr_u32(base + w + 12U, static_cast<crd::u32>(w - cd_start));
        iodetail::wr_u32(base + w + 16U, static_cast<crd::u32>(cd_start));
        iodetail::wr_u16(base + w + 20U, 0U);
        CRD_ASSERT_MSG(w + 22U == file.size(), "NpzWriter::finish: size accounting mismatch");
        out = static_cast<crd::containers::Array<crd::u8>&&>(file);
        return TensorStatus::Ok;
    }

private:
    struct Member
    {
        explicit Member(crd::memory::IAllocator* a) : name(a), npy_bytes(a) {}
        Member(Member&&) noexcept = default;
        Member& operator=(Member&&) noexcept = default;
        crd::containers::String name;
        crd::containers::Array<crd::u8> npy_bytes;
        crd::u32 crc = 0;
        crd::u32 local_offset = 0;
    };

    crd::memory::IAllocator* m_alloc;
    crd::containers::Array<Member> m_members;
};

// Reader: STORED + DEFLATE members, CRC-verified. parse() decompresses every
// DEFLATE member once into an owned backing buffer (reserved up front so
// entry offsets stay stable — the reserve-before-spans rule); STORED members
// stay zero-copy views into the source. `bytes` must outlive reads.
class NpzReader
{
public:
    explicit NpzReader(crd::memory::IAllocator* alloc) noexcept
        : m_alloc(alloc), m_entries(alloc), m_backing(alloc)
    {
    }

    [[nodiscard]] TensorStatus parse(crd::containers::ConstSpan<crd::u8> bytes) noexcept
    {
        m_src = bytes;
        m_entries.clear();
        m_backing.clear();
        // locate EOCD: scan backward over the (comment-bearing) tail
        if (bytes.size() < 22U)
        {
            return TensorStatus::BadInput;
        }
        crd::usize eocd = bytes.size(); // sentinel: not found
        const crd::usize scan_end = bytes.size() - 22U;
        const crd::usize scan_lo = bytes.size() > 22U + 0xFFFFU ? bytes.size() - 22U - 0xFFFFU : 0U;
        for (crd::usize i = scan_end + 1U; i-- > scan_lo;)
        {
            if (iodetail::rd_u32(bytes.data() + i) == 0x06054B50U)
            {
                eocd = i;
                break;
            }
        }
        if (eocd == bytes.size())
        {
            return TensorStatus::BadInput;
        }
        const crd::u16 count = iodetail::rd_u16(bytes.data() + eocd + 10U);
        const crd::u32 cd_size = iodetail::rd_u32(bytes.data() + eocd + 12U);
        const crd::u32 cd_off = iodetail::rd_u32(bytes.data() + eocd + 16U);
        if (count == 0xFFFFU || cd_size == 0xFFFFFFFFU || cd_off == 0xFFFFFFFFU)
        {
            return TensorStatus::Unsupported; // zip64
        }
        if (static_cast<crd::u64>(cd_off) + cd_size > eocd)
        {
            return TensorStatus::BadInput;
        }
        if (!m_entries.try_reserve(count))
        {
            return TensorStatus::AllocFailed;
        }
        // pass 1: central directory -> entries (+ total inflated size)
        crd::usize p = cd_off;
        crd::u64 inflated_total = 0;
        for (crd::u32 e = 0; e < count; ++e)
        {
            if (p + 46U > static_cast<crd::usize>(cd_off) + cd_size ||
                iodetail::rd_u32(bytes.data() + p) != 0x02014B50U)
            {
                return TensorStatus::BadInput;
            }
            Entry ent(m_alloc);
            const crd::u16 method = iodetail::rd_u16(bytes.data() + p + 10U);
            ent.crc = iodetail::rd_u32(bytes.data() + p + 16U);
            ent.comp_size = iodetail::rd_u32(bytes.data() + p + 20U);
            ent.uncomp_size = iodetail::rd_u32(bytes.data() + p + 24U);
            const crd::u16 name_len = iodetail::rd_u16(bytes.data() + p + 28U);
            const crd::u16 extra_len = iodetail::rd_u16(bytes.data() + p + 30U);
            const crd::u16 comment_len = iodetail::rd_u16(bytes.data() + p + 32U);
            ent.local_offset = iodetail::rd_u32(bytes.data() + p + 42U);
            if (ent.comp_size == 0xFFFFFFFFU || ent.uncomp_size == 0xFFFFFFFFU || ent.local_offset == 0xFFFFFFFFU)
            {
                return TensorStatus::Unsupported; // zip64
            }
            if (method != 0U && method != 8U)
            {
                return TensorStatus::Unsupported;
            }
            ent.deflated = method == 8U;
            if (p + 46U + name_len > static_cast<crd::usize>(cd_off) + cd_size)
            {
                return TensorStatus::BadInput;
            }
            if (!ent.name.try_reserve(name_len))
            {
                return TensorStatus::AllocFailed;
            }
            ent.name.append(reinterpret_cast<const char*>(bytes.data() + p + 46U), name_len);
            if (ent.deflated)
            {
                inflated_total += ent.uncomp_size;
            }
            m_entries.push_back(static_cast<Entry&&>(ent));
            p += 46U + static_cast<crd::usize>(name_len) + extra_len + comment_len;
        }
        // pass 2: resolve data offsets via local headers; inflate DEFLATE
        // members into the pre-reserved backing (offsets, never raw spans)
        if (!m_backing.try_reserve(static_cast<crd::usize>(inflated_total)))
        {
            return TensorStatus::AllocFailed;
        }
        for (Entry& ent : m_entries)
        {
            const crd::usize lp = ent.local_offset;
            if (lp + 30U > bytes.size() || iodetail::rd_u32(bytes.data() + lp) != 0x04034B50U)
            {
                return TensorStatus::BadInput;
            }
            const crd::u16 lname = iodetail::rd_u16(bytes.data() + lp + 26U);
            const crd::u16 lextra = iodetail::rd_u16(bytes.data() + lp + 28U);
            const crd::u64 data_off = static_cast<crd::u64>(lp) + 30U + lname + lextra;
            if (data_off + ent.comp_size > bytes.size())
            {
                return TensorStatus::BadInput;
            }
            if (!ent.deflated)
            {
                if (ent.comp_size != ent.uncomp_size)
                {
                    return TensorStatus::BadInput;
                }
                ent.src_offset = data_off;
                ent.in_backing = false;
            }
            else
            {
                const crd::usize dst_off = m_backing.size();
                m_backing.resize_uninitialized(dst_off + ent.uncomp_size); // within the reserve
                if (!iodetail::inflate(bytes.subspan(static_cast<crd::usize>(data_off), ent.comp_size),
                                       {m_backing.data() + dst_off, ent.uncomp_size}))
                {
                    return TensorStatus::BadInput;
                }
                ent.src_offset = dst_off;
                ent.in_backing = true;
            }
            // CRC-32 gate on every member (bit-exactness doctrine)
            const crd::u8* payload = ent.in_backing ? m_backing.data() + ent.src_offset
                                                    : m_src.data() + ent.src_offset;
            if (iodetail::crc32({payload, ent.uncomp_size}) != ent.crc)
            {
                return TensorStatus::BadInput;
            }
        }
        return TensorStatus::Ok;
    }

    [[nodiscard]] crd::usize count() const noexcept { return m_entries.size(); }

    // Full zip member name ("a.npy").
    [[nodiscard]] crd::containers::StringView name(crd::usize i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_entries.size(), "NpzReader::name: index out of range");
        return crd::containers::StringView{m_entries[i].name.data(), m_entries[i].name.size()};
    }

    // Find by numpy key; matches with or without the ".npy" suffix. -1 = absent.
    [[nodiscard]] crd::i64 find(crd::containers::StringView key) const noexcept
    {
        for (crd::usize i = 0; i < m_entries.size(); ++i)
        {
            const crd::containers::StringView n = name(i);
            if (n == key)
            {
                return static_cast<crd::i64>(i);
            }
            if (n.size() == key.size() + 4U && n.substr(0, key.size()) == key && n.substr(key.size()) == ".npy")
            {
                return static_cast<crd::i64>(i);
            }
        }
        return -1;
    }

    // Parse member i as .npy. The view's payload aliases either the source
    // bytes or this reader's backing — both must stay alive while used.
    [[nodiscard]] TensorStatus npy(crd::usize i, NpyView& out) const noexcept
    {
        if (i >= m_entries.size())
        {
            return TensorStatus::BadInput;
        }
        const Entry& ent = m_entries[i];
        const crd::u8* payload = ent.in_backing ? m_backing.data() + ent.src_offset : m_src.data() + ent.src_offset;
        return npy_parse({payload, ent.uncomp_size}, out);
    }

    template <typename T>
    [[nodiscard]] TensorStatus read(crd::usize i, crd::memory::IAllocator* alloc, Tensor<T>& out) const noexcept
    {
        NpyView v;
        const TensorStatus st = npy(i, v);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        return iodetail::materialize(alloc, v.dtype, io_dtype_of<T>(), v.shape_span(), v.payload, out);
    }

private:
    struct Entry
    {
        explicit Entry(crd::memory::IAllocator* a) : name(a) {}
        Entry(Entry&&) noexcept = default;
        Entry& operator=(Entry&&) noexcept = default;
        crd::containers::String name;
        crd::u64 src_offset = 0; // into m_src (stored) or m_backing (deflated)
        crd::u32 comp_size = 0;
        crd::u32 uncomp_size = 0;
        crd::u32 crc = 0;
        crd::u32 local_offset = 0;
        bool deflated = false;
        bool in_backing = false;
    };

    crd::memory::IAllocator* m_alloc;
    crd::containers::ConstSpan<crd::u8> m_src{};
    crd::containers::Array<Entry> m_entries;
    crd::containers::Array<crd::u8> m_backing; // inflated DEFLATE members
};

// =======================================================================
// safetensors — the huggingface tensor container
// =======================================================================

namespace iodetail
{

struct StDtypeRow
{
    const char* name;
    IoDtype dtype;
};

// the reference dtype strings (safetensors Dtype enum); complex types are
// not part of the format
inline constexpr StDtypeRow kStDtypes[] = {
    {"F64", IoDtype::F64},   {"F32", IoDtype::F32},         {"F16", IoDtype::F16},
    {"BF16", IoDtype::Bf16}, {"I64", IoDtype::I64},         {"U64", IoDtype::U64},
    {"I32", IoDtype::I32},   {"U32", IoDtype::U32},         {"I16", IoDtype::I16},
    {"U16", IoDtype::U16},   {"I8", IoDtype::I8},           {"U8", IoDtype::U8},
    {"BOOL", IoDtype::Bool}, {"F8_E4M3", IoDtype::Fp8E4m3}, {"F8_E5M2", IoDtype::Fp8E5m2},
};

[[nodiscard]] inline bool st_dtype_from_name(crd::containers::StringView s, IoDtype& out) noexcept
{
    for (const StDtypeRow& r : kStDtypes)
    {
        if (s == r.name)
        {
            out = r.dtype;
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr const char* st_dtype_name(IoDtype d) noexcept
{
    for (const StDtypeRow& r : kStDtypes)
    {
        if (r.dtype == d)
        {
            return r.name;
        }
    }
    return nullptr; // C32/C64 — not expressible in safetensors
}

// JSON escape into a byte array (RFC 8259 minimal escaping, \u00XX for
// other control bytes) — the writer side.
[[nodiscard]] inline bool json_escape_append(crd::containers::Array<crd::u8>& dst,
                                             crd::containers::StringView s) noexcept
{
    for (const char c : s)
    {
        const crd::u8 b = static_cast<crd::u8>(c);
        bool ok = true;
        if (c == '"' || c == '\\')
        {
            const char esc[2] = {'\\', c};
            ok = append_bytes(dst, esc, 2U);
        }
        else if (b >= 0x20U)
        {
            ok = append_bytes(dst, &c, 1U);
        }
        else
        {
            switch (c)
            {
            case '\n':
                ok = append_cstr(dst, "\\n");
                break;
            case '\r':
                ok = append_cstr(dst, "\\r");
                break;
            case '\t':
                ok = append_cstr(dst, "\\t");
                break;
            case '\b':
                ok = append_cstr(dst, "\\b");
                break;
            case '\f':
                ok = append_cstr(dst, "\\f");
                break;
            default:
            {
                static constexpr char hex[] = "0123456789abcdef";
                const char u[6] = {'\\', 'u', '0', '0', hex[(b >> 4U) & 0xFU], hex[b & 0xFU]};
                ok = append_bytes(dst, u, 6U);
                break;
            }
            }
        }
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

// Deterministic JSON parser for the safetensors header — full string
// unescaping (incl. \uXXXX with surrogate pairs → UTF-8), unsigned integers
// only (the header's numbers are dims/offsets). No std containers.
class JsonCursor
{
public:
    JsonCursor(const char* begin, const char* end) noexcept : m_p(begin), m_end(end) {}

    void skip_ws() noexcept
    {
        while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r'))
        {
            ++m_p;
        }
    }

    [[nodiscard]] bool accept(char c) noexcept
    {
        skip_ws();
        if (m_p < m_end && *m_p == c)
        {
            ++m_p;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool peek(char c) noexcept
    {
        skip_ws();
        return m_p < m_end && *m_p == c;
    }

    [[nodiscard]] bool at_end() noexcept
    {
        skip_ws();
        return m_p >= m_end;
    }

    [[nodiscard]] bool number_u64(crd::u64& out) noexcept
    {
        skip_ws();
        if (m_p >= m_end || *m_p < '0' || *m_p > '9')
        {
            return false;
        }
        crd::u64 v = 0;
        while (m_p < m_end && *m_p >= '0' && *m_p <= '9')
        {
            const crd::u64 digit = static_cast<crd::u64>(*m_p - '0');
            if (v > ((~crd::u64{0}) - digit) / 10U)
            {
                return false;
            }
            v = v * 10U + digit;
            ++m_p;
        }
        out = v;
        return true;
    }

    // Parse a JSON string into `out` (unescaped UTF-8).
    [[nodiscard]] bool string(crd::containers::String& out) noexcept
    {
        skip_ws();
        if (m_p >= m_end || *m_p != '"')
        {
            return false;
        }
        ++m_p;
        out.clear();
        while (m_p < m_end && *m_p != '"')
        {
            const char c = *m_p;
            if (static_cast<crd::u8>(c) < 0x20U)
            {
                return false; // raw control character — invalid JSON
            }
            if (c != '\\')
            {
                out.push_back(c);
                ++m_p;
                continue;
            }
            ++m_p;
            if (m_p >= m_end)
            {
                return false;
            }
            const char e = *m_p++;
            switch (e)
            {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
            {
                crd::u32 cp = 0;
                if (!hex4(cp))
                {
                    return false;
                }
                if (cp >= 0xD800U && cp <= 0xDBFFU) // high surrogate — pair required
                {
                    crd::u32 lo = 0;
                    if (m_p + 2 > m_end || m_p[0] != '\\' || m_p[1] != 'u')
                    {
                        return false;
                    }
                    m_p += 2;
                    if (!hex4(lo) || lo < 0xDC00U || lo > 0xDFFFU)
                    {
                        return false;
                    }
                    cp = 0x10000U + (((cp - 0xD800U) << 10U) | (lo - 0xDC00U));
                }
                else if (cp >= 0xDC00U && cp <= 0xDFFFU)
                {
                    return false; // stray low surrogate
                }
                if (!utf8_append(out, cp))
                {
                    return false;
                }
                break;
            }
            default:
                return false;
            }
        }
        if (m_p >= m_end)
        {
            return false;
        }
        ++m_p; // closing quote
        return true;
    }

    // Skip a complete JSON value (used only to reject with position intact).
    [[nodiscard]] const char* pos() const noexcept { return m_p; }

private:
    [[nodiscard]] bool hex4(crd::u32& out) noexcept
    {
        if (m_p + 4 > m_end)
        {
            return false;
        }
        crd::u32 v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = *m_p++;
            crd::u32 d;
            if (c >= '0' && c <= '9')
            {
                d = static_cast<crd::u32>(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
                d = static_cast<crd::u32>(c - 'a') + 10U;
            }
            else if (c >= 'A' && c <= 'F')
            {
                d = static_cast<crd::u32>(c - 'A') + 10U;
            }
            else
            {
                return false;
            }
            v = (v << 4U) | d;
        }
        out = v;
        return true;
    }

    [[nodiscard]] static bool utf8_append(crd::containers::String& out, crd::u32 cp) noexcept
    {
        if (cp <= 0x7FU)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7FFU)
        {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else if (cp <= 0xFFFFU)
        {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else if (cp <= 0x10FFFFU)
        {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
        else
        {
            return false;
        }
        return true;
    }

    const char* m_p;
    const char* m_end;
};

} // namespace iodetail

// One parsed safetensors tensor entry. `payload` views the source bytes
// given to SafetensorsFile::parse — keep them alive while used.
struct SafetensorsEntry
{
    explicit SafetensorsEntry(crd::memory::IAllocator* a) : name(a) {}
    SafetensorsEntry(SafetensorsEntry&&) noexcept = default;
    SafetensorsEntry& operator=(SafetensorsEntry&&) noexcept = default;

    crd::containers::String name;
    IoDtype dtype = IoDtype::F32;
    crd::u32 rank = 0;
    crd::u64 shape[kMaxRank] = {};
    crd::u64 begin = 0; // data_offsets into the byte buffer
    crd::u64 end = 0;
    crd::containers::ConstSpan<crd::u8> payload{};

    [[nodiscard]] crd::containers::StringView name_view() const noexcept
    {
        return crd::containers::StringView{name.data(), name.size()};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u64> shape_span() const noexcept { return {shape, rank}; }
};

// Parsed safetensors container (v14-m consumes this surface: parse once,
// look tensors up by name, materialize typed Tensors or view raw bits).
class SafetensorsFile
{
public:
    explicit SafetensorsFile(crd::memory::IAllocator* alloc) noexcept
        : m_alloc(alloc), m_tensors(alloc), m_meta_keys(alloc), m_meta_values(alloc)
    {
    }

    // Parse an in-memory .safetensors file. Zero-copy: entry payloads view
    // `bytes` (the caller keeps `bytes` alive — Span discipline).
    [[nodiscard]] TensorStatus parse(crd::containers::ConstSpan<crd::u8> bytes) noexcept
    {
        if (bytes.size() < 8U)
        {
            return TensorStatus::BadInput;
        }
        const crd::u64 header_len = iodetail::rd_u64(bytes.data());
        if (header_len > bytes.size() - 8U)
        {
            return TensorStatus::BadInput;
        }
        return parse_json(reinterpret_cast<const char*>(bytes.data() + 8U), header_len,
                          bytes.data() + 8U + header_len, bytes.size() - 8U - header_len);
    }

    // Parse ONLY the JSON header against a known buffer size (the streaming
    // file path: entries carry dtype/shape/offsets; payload spans stay empty).
    [[nodiscard]] TensorStatus parse_header(crd::containers::ConstSpan<crd::u8> header_json,
                                            crd::u64 buffer_size) noexcept
    {
        return parse_json(reinterpret_cast<const char*>(header_json.data()), header_json.size(), nullptr,
                          buffer_size);
    }

private:
    [[nodiscard]] TensorStatus parse_json(const char* h, crd::u64 header_len, const crd::u8* buf_data,
                                          crd::u64 buf_size) noexcept
    {
        m_tensors.clear();
        m_meta_keys.clear();
        m_meta_values.clear();
        iodetail::JsonCursor j(h, h + header_len);
        if (!j.accept('{'))
        {
            return TensorStatus::BadInput;
        }
        if (!j.peek('}'))
        {
            do
            {
                crd::containers::String key(m_alloc);
                if (!j.string(key) || !j.accept(':'))
                {
                    return TensorStatus::BadInput;
                }
                if (crd::containers::StringView{key.data(), key.size()} == "__metadata__")
                {
                    const TensorStatus st = parse_metadata(j);
                    if (st != TensorStatus::Ok)
                    {
                        return st;
                    }
                }
                else
                {
                    const TensorStatus st =
                        parse_tensor(j, static_cast<crd::containers::String&&>(key), buf_data, buf_size);
                    if (st != TensorStatus::Ok)
                    {
                        return st;
                    }
                }
            } while (j.accept(','));
        }
        if (!j.accept('}') || !j.at_end()) // spec pads the header with trailing spaces only
        {
            return TensorStatus::BadInput;
        }
        // duplicate names are malformed (mirrors the reference validator)
        for (crd::usize i = 0; i < m_tensors.size(); ++i)
        {
            for (crd::usize k = i + 1U; k < m_tensors.size(); ++k)
            {
                if (m_tensors[i].name_view() == m_tensors[k].name_view())
                {
                    return TensorStatus::BadInput;
                }
            }
        }
        return TensorStatus::Ok;
    }

public:
    [[nodiscard]] crd::usize tensor_count() const noexcept { return m_tensors.size(); }

    [[nodiscard]] const SafetensorsEntry& tensor(crd::usize i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_tensors.size(), "SafetensorsFile::tensor: index out of range");
        return m_tensors[i];
    }

    [[nodiscard]] crd::i64 find(crd::containers::StringView name) const noexcept
    {
        for (crd::usize i = 0; i < m_tensors.size(); ++i)
        {
            if (m_tensors[i].name_view() == name)
            {
                return static_cast<crd::i64>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] crd::usize metadata_count() const noexcept { return m_meta_keys.size(); }
    [[nodiscard]] crd::containers::StringView metadata_key(crd::usize i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_meta_keys.size(), "SafetensorsFile::metadata_key: index out of range");
        return crd::containers::StringView{m_meta_keys[i].data(), m_meta_keys[i].size()};
    }
    [[nodiscard]] crd::containers::StringView metadata_value(crd::usize i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_meta_values.size(), "SafetensorsFile::metadata_value: index out of range");
        return crd::containers::StringView{m_meta_values[i].data(), m_meta_values[i].size()};
    }
    [[nodiscard]] crd::i64 metadata_find(crd::containers::StringView key) const noexcept
    {
        for (crd::usize i = 0; i < m_meta_keys.size(); ++i)
        {
            if (metadata_key(i) == key)
            {
                return static_cast<crd::i64>(i);
            }
        }
        return -1;
    }

    // Materialize tensor i into an owning Tensor<T> (exact dtype required).
    template <typename T>
    [[nodiscard]] TensorStatus read(crd::usize i, crd::memory::IAllocator* alloc, Tensor<T>& out) const noexcept
    {
        if (i >= m_tensors.size())
        {
            return TensorStatus::BadInput;
        }
        const SafetensorsEntry& e = m_tensors[i];
        return iodetail::materialize(alloc, e.dtype, io_dtype_of<T>(), e.shape_span(), e.payload, out);
    }

    // Bits-carrier read for storage dtypes (f16/bf16/bool/fp8): T = u16/u8.
    template <typename T>
    [[nodiscard]] TensorStatus read_as(crd::usize i, IoDtype expected, crd::memory::IAllocator* alloc,
                                       Tensor<T>& out) const noexcept
    {
        if (i >= m_tensors.size())
        {
            return TensorStatus::BadInput;
        }
        const SafetensorsEntry& e = m_tensors[i];
        return iodetail::materialize(alloc, e.dtype, expected, e.shape_span(), e.payload, out);
    }

private:
    [[nodiscard]] TensorStatus parse_metadata(iodetail::JsonCursor& j) noexcept
    {
        if (!j.accept('{'))
        {
            return TensorStatus::BadInput;
        }
        if (j.accept('}'))
        {
            return TensorStatus::Ok;
        }
        do
        {
            crd::containers::String k(m_alloc);
            crd::containers::String v(m_alloc);
            if (!j.string(k) || !j.accept(':') || !j.string(v))
            {
                return TensorStatus::BadInput; // metadata values are strings by spec
            }
            m_meta_keys.push_back(static_cast<crd::containers::String&&>(k));
            m_meta_values.push_back(static_cast<crd::containers::String&&>(v));
        } while (j.accept(','));
        return j.accept('}') ? TensorStatus::Ok : TensorStatus::BadInput;
    }

    [[nodiscard]] TensorStatus parse_tensor(iodetail::JsonCursor& j, crd::containers::String&& name,
                                            const crd::u8* buf_data, crd::u64 buf_size) noexcept
    {
        SafetensorsEntry e(m_alloc);
        e.name = static_cast<crd::containers::String&&>(name);
        bool have_dtype = false;
        bool have_shape = false;
        bool have_offsets = false;
        if (!j.accept('{'))
        {
            return TensorStatus::BadInput;
        }
        do
        {
            crd::containers::String key(m_alloc);
            if (!j.string(key) || !j.accept(':'))
            {
                return TensorStatus::BadInput;
            }
            const crd::containers::StringView kv{key.data(), key.size()};
            if (kv == "dtype")
            {
                crd::containers::String dt(m_alloc);
                if (have_dtype || !j.string(dt))
                {
                    return TensorStatus::BadInput;
                }
                if (!iodetail::st_dtype_from_name(crd::containers::StringView{dt.data(), dt.size()}, e.dtype))
                {
                    return TensorStatus::Unsupported; // dtype outside our pinned set
                }
                have_dtype = true;
            }
            else if (kv == "shape")
            {
                if (have_shape || !j.accept('['))
                {
                    return TensorStatus::BadInput;
                }
                if (!j.peek(']'))
                {
                    do
                    {
                        crd::u64 dim = 0;
                        if (!j.number_u64(dim))
                        {
                            return TensorStatus::BadInput;
                        }
                        if (e.rank >= kMaxRank)
                        {
                            return TensorStatus::RankOverflow;
                        }
                        e.shape[e.rank++] = dim;
                    } while (j.accept(','));
                }
                if (!j.accept(']'))
                {
                    return TensorStatus::BadInput;
                }
                have_shape = true;
            }
            else if (kv == "data_offsets")
            {
                if (have_offsets || !j.accept('['))
                {
                    return TensorStatus::BadInput;
                }
                if (!j.number_u64(e.begin) || !j.accept(',') || !j.number_u64(e.end) || !j.accept(']'))
                {
                    return TensorStatus::BadInput;
                }
                have_offsets = true;
            }
            else
            {
                return TensorStatus::BadInput; // fixed schema — unknown key
            }
        } while (j.accept(','));
        if (!j.accept('}') || !have_dtype || !have_shape || !have_offsets)
        {
            return TensorStatus::BadInput;
        }
        crd::u64 elems = 0;
        crd::u64 payload_bytes = 0;
        if (!iodetail::shape_elems(e.shape_span(), io_dtype_size(e.dtype), elems, payload_bytes))
        {
            return TensorStatus::BadInput;
        }
        if (e.begin > e.end || e.end > buf_size || e.end - e.begin != payload_bytes)
        {
            return TensorStatus::BadInput;
        }
        if (buf_data != nullptr) // header-only mode leaves payload views empty
        {
            e.payload = crd::containers::ConstSpan<crd::u8>{buf_data + e.begin,
                                                            static_cast<crd::usize>(payload_bytes)};
        }
        if (!m_tensors.try_reserve(m_tensors.size() + 1U))
        {
            return TensorStatus::AllocFailed;
        }
        m_tensors.push_back(static_cast<SafetensorsEntry&&>(e));
        return TensorStatus::Ok;
    }

    crd::memory::IAllocator* m_alloc;
    crd::containers::Array<SafetensorsEntry> m_tensors;
    crd::containers::Array<crd::containers::String> m_meta_keys;
    crd::containers::Array<crd::containers::String> m_meta_values;
};

// Writer. Payload spans are BORROWED — the caller keeps every added tensor's
// bytes alive until finish() returns (Span discipline; no hidden copies of
// multi-GB weights). Deterministic output: metadata keys and tensors are
// emitted name-sorted with contiguous offsets from 0 (what the reference
// library produces), header space-padded to an 8-byte multiple.
class SafetensorsWriter
{
public:
    explicit SafetensorsWriter(crd::memory::IAllocator* alloc) noexcept
        : m_alloc(alloc), m_entries(alloc), m_meta_keys(alloc), m_meta_values(alloc)
    {
    }

    [[nodiscard]] TensorStatus add(crd::containers::StringView name, IoDtype dtype,
                                   crd::containers::ConstSpan<crd::u64> shape,
                                   crd::containers::ConstSpan<crd::u8> payload) noexcept
    {
        if (iodetail::st_dtype_name(dtype) == nullptr)
        {
            return TensorStatus::Unsupported; // C32/C64 are not in the format
        }
        if (shape.size() > kMaxRank)
        {
            return TensorStatus::RankOverflow;
        }
        crd::u64 elems = 0;
        crd::u64 payload_bytes = 0;
        if (!iodetail::shape_elems(shape, io_dtype_size(dtype), elems, payload_bytes))
        {
            return TensorStatus::BadInput;
        }
        if (payload.size() != payload_bytes)
        {
            return TensorStatus::BadInput;
        }
        Pending p(m_alloc);
        if (!p.name.try_reserve(name.size()))
        {
            return TensorStatus::AllocFailed;
        }
        p.name.append(name.data(), name.size());
        p.dtype = dtype;
        p.rank = static_cast<crd::u32>(shape.size());
        for (crd::u32 d = 0; d < p.rank; ++d)
        {
            p.shape[d] = shape[d];
        }
        p.payload = payload;
        if (!m_entries.try_reserve(m_entries.size() + 1U))
        {
            return TensorStatus::AllocFailed;
        }
        m_entries.push_back(static_cast<Pending&&>(p));
        return TensorStatus::Ok;
    }

    template <typename T>
    [[nodiscard]] TensorStatus add(crd::containers::StringView name, const TensorView<const T>& view) noexcept
    {
        if (!view.is_contiguous())
        {
            return TensorStatus::NotContiguous;
        }
        return add(name, io_dtype_of<T>(), view.shape(),
                   {reinterpret_cast<const crd::u8*>(view.data()), static_cast<crd::usize>(view.size()) * sizeof(T)});
    }

    [[nodiscard]] TensorStatus add_metadata(crd::containers::StringView key,
                                            crd::containers::StringView value) noexcept
    {
        crd::containers::String k(m_alloc);
        crd::containers::String v(m_alloc);
        if (!k.try_reserve(key.size()) || !v.try_reserve(value.size()))
        {
            return TensorStatus::AllocFailed;
        }
        k.append(key.data(), key.size());
        v.append(value.data(), value.size());
        m_meta_keys.push_back(static_cast<crd::containers::String&&>(k));
        m_meta_values.push_back(static_cast<crd::containers::String&&>(v));
        return TensorStatus::Ok;
    }

    [[nodiscard]] TensorStatus finish(crd::containers::Array<crd::u8>& out) noexcept
    {
        crd::containers::Array<crd::u8> hdr(m_alloc);
        crd::containers::Array<crd::u32> idx(m_alloc);
        crd::u64 payload_total = 0;
        const TensorStatus st = build_header(hdr, idx, payload_total);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        const crd::u64 total = 8U + hdr.size() + payload_total;
        crd::containers::Array<crd::u8> file(m_alloc);
        if (!file.try_reserve(static_cast<crd::usize>(total)))
        {
            return TensorStatus::AllocFailed;
        }
        file.resize_uninitialized(static_cast<crd::usize>(total));
        crd::u8* w = file.data();
        iodetail::wr_u64(w, hdr.size());
        std::memcpy(w + 8U, hdr.data(), hdr.size());
        crd::usize pos = 8U + hdr.size();
        for (crd::usize i = 0; i < idx.size(); ++i)
        {
            const Pending& e = m_entries[idx[i]];
            if (!e.payload.empty())
            {
                std::memcpy(w + pos, e.payload.data(), e.payload.size());
            }
            pos += e.payload.size();
        }
        out = static_cast<crd::containers::Array<crd::u8>&&>(file);
        return TensorStatus::Ok;
    }

    // Streaming variant: header + payload spans fwrite in emission order —
    // single pass over the payloads, no whole-file staging buffer.
    // Byte-identical output to finish() + io_write_file (gated).
    [[nodiscard]] TensorStatus finish_file(crd::containers::StringView path) noexcept
    {
        crd::containers::Array<crd::u8> hdr(m_alloc);
        crd::containers::Array<crd::u32> idx(m_alloc);
        crd::u64 payload_total = 0;
        const TensorStatus st = build_header(hdr, idx, payload_total);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        const iodetail::CFile f(path, iodetail::CFile::Overwrite{}, m_alloc);
        if (!f.ok())
        {
            return TensorStatus::BadInput;
        }
        crd::u8 len8[8];
        iodetail::wr_u64(len8, hdr.size());
        if (!f.write_exact(len8, 8U) || !f.write_exact(hdr.data(), hdr.size()))
        {
            return TensorStatus::BadInput;
        }
        for (crd::usize i = 0; i < idx.size(); ++i)
        {
            const Pending& e = m_entries[idx[i]];
            if (!f.write_exact(e.payload.data(), e.payload.size()))
            {
                return TensorStatus::BadInput;
            }
        }
        return f.truncate_here() ? TensorStatus::Ok : TensorStatus::BadInput;
    }

private:
    // The space-padded header JSON + the name-sorted emission order (stable
    // insertion sort — deterministic, matches the reference library's
    // serialized layout).
    [[nodiscard]] TensorStatus build_header(crd::containers::Array<crd::u8>& out_hdr,
                                            crd::containers::Array<crd::u32>& out_idx,
                                            crd::u64& out_payload_total) noexcept
    {
        crd::containers::Array<crd::u32>& idx = out_idx;
        if (!idx.try_reserve(m_entries.size()))
        {
            return TensorStatus::AllocFailed;
        }
        for (crd::usize i = 0; i < m_entries.size(); ++i)
        {
            idx.push_back(static_cast<crd::u32>(i));
        }
        for (crd::usize i = 1; i < idx.size(); ++i)
        {
            const crd::u32 v = idx[i];
            crd::usize p = i;
            while (p > 0U && m_entries[v].name_view() < m_entries[idx[p - 1U]].name_view())
            {
                idx[p] = idx[p - 1U];
                --p;
            }
            idx[p] = v;
        }
        crd::containers::Array<crd::u32> midx(m_alloc);
        if (!midx.try_reserve(m_meta_keys.size()))
        {
            return TensorStatus::AllocFailed;
        }
        for (crd::usize i = 0; i < m_meta_keys.size(); ++i)
        {
            midx.push_back(static_cast<crd::u32>(i));
        }
        for (crd::usize i = 1; i < midx.size(); ++i)
        {
            const crd::u32 v = midx[i];
            crd::usize p = i;
            while (p > 0U && key_view(midx[p - 1U]) > key_view(v))
            {
                midx[p] = midx[p - 1U];
                --p;
            }
            midx[p] = v;
        }

        // header JSON (compact — the reference serializer's shape)
        crd::containers::Array<crd::u8> hdr(m_alloc);
        bool ok = iodetail::append_cstr(hdr, "{");
        bool first = true;
        if (!m_meta_keys.empty())
        {
            ok = ok && iodetail::append_cstr(hdr, "\"__metadata__\":{");
            for (crd::usize i = 0; i < midx.size(); ++i)
            {
                if (i > 0U)
                {
                    ok = ok && iodetail::append_cstr(hdr, ",");
                }
                ok = ok && iodetail::append_cstr(hdr, "\"");
                ok = ok && iodetail::json_escape_append(hdr, key_view(midx[i]));
                ok = ok && iodetail::append_cstr(hdr, "\":\"");
                ok = ok && iodetail::json_escape_append(
                               hdr, crd::containers::StringView{m_meta_values[midx[i]].data(),
                                                                m_meta_values[midx[i]].size()});
                ok = ok && iodetail::append_cstr(hdr, "\"");
            }
            ok = ok && iodetail::append_cstr(hdr, "}");
            first = false;
        }
        crd::u64 offset = 0;
        for (crd::usize i = 0; i < idx.size(); ++i)
        {
            const Pending& e = m_entries[idx[i]];
            if (!first)
            {
                ok = ok && iodetail::append_cstr(hdr, ",");
            }
            first = false;
            ok = ok && iodetail::append_cstr(hdr, "\"");
            ok = ok && iodetail::json_escape_append(hdr, e.name_view());
            ok = ok && iodetail::append_cstr(hdr, "\":{\"dtype\":\"");
            ok = ok && iodetail::append_cstr(hdr, iodetail::st_dtype_name(e.dtype));
            ok = ok && iodetail::append_cstr(hdr, "\",\"shape\":[");
            for (crd::u32 d = 0; d < e.rank; ++d)
            {
                if (d > 0U)
                {
                    ok = ok && iodetail::append_cstr(hdr, ",");
                }
                ok = ok && iodetail::append_u64_dec(hdr, e.shape[d]);
            }
            ok = ok && iodetail::append_cstr(hdr, "],\"data_offsets\":[");
            ok = ok && iodetail::append_u64_dec(hdr, offset);
            ok = ok && iodetail::append_cstr(hdr, ",");
            offset += e.payload.size();
            ok = ok && iodetail::append_u64_dec(hdr, offset);
            ok = ok && iodetail::append_cstr(hdr, "]}");
        }
        ok = ok && iodetail::append_cstr(hdr, "}");
        if (!ok)
        {
            return TensorStatus::AllocFailed;
        }
        const crd::u64 padded = ((hdr.size() + 7U) / 8U) * 8U; // 8-byte header alignment (space padding)
        while (hdr.size() < padded)
        {
            if (!iodetail::append_cstr(hdr, " "))
            {
                return TensorStatus::AllocFailed;
            }
        }
        out_hdr = static_cast<crd::containers::Array<crd::u8>&&>(hdr);
        out_payload_total = offset;
        return TensorStatus::Ok;
    }

    struct Pending
    {
        explicit Pending(crd::memory::IAllocator* a) : name(a) {}
        Pending(Pending&&) noexcept = default;
        Pending& operator=(Pending&&) noexcept = default;
        crd::containers::String name;
        IoDtype dtype = IoDtype::F32;
        crd::u32 rank = 0;
        crd::u64 shape[kMaxRank] = {};
        crd::containers::ConstSpan<crd::u8> payload{}; // BORROWED until finish()
        [[nodiscard]] crd::containers::StringView name_view() const noexcept
        {
            return crd::containers::StringView{name.data(), name.size()};
        }
    };

    [[nodiscard]] crd::containers::StringView key_view(crd::usize i) const noexcept
    {
        return crd::containers::StringView{m_meta_keys[i].data(), m_meta_keys[i].size()};
    }

    crd::memory::IAllocator* m_alloc;
    crd::containers::Array<Pending> m_entries;
    crd::containers::Array<crd::containers::String> m_meta_keys;
    crd::containers::Array<crd::containers::String> m_meta_values;
};

// The reference implementation's header-size cap (100 MB) — a defensive
// bound for the streaming reader.
inline constexpr crd::u64 kSafetensorsMaxHeader = 100ULL * 1000ULL * 1000ULL;

namespace iodetail
{

// Streaming single-tensor read shared tail: parse the header from the file,
// locate `name`, fread its payload directly into an owned Tensor<T> —
// single pass, no whole-file staging (the ML-weights access pattern).
template <typename T>
[[nodiscard]] TensorStatus st_read_tensor_file(crd::memory::IAllocator* alloc, crd::containers::StringView path,
                                               crd::containers::StringView name, IoDtype expected,
                                               Tensor<T>& out) noexcept
{
    const CFile f(path, "rb", alloc);
    if (!f.ok())
    {
        return TensorStatus::BadInput;
    }
    crd::u8 len8[8];
    if (!f.read_exact(len8, 8U))
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 header_len = rd_u64(len8);
    if (header_len > kSafetensorsMaxHeader)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 fsize = crd::platform::fs::file_size(crd::platform::fs::Path{path});
    if (fsize < 8U + header_len)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 buffer_size = fsize - 8U - header_len;
    crd::containers::Array<crd::u8> hdr(alloc);
    if (!hdr.try_reserve(static_cast<crd::usize>(header_len)))
    {
        return TensorStatus::AllocFailed;
    }
    hdr.resize_uninitialized(static_cast<crd::usize>(header_len));
    if (!f.read_exact(hdr.data(), header_len))
    {
        return TensorStatus::BadInput;
    }
    SafetensorsFile sf(alloc);
    const TensorStatus pst = sf.parse_header(crd::containers::as_const_span(hdr), buffer_size);
    if (pst != TensorStatus::Ok)
    {
        return pst;
    }
    const crd::i64 i = sf.find(name);
    if (i < 0)
    {
        return TensorStatus::BadInput;
    }
    const SafetensorsEntry& e = sf.tensor(static_cast<crd::usize>(i));
    if (e.dtype != expected || io_dtype_size(e.dtype) != sizeof(T))
    {
        return TensorStatus::ShapeMismatch;
    }
    Tensor<T> t(alloc);
    const TensorStatus rst = t.resize(e.shape_span());
    if (rst != TensorStatus::Ok)
    {
        return rst;
    }
    if (!f.seek(8U + header_len + e.begin) || !f.read_exact(t.data(), t.size() * sizeof(T)))
    {
        return TensorStatus::BadInput;
    }
    out = static_cast<Tensor<T>&&>(t);
    return TensorStatus::Ok;
}

} // namespace iodetail

// Streaming single-tensor safetensors read (file -> owned Tensor<T>, one
// payload pass). Exact-dtype contract, like SafetensorsFile::read.
template <typename T>
[[nodiscard]] TensorStatus safetensors_read_tensor_file(crd::memory::IAllocator* alloc,
                                                        crd::containers::StringView path,
                                                        crd::containers::StringView name, Tensor<T>& out) noexcept
{
    return iodetail::st_read_tensor_file(alloc, path, name, io_dtype_of<T>(), out);
}

// Bits-carrier variant for the storage dtypes (f16/bf16/bool/fp8).
template <typename T>
[[nodiscard]] TensorStatus safetensors_read_tensor_file_as(crd::memory::IAllocator* alloc,
                                                           crd::containers::StringView path,
                                                           crd::containers::StringView name, IoDtype expected,
                                                           Tensor<T>& out) noexcept
{
    return iodetail::st_read_tensor_file(alloc, path, name, expected, out);
}

// =======================================================================
// philox_fill_uniform — counter-RNG tensor fills (reproducible by
// construction). Element at canonical logical index k gets the k-th draw of
// PhiloxRng(seed, stream) — f32: lane (k & 3) of block (k >> 2) mapped by
// (v >> 8) * 2^-24; f64: the (k & 1)-th u64 of block (k >> 1) mapped by
// (v >> 11) * 2^-53. A pure function of (seed, stream, k): worker count,
// chunking, and striding CANNOT change the bits ({1..16} gated), and the
// result is bit-identical to sequential PhiloxRng draws (gated).
// =======================================================================

namespace iodetail
{

template <typename T>
inline void philox_block_values(crd::u64 seed, crd::u64 stream, crd::u64 block, T* vals) noexcept
{
    const crd::u32 counter[4] = {static_cast<crd::u32>(block), static_cast<crd::u32>(block >> 32U),
                                 static_cast<crd::u32>(stream), static_cast<crd::u32>(stream >> 32U)};
    const crd::u32 key[2] = {static_cast<crd::u32>(seed), static_cast<crd::u32>(seed >> 32U)};
    const crd::hesap::stats::PhiloxBlock b = crd::hesap::stats::philox4x32(counter, key);
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        for (crd::u32 l = 0; l < 4U; ++l)
        {
            vals[l] = static_cast<crd::f32>(b.v[l] >> 8U) * (1.0F / 16777216.0F); // / 2^24
        }
    }
    else
    {
        for (crd::u32 l = 0; l < 2U; ++l)
        {
            const crd::u64 lo = b.v[2U * l];
            const crd::u64 hi = b.v[2U * l + 1U];
            const crd::u64 u = (hi << 32U) | lo;
            vals[l] = static_cast<crd::f64>(u >> 11U) * (1.0 / 9007199254740992.0); // / 2^53
        }
    }
}

template <typename T> [[nodiscard]] inline T philox_value_at(crd::u64 seed, crd::u64 stream, crd::u64 k) noexcept
{
    constexpr crd::u64 per_block = std::is_same_v<T, crd::f32> ? 4U : 2U;
    T vals[per_block];
    philox_block_values<T>(seed, stream, k / per_block, vals);
    return vals[k % per_block];
}

} // namespace iodetail

template <typename T>
[[nodiscard]] TensorStatus philox_fill_uniform(TensorView<T> dst, crd::u64 seed, crd::u64 stream = 0,
                                               crd::u32 num_workers = 0) noexcept
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>,
                  "philox_fill_uniform: T must be f32 or f64");
    const crd::u64 n = dst.size();
    if (n == 0U)
    {
        return TensorStatus::Ok;
    }
    if (!dst.is_contiguous())
    {
        // strided path: same key domain (canonical logical index), scalar walk
        crd::u64 k = 0;
        dst.for_each(
            [&](const crd::u64*, T& v)
            {
                v = iodetail::philox_value_at<T>(seed, stream, k);
                ++k;
            });
        return TensorStatus::Ok;
    }
    constexpr crd::u64 per_block = std::is_same_v<T, crd::f32> ? 4U : 2U;
    T* p = dst.data();
    const crd::u64 blocks64 = (n + per_block - 1U) / per_block;
    const auto run_blocks = [=](crd::u32 b0, crd::u32 b1) noexcept
    {
        for (crd::u32 b = b0; b < b1; ++b)
        {
            T vals[per_block];
            iodetail::philox_block_values<T>(seed, stream, b, vals);
            const crd::u64 base = static_cast<crd::u64>(b) * per_block;
            const crd::u64 cnt = n - base < per_block ? n - base : per_block;
            for (crd::u64 l = 0; l < cnt; ++l)
            {
                p[base + l] = vals[l];
            }
        }
    };
    if (blocks64 > 0xFFFFFFFFULL)
    {
        return TensorStatus::BadInput; // > 16G elements per call — split at the caller
    }
    const crd::u32 blocks = static_cast<crd::u32>(blocks64);
    crd::u32 nw = num_workers;
    if (nw == 0U)
    {
        nw = crd::jobs::num_workers();
    }
    if (nw <= 1U || blocks < 2U)
    {
        run_blocks(0U, blocks);
        return TensorStatus::Ok;
    }
    struct Ctx
    {
        const decltype(run_blocks)* run;
    };
    Ctx ctx{&run_blocks};
    Ctx* const cp = &ctx;
    auto* const counter =
        crd::jobs::parallel_for(blocks, nw, [cp](crd::u32 b0, crd::u32 b1) { (*cp->run)(b0, b1); });
    crd::jobs::wait(counter);
    return TensorStatus::Ok;
}

} // namespace crd::hesap::tensor
