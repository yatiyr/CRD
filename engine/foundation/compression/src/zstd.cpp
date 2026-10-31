#include <crd/compression/zstd.hpp>

#include <cstddef>
#include <zstd.h>

namespace crd::compression
{

crd::usize zstd_compress_bound(crd::usize size) noexcept
{
    const std::size_t bound = ZSTD_compressBound(size);
    return ZSTD_isError(bound) != 0U ? 0U : static_cast<crd::usize>(bound);
}

bool zstd_compress(crd::containers::ConstSpan<crd::u8> in, int level, crd::containers::Array<crd::u8>& out)
{
    out.clear();
    const crd::usize bound = zstd_compress_bound(in.size());
    if (bound == 0U)
    {
        return false;
    }
    out.resize(bound);
    const std::size_t written = ZSTD_compress(out.data(), bound, in.data(), in.size(), level);
    if (ZSTD_isError(written) != 0U)
    {
        out.clear();
        return false;
    }
    out.resize(static_cast<crd::usize>(written));
    return true;
}

bool zstd_decompress_exact(crd::containers::ConstSpan<crd::u8> in, crd::containers::Span<crd::u8> out) noexcept
{
    const std::size_t written = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    return ZSTD_isError(written) == 0U && written == out.size();
}

} // namespace crd::compression
