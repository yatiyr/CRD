#include <crd/compression/zstd.hpp>
#include <crd/memory/allocator.hpp>

#include <cstddef>
#define ZSTD_STATIC_LINKING_ONLY // the static workspaces and magicless frames of the pinned version (cmake/pins.json)
#include <zdict.h>
#include <zstd.h>

namespace crd::compression
{
namespace
{
// zstd's workspaces hold 64-bit tables.
constexpr std::size_t kWorkspaceAlignment = 16U;

// One set of parameters per message size and level: a dictionary's tables and a context's are the same shape.
[[nodiscard]] ZSTD_compressionParameters message_parameters(crd::u32 max_message_bytes, int level) noexcept
{
    return ZSTD_getCParams(level, max_message_bytes, 0U);
}
} // namespace

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

ZstdDictionary::ZstdDictionary(crd::memory::IAllocator& allocator, crd::containers::ConstSpan<crd::u8> bytes,
                               crd::u32 max_message_bytes, int level) noexcept
    : m_allocator(allocator)
{
    const ZSTD_compressionParameters parameters = message_parameters(max_message_bytes, level);
    const std::size_t compress_bytes = ZSTD_estimateCDictSize_advanced(bytes.size(), parameters, ZSTD_dlm_byRef);
    const std::size_t decompress_bytes = ZSTD_estimateDDictSize(bytes.size(), ZSTD_dlm_byRef);
    m_memory = allocator.try_allocate(compress_bytes + decompress_bytes, kWorkspaceAlignment);
    if (m_memory == nullptr)
    {
        return;
    }
    auto* const base = static_cast<crd::u8*>(m_memory);
    m_compress = ZSTD_initStaticCDict(base, compress_bytes, bytes.data(), bytes.size(), ZSTD_dlm_byRef, ZSTD_dct_auto,
                                      parameters);
    m_decompress = ZSTD_initStaticDDict(base + compress_bytes, decompress_bytes, bytes.data(), bytes.size(),
                                        ZSTD_dlm_byRef, ZSTD_dct_auto);
}

ZstdDictionary::~ZstdDictionary()
{
    m_allocator.deallocate(m_memory);
}

ZstdMessageCoder::ZstdMessageCoder(crd::memory::IAllocator& allocator, crd::u32 max_message_bytes, int level) noexcept
    : m_allocator(allocator)
{
    const ZSTD_compressionParameters parameters = message_parameters(max_message_bytes, level);
    const std::size_t compress_bytes = ZSTD_estimateCCtxSize_usingCParams(parameters);
    const std::size_t decompress_bytes = ZSTD_estimateDCtxSize();
    m_memory = allocator.try_allocate(compress_bytes + decompress_bytes, kWorkspaceAlignment);
    if (m_memory == nullptr)
    {
        return;
    }
    auto* const base = static_cast<crd::u8*>(m_memory);
    m_compress = ZSTD_initStaticCCtx(base, compress_bytes);
    m_decompress = ZSTD_initStaticDCtx(base + compress_bytes, decompress_bytes);
    if (m_compress == nullptr || m_decompress == nullptr)
    {
        m_compress = nullptr;
        m_decompress = nullptr;
        return;
    }
    // Fixed, valid parameters on fresh contexts: nothing here can fail.
    static_cast<void>(ZSTD_CCtx_setCParams(m_compress, parameters));
    static_cast<void>(ZSTD_CCtx_setParameter(m_compress, ZSTD_c_format, ZSTD_f_zstd1_magicless));
    static_cast<void>(ZSTD_CCtx_setParameter(m_compress, ZSTD_c_contentSizeFlag, 0));
    static_cast<void>(ZSTD_CCtx_setParameter(m_compress, ZSTD_c_checksumFlag, 0));
    static_cast<void>(ZSTD_CCtx_setParameter(m_compress, ZSTD_c_dictIDFlag, 0));
    // Attaching reads the dictionary's tables where they are; copying them would cost more than a small message does.
    static_cast<void>(ZSTD_CCtx_setParameter(m_compress, ZSTD_c_forceAttachDict, ZSTD_dictForceAttach));
    static_cast<void>(ZSTD_DCtx_setParameter(m_decompress, ZSTD_d_format, ZSTD_f_zstd1_magicless));
}

ZstdMessageCoder::~ZstdMessageCoder()
{
    m_allocator.deallocate(m_memory);
}

crd::usize ZstdMessageCoder::compress(const ZstdDictionary& dictionary, crd::containers::ConstSpan<crd::u8> in,
                                      crd::containers::Span<crd::u8> out) noexcept
{
    // A compress that ran out of room leaves its session open, and a context in a session refuses a dictionary.
    static_cast<void>(ZSTD_CCtx_reset(m_compress, ZSTD_reset_session_only));
    if (ZSTD_isError(ZSTD_CCtx_refCDict(m_compress, dictionary.m_compress)) != 0U)
    {
        return 0U;
    }
    const std::size_t written = ZSTD_compress2(m_compress, out.data(), out.size(), in.data(), in.size());
    return ZSTD_isError(written) != 0U ? 0U : static_cast<crd::usize>(written);
}

bool ZstdMessageCoder::decompress_exact(const ZstdDictionary& dictionary, crd::containers::ConstSpan<crd::u8> in,
                                        crd::containers::Span<crd::u8> out) noexcept
{
    const std::size_t written =
        ZSTD_decompress_usingDDict(m_decompress, out.data(), out.size(), in.data(), in.size(), dictionary.m_decompress);
    return ZSTD_isError(written) == 0U && written == out.size();
}

crd::usize zstd_train_dictionary(crd::containers::ConstSpan<crd::u8> samples,
                                 crd::containers::ConstSpan<crd::usize> sizes,
                                 crd::containers::Span<crd::u8> out) noexcept
{
    static_assert(sizeof(crd::usize) == sizeof(std::size_t));
    const std::size_t written =
        ZDICT_trainFromBuffer(out.data(), out.size(), samples.data(),
                              reinterpret_cast<const std::size_t*>(sizes.data()), static_cast<unsigned>(sizes.size()));
    return ZDICT_isError(written) != 0U ? 0U : static_cast<crd::usize>(written);
}

} // namespace crd::compression
