#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-l io detail: the little-endian scalar reads and
// writes of the .npy and safetensors headers. The .npz zip records are
// crd-compression's ZIP container (OWN.1, capability compression.zip-archives).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>

#include <cstring> // memcpy (LE-host scalar reads, no unaligned UB)

namespace crd::hesap::tensor::iodetail
{

// ---- little-endian scalar peeks/pokes (LE-host posture, memcpy for alignment) ----

[[nodiscard]] inline crd::u16 rd_u16(const crd::u8* p) noexcept
{
    crd::u16 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
[[nodiscard]] inline crd::u32 rd_u32(const crd::u8* p) noexcept
{
    crd::u32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
[[nodiscard]] inline crd::u64 rd_u64(const crd::u8* p) noexcept
{
    crd::u64 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline void wr_u16(crd::u8* p, crd::u16 v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
}
inline void wr_u32(crd::u8* p, crd::u32 v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
}
inline void wr_u64(crd::u8* p, crd::u64 v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
}

} // namespace crd::hesap::tensor::iodetail
