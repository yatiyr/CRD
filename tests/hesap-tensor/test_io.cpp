// v14-l tensor I/O + interop gates.
// The BIT gate: round-trips of the python-oracle corpus (scripts/
// v14l_io_oracle.py gen) are byte-exact in both directions; the moat gate:
// philox_fill at {1,2,4,8,16} workers bit-identical (and == sequential
// PhiloxRng draws). DLPack: export->import identity is pointer-equal
// zero-copy with the spec's deleter contract. Corpus path defaults to
// tests/hesap-tensor/io_corpus (override: CRD_IO_CORPUS), out/ files are
// re-verified by the oracle's check mode.
#include <crd/hesap/resources/tensor_artifact.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/hesap/tensor/dlpack.hpp>
#include <crd/hesap/tensor/io.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdlib>
#include <cstring>

using crd::hesap::tensor::IoDtype;
using crd::hesap::tensor::NpyView;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;

namespace
{

constexpr crd::usize kPool = 1U << 26;

crd::containers::String corpus_path(crd::memory::IAllocator* alloc, const char* sub, const char* file)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // getenv — corpus-path dev knob only (the dense_lu_kernels precedent)
#endif
    const char* root = std::getenv("CRD_IO_CORPUS");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (root == nullptr)
    {
        root = "tests/hesap-tensor/io_corpus";
    }
    crd::containers::String s(alloc);
    s.append(root);
    s.append("/");
    s.append(sub);
    s.append("/");
    s.append(file);
    return s;
}

bool bytes_equal(crd::containers::ConstSpan<crd::u8> a, crd::containers::ConstSpan<crd::u8> b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    return a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// The pinned oracle corpus table (mirror of make_npy_entries + the forced-v2
// file — both sides change together or not at all).
struct NpyCase
{
    const char* name;
    IoDtype dtype;
    crd::u32 rank;
    crd::u64 shape[3];
    bool v1_byte_identical; // our writer must reproduce numpy's bytes exactly
};

constexpr NpyCase kNpyCases[] = {
    {"f32_2x3x4", IoDtype::F32, 3, {2, 3, 4}, true},
    {"f64_5x7", IoDtype::F64, 2, {5, 7, 0}, true},
    {"f16_3x5", IoDtype::F16, 2, {3, 5, 0}, true},
    {"i8_16", IoDtype::I8, 1, {16, 0, 0}, true},
    {"i16_9", IoDtype::I16, 1, {9, 0, 0}, true},
    {"i32_4x4", IoDtype::I32, 2, {4, 4, 0}, true},
    {"i64_2x2", IoDtype::I64, 2, {2, 2, 0}, true},
    {"u8_8", IoDtype::U8, 1, {8, 0, 0}, true},
    {"u16_6", IoDtype::U16, 1, {6, 0, 0}, true},
    {"u32_3", IoDtype::U32, 1, {3, 0, 0}, true},
    {"u64_2x5", IoDtype::U64, 2, {2, 5, 0}, true},
    {"bool_7", IoDtype::Bool, 1, {7, 0, 0}, true},
    {"c32_2x3", IoDtype::C32, 2, {2, 3, 0}, true},
    {"c64_3", IoDtype::C64, 1, {3, 0, 0}, true},
    {"f32_0d", IoDtype::F32, 0, {0, 0, 0}, true},
    {"f32_empty_0x5", IoDtype::F32, 2, {0, 5, 0}, true},
    {"f32_2x3x4_v2", IoDtype::F32, 3, {2, 3, 4}, false}, // forced (2,0) header — we emit v1 for this size
};

struct StCase
{
    const char* name;
    IoDtype dtype;
    crd::u32 rank;
    crd::u64 shape[2];
};

constexpr StCase kStCases[] = {
    {"w_f32", IoDtype::F32, 2, {16, 32}},   {"w_f64", IoDtype::F64, 2, {4, 4}},
    {"w_f16", IoDtype::F16, 2, {8, 8}},     {"w_bf16", IoDtype::Bf16, 2, {8, 8}},
    {"b_i8", IoDtype::I8, 1, {24, 0}},      {"b_u8", IoDtype::U8, 1, {24, 0}},
    {"b_i16", IoDtype::I16, 1, {5, 0}},     {"b_i32", IoDtype::I32, 1, {5, 0}},
    {"idx_i64", IoDtype::I64, 2, {2, 3}},   {"scalar_f32", IoDtype::F32, 0, {0, 0}},
    {"empty_f32", IoDtype::F32, 2, {0, 4}}, {"w_f8e4m3", IoDtype::Fp8E4m3, 1, {10, 0}},
};

TensorStatus read_ref(crd::memory::IAllocator* alloc, const char* file, crd::containers::Array<crd::u8>& out)
{
    const crd::containers::String p = corpus_path(alloc, "ref", file);
    return crd::hesap::tensor::io_read_file(crd::containers::StringView{p.data(), p.size()}, out);
}

} // namespace

TEST_CASE("io: npy corpus parses bit-exact against the oracle sidecars", "[v14l][io][npy]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    for (const NpyCase& c : kNpyCases)
    {
        INFO("case " << c.name);
        crd::containers::String fn(&alloc);
        fn.append(c.name);
        fn.append(".npy");
        crd::containers::Array<crd::u8> bytes(&alloc);
        REQUIRE(read_ref(&alloc, fn.c_str(), bytes) == TensorStatus::Ok);
        NpyView v;
        REQUIRE(crd::hesap::tensor::npy_parse(crd::containers::as_const_span(bytes), v) == TensorStatus::Ok);
        REQUIRE(v.dtype == c.dtype);
        REQUIRE(v.rank == c.rank);
        for (crd::u32 d = 0; d < c.rank; ++d)
        {
            REQUIRE(v.shape[d] == c.shape[d]);
        }
        crd::containers::String bin(&alloc);
        bin.append(c.name);
        bin.append(".npy.bin");
        crd::containers::Array<crd::u8> ref_payload(&alloc);
        REQUIRE(read_ref(&alloc, bin.c_str(), ref_payload) == TensorStatus::Ok);
        REQUIRE(bytes_equal(v.payload, crd::containers::as_const_span(ref_payload)));
    }
}

TEST_CASE("io: npy writer reproduces numpy v1 files byte-for-byte and round-trips", "[v14l][io][npy]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    for (const NpyCase& c : kNpyCases)
    {
        INFO("case " << c.name);
        crd::containers::String fn(&alloc);
        fn.append(c.name);
        fn.append(".npy");
        crd::containers::Array<crd::u8> ref_bytes(&alloc);
        REQUIRE(read_ref(&alloc, fn.c_str(), ref_bytes) == TensorStatus::Ok);
        NpyView v;
        REQUIRE(crd::hesap::tensor::npy_parse(crd::containers::as_const_span(ref_bytes), v) == TensorStatus::Ok);

        crd::containers::Array<crd::u8> ours(&alloc);
        REQUIRE(crd::hesap::tensor::npy_encode(&alloc, v.dtype, v.shape_span(), v.payload, ours) ==
                TensorStatus::Ok);
        if (c.v1_byte_identical)
        {
            REQUIRE(bytes_equal(crd::containers::as_const_span(ours), crd::containers::as_const_span(ref_bytes)));
        }
        // in-memory re-read of our own bytes
        NpyView v2;
        REQUIRE(crd::hesap::tensor::npy_parse(crd::containers::as_const_span(ours), v2) == TensorStatus::Ok);
        REQUIRE(v2.dtype == v.dtype);
        REQUIRE(v2.rank == v.rank);
        REQUIRE(bytes_equal(v2.payload, v.payload));
        // out/ copy for the python checker
        const crd::containers::String outp = corpus_path(&alloc, "out", fn.c_str());
        REQUIRE(crd::hesap::tensor::io_write_file(crd::containers::StringView{outp.data(), outp.size()},
                                                  crd::containers::as_const_span(ours)) == TensorStatus::Ok);
    }
}

TEST_CASE("io: npy typed read materializes owned tensors with exact bits", "[v14l][io][npy]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    crd::containers::Array<crd::u8> bytes(&alloc);
    REQUIRE(read_ref(&alloc, "f64_5x7.npy", bytes) == TensorStatus::Ok);
    Tensor<crd::f64> t(&alloc);
    REQUIRE(crd::hesap::tensor::npy_read<crd::f64>(&alloc, crd::containers::as_const_span(bytes), t) ==
            TensorStatus::Ok);
    REQUIRE(t.rank() == 2U);
    REQUIRE(t.shape(0) == 5U);
    REQUIRE(t.shape(1) == 7U);
    crd::containers::Array<crd::u8> ref_payload(&alloc);
    REQUIRE(read_ref(&alloc, "f64_5x7.npy.bin", ref_payload) == TensorStatus::Ok);
    REQUIRE(bytes_equal({reinterpret_cast<const crd::u8*>(t.data()), t.size() * sizeof(crd::f64)},
                        crd::containers::as_const_span(ref_payload)));
    // wrong element type is a status, never a silent convert
    Tensor<crd::f32> wrong(&alloc);
    REQUIRE(crd::hesap::tensor::npy_read<crd::f32>(&alloc, crd::containers::as_const_span(bytes), wrong) ==
            TensorStatus::ShapeMismatch);
    // f16 rides the bits carrier
    crd::containers::Array<crd::u8> f16bytes(&alloc);
    REQUIRE(read_ref(&alloc, "f16_3x5.npy", f16bytes) == TensorStatus::Ok);
    Tensor<crd::u16> h(&alloc);
    REQUIRE(crd::hesap::tensor::npy_read_as<crd::u16>(&alloc, crd::containers::as_const_span(f16bytes),
                                                      IoDtype::F16, h) == TensorStatus::Ok);
    REQUIRE(h.size() == 15U);
    // streaming file round-trip (npy_write_file -> npy_read_file) is bit-exact
    {
        const crd::containers::String tmp = corpus_path(&alloc, "out", "stream_roundtrip_f64.npy");
        const crd::containers::StringView tmpv{tmp.data(), tmp.size()};
        REQUIRE(crd::hesap::tensor::npy_write_file<crd::f64>(&alloc, tmpv,
                                                             TensorView<const crd::f64>(t.view())) ==
                TensorStatus::Ok);
        Tensor<crd::f64> back(&alloc);
        REQUIRE(crd::hesap::tensor::npy_read_file<crd::f64>(&alloc, tmpv, back) == TensorStatus::Ok);
        REQUIRE(back.size() == t.size());
        REQUIRE(std::memcmp(back.data(), t.data(), t.size() * sizeof(crd::f64)) == 0);
        // the streamed file is byte-identical to the in-memory encoder's output
        crd::containers::Array<crd::u8> streamed(&alloc);
        REQUIRE(crd::hesap::tensor::io_read_file(tmpv, streamed) == TensorStatus::Ok);
        crd::containers::Array<crd::u8> encoded(&alloc);
        REQUIRE(crd::hesap::tensor::npy_write<crd::f64>(&alloc, TensorView<const crd::f64>(t.view()), encoded) ==
                TensorStatus::Ok);
        REQUIRE(bytes_equal(crd::containers::as_const_span(streamed), crd::containers::as_const_span(encoded)));
    }
}

TEST_CASE("io: npy adversaries return clean statuses", "[v14l][io][npy][adversary]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    crd::containers::Array<crd::u8> good(&alloc);
    REQUIRE(read_ref(&alloc, "f32_2x3x4.npy", good) == TensorStatus::Ok);
    NpyView v;
    // truncated payload
    REQUIRE(crd::hesap::tensor::npy_parse({good.data(), good.size() / 2U}, v) == TensorStatus::BadInput);
    // truncated preamble
    REQUIRE(crd::hesap::tensor::npy_parse({good.data(), 4U}, v) == TensorStatus::BadInput);
    // garbage magic
    {
        crd::containers::Array<crd::u8> bad(good, &alloc);
        bad.data()[0] = 0x00U;
        REQUIRE(crd::hesap::tensor::npy_parse(crd::containers::as_const_span(bad), v) == TensorStatus::BadInput);
    }
    // header length past end of file (the huge-header adversary)
    {
        crd::containers::Array<crd::u8> bad(good, &alloc);
        bad.data()[8] = 0xFFU;
        bad.data()[9] = 0xFFU;
        REQUIRE(crd::hesap::tensor::npy_parse(crd::containers::as_const_span(bad), v) == TensorStatus::BadInput);
    }
    // big-endian descr
    {
        crd::containers::Array<crd::u8> bad(good, &alloc);
        for (crd::usize i = 10; i < bad.size() && i < 40U; ++i)
        {
            if (bad.data()[i] == '<')
            {
                bad.data()[i] = '>';
                break;
            }
        }
        REQUIRE(crd::hesap::tensor::npy_parse(crd::containers::as_const_span(bad), v) == TensorStatus::Unsupported);
    }
    // fortran_order=True (numpy-written) is Unsupported, loudly
    {
        crd::containers::Array<crd::u8> f(&alloc);
        REQUIRE(read_ref(&alloc, "f32_fortran.npy", f) == TensorStatus::Ok);
        REQUIRE(crd::hesap::tensor::npy_parse(crd::containers::as_const_span(f), v) == TensorStatus::Unsupported);
    }
    // bf16 has no npy descr — write refuses instead of inventing one
    {
        const crd::u64 shp[1] = {4U};
        const crd::u16 bits[4] = {0x3F80U, 0x0000U, 0xBF80U, 0x7F80U};
        crd::containers::Array<crd::u8> out(&alloc);
        REQUIRE(crd::hesap::tensor::npy_write_as<crd::u16>(&alloc, IoDtype::Bf16, {shp, 1U}, {bits, 4U}, out) ==
                TensorStatus::Unsupported);
    }
    // missing file is a status, not a crash
    {
        crd::containers::Array<crd::u8> none(&alloc);
        REQUIRE(read_ref(&alloc, "does_not_exist.npy", none) == TensorStatus::BadInput);
    }
}

TEST_CASE("io: npz stored + deflated members read bit-exact (CRC-gated)", "[v14l][io][npz]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const char* archives[2] = {"ref_stored.npz", "ref_deflate.npz"};
    const char* keys[3] = {"a", "b", "c"};
    const IoDtype dtypes[3] = {IoDtype::F32, IoDtype::I64, IoDtype::F16};
    for (const char* archive : archives)
    {
        INFO("archive " << archive);
        crd::containers::Array<crd::u8> bytes(&alloc);
        REQUIRE(read_ref(&alloc, archive, bytes) == TensorStatus::Ok);
        crd::hesap::tensor::NpzReader rd(&alloc);
        REQUIRE(rd.parse(crd::containers::as_const_span(bytes)) == TensorStatus::Ok);
        REQUIRE(rd.count() == 3U);
        for (crd::u32 k = 0; k < 3U; ++k)
        {
            INFO("member " << keys[k]);
            const crd::i64 idx = rd.find(keys[k]);
            REQUIRE(idx >= 0);
            NpyView v;
            REQUIRE(rd.npy(static_cast<crd::usize>(idx), v) == TensorStatus::Ok);
            REQUIRE(v.dtype == dtypes[k]);
            crd::containers::String bin(&alloc);
            bin.append("npz_");
            bin.append(keys[k]);
            bin.append(".bin");
            crd::containers::Array<crd::u8> ref_payload(&alloc);
            REQUIRE(read_ref(&alloc, bin.c_str(), ref_payload) == TensorStatus::Ok);
            REQUIRE(bytes_equal(v.payload, crd::containers::as_const_span(ref_payload)));
        }
    }
}

TEST_CASE("io: npz writer round-trips through our reader and numpy's", "[v14l][io][npz]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const char* archives[2] = {"ref_stored.npz", "ref_deflate.npz"};
    const char* outs[2] = {"roundtrip_stored.npz", "roundtrip_deflate.npz"};
    for (crd::u32 a = 0; a < 2U; ++a)
    {
        INFO("archive " << archives[a]);
        crd::containers::Array<crd::u8> bytes(&alloc);
        REQUIRE(read_ref(&alloc, archives[a], bytes) == TensorStatus::Ok);
        crd::hesap::tensor::NpzReader rd(&alloc);
        REQUIRE(rd.parse(crd::containers::as_const_span(bytes)) == TensorStatus::Ok);
        crd::hesap::tensor::NpzWriter wr(&alloc);
        const char* keys[3] = {"a", "b", "c"};
        for (const char* key : keys)
        {
            const crd::i64 idx = rd.find(key);
            REQUIRE(idx >= 0);
            NpyView v;
            REQUIRE(rd.npy(static_cast<crd::usize>(idx), v) == TensorStatus::Ok);
            REQUIRE(wr.add(key, v.dtype, v.shape_span(), v.payload) == TensorStatus::Ok);
        }
        crd::containers::Array<crd::u8> ours(&alloc);
        REQUIRE(wr.finish(ours) == TensorStatus::Ok);
        // re-read our own zip and byte-compare payloads
        crd::hesap::tensor::NpzReader rd2(&alloc);
        REQUIRE(rd2.parse(crd::containers::as_const_span(ours)) == TensorStatus::Ok);
        REQUIRE(rd2.count() == 3U);
        for (const char* key : keys)
        {
            NpyView v0;
            NpyView v1;
            REQUIRE(rd.npy(static_cast<crd::usize>(rd.find(key)), v0) == TensorStatus::Ok);
            REQUIRE(rd2.npy(static_cast<crd::usize>(rd2.find(key)), v1) == TensorStatus::Ok);
            REQUIRE(v0.dtype == v1.dtype);
            REQUIRE(bytes_equal(v0.payload, v1.payload));
        }
        const crd::containers::String outp = corpus_path(&alloc, "out", outs[a]);
        REQUIRE(crd::hesap::tensor::io_write_file(crd::containers::StringView{outp.data(), outp.size()},
                                                  crd::containers::as_const_span(ours)) == TensorStatus::Ok);
    }
}

TEST_CASE("io: npz adversaries return clean statuses", "[v14l][io][npz][adversary]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    crd::containers::Array<crd::u8> good(&alloc);
    REQUIRE(read_ref(&alloc, "ref_stored.npz", good) == TensorStatus::Ok);
    crd::hesap::tensor::NpzReader rd(&alloc);
    // truncated
    REQUIRE(rd.parse({good.data(), good.size() / 2U}) == TensorStatus::BadInput);
    REQUIRE(rd.parse({good.data(), 10U}) == TensorStatus::BadInput);
    // corrupt payload byte -> CRC mismatch
    {
        crd::containers::Array<crd::u8> bad(good, &alloc);
        bad.data()[80] ^= 0xFFU; // inside the first member's npy payload region
        REQUIRE(rd.parse(crd::containers::as_const_span(bad)) == TensorStatus::BadInput);
    }
    // not a zip at all
    {
        const crd::u8 junk[32] = {1, 2, 3, 4};
        REQUIRE(rd.parse({junk, 32U}) == TensorStatus::BadInput);
    }
}

TEST_CASE("io: safetensors corpus parses bit-exact incl bf16 and fp8", "[v14l][io][safetensors]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    crd::containers::Array<crd::u8> bytes(&alloc);
    REQUIRE(read_ref(&alloc, "ref.safetensors", bytes) == TensorStatus::Ok);
    crd::hesap::tensor::SafetensorsFile f(&alloc);
    REQUIRE(f.parse(crd::containers::as_const_span(bytes)) == TensorStatus::Ok);
    REQUIRE(f.tensor_count() == sizeof(kStCases) / sizeof(kStCases[0]));
    for (const StCase& c : kStCases)
    {
        INFO("tensor " << c.name);
        const crd::i64 idx = f.find(c.name);
        REQUIRE(idx >= 0);
        const crd::hesap::tensor::SafetensorsEntry& e = f.tensor(static_cast<crd::usize>(idx));
        REQUIRE(e.dtype == c.dtype);
        REQUIRE(e.rank == c.rank);
        for (crd::u32 d = 0; d < c.rank; ++d)
        {
            REQUIRE(e.shape[d] == c.shape[d]);
        }
        crd::containers::String bin(&alloc);
        bin.append("st_");
        bin.append(c.name);
        bin.append(".bin");
        crd::containers::Array<crd::u8> ref_payload(&alloc);
        REQUIRE(read_ref(&alloc, bin.c_str(), ref_payload) == TensorStatus::Ok);
        REQUIRE(bytes_equal(e.payload, crd::containers::as_const_span(ref_payload)));
    }
    // metadata written by the oracle
    REQUIRE(f.metadata_find("framework") >= 0);
    REQUIRE(f.metadata_value(static_cast<crd::usize>(f.metadata_find("framework"))) == "cerid-v14l-oracle");
    // typed + bits-carrier materialisation
    Tensor<crd::f32> w(&alloc);
    REQUIRE(f.read<crd::f32>(static_cast<crd::usize>(f.find("w_f32")), &alloc, w) == TensorStatus::Ok);
    REQUIRE(w.shape(0) == 16U);
    REQUIRE(w.shape(1) == 32U);
    Tensor<crd::u16> bf(&alloc);
    REQUIRE(f.read_as<crd::u16>(static_cast<crd::usize>(f.find("w_bf16")), IoDtype::Bf16, &alloc, bf) ==
            TensorStatus::Ok);
    REQUIRE(bf.size() == 64U);
    // exact-dtype contract
    Tensor<crd::f64> wrong(&alloc);
    REQUIRE(f.read<crd::f64>(static_cast<crd::usize>(f.find("w_f32")), &alloc, wrong) ==
            TensorStatus::ShapeMismatch);
}

TEST_CASE("io: safetensors writer round-trips through our reader and theirs", "[v14l][io][safetensors]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    crd::containers::Array<crd::u8> bytes(&alloc);
    REQUIRE(read_ref(&alloc, "ref.safetensors", bytes) == TensorStatus::Ok);
    crd::hesap::tensor::SafetensorsFile f(&alloc);
    REQUIRE(f.parse(crd::containers::as_const_span(bytes)) == TensorStatus::Ok);

    crd::hesap::tensor::SafetensorsWriter wr(&alloc);
    for (const StCase& c : kStCases)
    {
        const crd::hesap::tensor::SafetensorsEntry& e = f.tensor(static_cast<crd::usize>(f.find(c.name)));
        REQUIRE(wr.add(e.name_view(), e.dtype, e.shape_span(), e.payload) == TensorStatus::Ok);
    }
    REQUIRE(wr.add_metadata("framework", "cerid-v14l-oracle") == TensorStatus::Ok);
    REQUIRE(wr.add_metadata("note", "bit-exact interop gate") == TensorStatus::Ok);
    crd::containers::Array<crd::u8> ours(&alloc);
    REQUIRE(wr.finish(ours) == TensorStatus::Ok);

    crd::hesap::tensor::SafetensorsFile f2(&alloc);
    REQUIRE(f2.parse(crd::containers::as_const_span(ours)) == TensorStatus::Ok);
    REQUIRE(f2.tensor_count() == f.tensor_count());
    for (const StCase& c : kStCases)
    {
        INFO("tensor " << c.name);
        const crd::i64 i0 = f.find(c.name);
        const crd::i64 i1 = f2.find(c.name);
        REQUIRE(i1 >= 0);
        const crd::hesap::tensor::SafetensorsEntry& e0 = f.tensor(static_cast<crd::usize>(i0));
        const crd::hesap::tensor::SafetensorsEntry& e1 = f2.tensor(static_cast<crd::usize>(i1));
        REQUIRE(e0.dtype == e1.dtype);
        REQUIRE(e0.rank == e1.rank);
        REQUIRE(bytes_equal(e0.payload, e1.payload));
    }
    // finish_file streams byte-identical output (write the out/ artifact
    // through the STREAMING path so the python checker gates it end-to-end)
    const crd::containers::String outp = corpus_path(&alloc, "out", "roundtrip.safetensors");
    const crd::containers::StringView outv{outp.data(), outp.size()};
    REQUIRE(wr.finish_file(outv) == TensorStatus::Ok);
    crd::containers::Array<crd::u8> streamed(&alloc);
    REQUIRE(crd::hesap::tensor::io_read_file(outv, streamed) == TensorStatus::Ok);
    REQUIRE(bytes_equal(crd::containers::as_const_span(streamed), crd::containers::as_const_span(ours)));

    // streaming single-tensor read: file -> owned tensor, bit-exact
    {
        const crd::containers::String refp = corpus_path(&alloc, "ref", "ref.safetensors");
        const crd::containers::StringView refv{refp.data(), refp.size()};
        Tensor<crd::f32> w(&alloc);
        REQUIRE(crd::hesap::tensor::safetensors_read_tensor_file<crd::f32>(&alloc, refv, "w_f32", w) ==
                TensorStatus::Ok);
        REQUIRE(w.shape(0) == 16U);
        crd::containers::Array<crd::u8> ref_payload(&alloc);
        REQUIRE(read_ref(&alloc, "st_w_f32.bin", ref_payload) == TensorStatus::Ok);
        REQUIRE(bytes_equal({reinterpret_cast<const crd::u8*>(w.data()), w.size() * sizeof(crd::f32)},
                            crd::containers::as_const_span(ref_payload)));
        Tensor<crd::u16> bf(&alloc);
        REQUIRE(crd::hesap::tensor::safetensors_read_tensor_file_as<crd::u16>(&alloc, refv, "w_bf16",
                                                                              IoDtype::Bf16, bf) ==
                TensorStatus::Ok);
        REQUIRE(bf.size() == 64U);
        Tensor<crd::f32> missing(&alloc);
        REQUIRE(crd::hesap::tensor::safetensors_read_tensor_file<crd::f32>(&alloc, refv, "nope", missing) ==
                TensorStatus::BadInput);
    }
}

TEST_CASE("io: safetensors adversaries return clean statuses", "[v14l][io][safetensors][adversary]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    crd::hesap::tensor::SafetensorsFile f(&alloc);
    // too short / header past end
    {
        const crd::u8 tiny[4] = {1, 0, 0, 0};
        REQUIRE(f.parse({tiny, 4U}) == TensorStatus::BadInput);
        const crd::u8 lying[9] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0, 0, 0, 0, '{'};
        REQUIRE(f.parse({lying, 9U}) == TensorStatus::BadInput);
    }
    const auto build = [](const char* header, const char* buffer, crd::usize buf_len,
                          crd::containers::Array<crd::u8>& out)
    {
        const crd::usize hlen = std::strlen(header);
        out.clear();
        out.resize_uninitialized(8U + hlen + buf_len);
        crd::u64 n = hlen;
        std::memcpy(out.data(), &n, 8U);
        std::memcpy(out.data() + 8U, header, hlen);
        if (buf_len > 0U)
        {
            std::memcpy(out.data() + 8U + hlen, buffer, buf_len);
        }
    };
    crd::containers::Array<crd::u8> file(&alloc);
    const char zeros4[4] = {}; // explicit zero payload (no embedded-NUL literals)
    // malformed JSON
    build(R"({"a":{"dtype":"F32",)", "", 0U, file);
    REQUIRE(f.parse(crd::containers::as_const_span(file)) == TensorStatus::BadInput);
    // offsets that do not match shape*itemsize
    build(R"({"a":{"dtype":"F32","shape":[2],"data_offsets":[0,4]}})", zeros4, 4U, file);
    REQUIRE(f.parse(crd::containers::as_const_span(file)) == TensorStatus::BadInput);
    // offsets past the buffer
    build(R"({"a":{"dtype":"F32","shape":[2],"data_offsets":[0,8]}})", zeros4, 4U, file);
    REQUIRE(f.parse(crd::containers::as_const_span(file)) == TensorStatus::BadInput);
    // unknown dtype string
    build(R"({"a":{"dtype":"X32","shape":[1],"data_offsets":[0,4]}})", zeros4, 4U, file);
    REQUIRE(f.parse(crd::containers::as_const_span(file)) == TensorStatus::Unsupported);
    // rank overflow (9 dims > kMaxRank)
    build(R"({"a":{"dtype":"F32","shape":[1,1,1,1,1,1,1,1,1],"data_offsets":[0,4]}})", zeros4, 4U,
          file);
    REQUIRE(f.parse(crd::containers::as_const_span(file)) == TensorStatus::RankOverflow);
    // duplicate tensor names
    build(R"({"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]},"a":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})",
          zeros4, 4U, file);
    REQUIRE(f.parse(crd::containers::as_const_span(file)) == TensorStatus::BadInput);
    // trailing junk after the closing brace (spec allows only space padding)
    build("{}garbage", "", 0U, file);
    REQUIRE(f.parse(crd::containers::as_const_span(file)) == TensorStatus::BadInput);
    // escaped names parse correctly (A = 'A')
    build(R"({"\u0041":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})", zeros4, 4U, file);
    REQUIRE(f.parse(crd::containers::as_const_span(file)) == TensorStatus::Ok);
    REQUIRE(f.find("A") >= 0);
}

namespace
{
bool g_dlpack_deleted = false;
void mark_deleted(DLManagedTensorVersioned* self)
{
    (void)self;
    g_dlpack_deleted = true;
}
} // namespace

TEST_CASE("io: dlpack export-import identity is pointer-equal zero-copy", "[v14l][io][dlpack]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 shp[3] = {4U, 5U, 6U};
    Tensor<crd::f64> t(&alloc, {shp, 3});
    for (crd::u64 i = 0; i < t.size(); ++i)
    {
        t.data()[i] = static_cast<crd::f64>(i) * 0.5;
    }
    // mutable export: flags 0, writable import, shared buffer
    DLManagedTensorVersioned* cap = nullptr;
    REQUIRE(crd::hesap::tensor::dlpack_export(t.view(), &alloc, cap) == TensorStatus::Ok);
    REQUIRE(cap != nullptr);
    REQUIRE(cap->version.major == DLPACK_MAJOR_VERSION);
    REQUIRE(cap->flags == 0U);
    REQUIRE(cap->dl_tensor.data == t.data());
    REQUIRE(cap->dl_tensor.ndim == 3);
    REQUIRE(cap->dl_tensor.dtype.code == kDLFloat);
    REQUIRE(cap->dl_tensor.dtype.bits == 64U);
    REQUIRE(cap->dl_tensor.shape[1] == 5);
    REQUIRE(cap->dl_tensor.strides[0] == 30);
    {
        crd::hesap::tensor::DlpackImported<crd::f64> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import(cap, imp) == TensorStatus::Ok);
        REQUIRE(imp.view().data() == t.data()); // ZERO copy — the identity gate
        REQUIRE(imp.view().rank() == 3U);
        REQUIRE(imp.view().shape(2) == 6U);
        REQUIRE(imp.view().stride(2) == 1);
        imp.view()(1U, 2U, 3U) = 123.25; // writable — mutation lands in the producer
        REQUIRE(t.view()(1U, 2U, 3U) == 123.25);
    } // handle destructor calls the producer's deleter exactly once

    // const export: READ_ONLY stamped; writable import refused
    const Tensor<crd::f64>& ct = t;
    DLManagedTensorVersioned* rcap = nullptr;
    REQUIRE(crd::hesap::tensor::dlpack_export(ct.view(), &alloc, rcap) == TensorStatus::Ok);
    REQUIRE((rcap->flags & DLPACK_FLAG_BITMASK_READ_ONLY) != 0U);
    {
        crd::hesap::tensor::DlpackImported<crd::f64> imp_w;
        REQUIRE(crd::hesap::tensor::dlpack_import(rcap, imp_w) == TensorStatus::Unsupported);
        crd::hesap::tensor::DlpackImported<const crd::f64> imp_r;
        REQUIRE(crd::hesap::tensor::dlpack_import(rcap, imp_r) == TensorStatus::Ok);
        REQUIRE(imp_r.read_only());
        REQUIRE(imp_r.view().data() == t.data());
    }

    // strided view export: slice metadata crosses the ABI intact
    const TensorView<const crd::f64> sl = TensorView<const crd::f64>(t.view()).slice(1U, 1U, 5U, 2U);
    DLManagedTensorVersioned* scap = nullptr;
    REQUIRE(crd::hesap::tensor::dlpack_export(sl, &alloc, scap) == TensorStatus::Ok);
    {
        crd::hesap::tensor::DlpackImported<const crd::f64> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import(scap, imp) == TensorStatus::Ok);
        REQUIRE(imp.view().data() == sl.data());
        REQUIRE(imp.view().shape(1) == 2U);
        REQUIRE(imp.view().stride(1) == 12);
        REQUIRE(std::bit_cast<crd::u64>(imp.view()(2U, 1U, 4U)) == std::bit_cast<crd::u64>(sl(2U, 1U, 4U)));
    }
}

TEST_CASE("io: dlpack legacy ABI, spec deleter rules, and rejects", "[v14l][io][dlpack]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 shp[2] = {3U, 4U};
    Tensor<crd::f32> t(&alloc, {shp, 2});
    for (crd::u64 i = 0; i < t.size(); ++i)
    {
        t.data()[i] = static_cast<crd::f32>(i);
    }
    // legacy round-trip
    DLManagedTensor* leg = nullptr;
    REQUIRE(crd::hesap::tensor::dlpack_export_legacy(t.view(), &alloc, leg) == TensorStatus::Ok);
    {
        crd::hesap::tensor::DlpackImported<crd::f32> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import_legacy(leg, imp) == TensorStatus::Ok);
        REQUIRE(imp.view().data() == t.data());
    }
    // NULL strides = compact row-major (consumer-side default)
    crd::i64 shape_arr[2] = {3, 4};
    DLManagedTensor manual{};
    manual.dl_tensor.data = t.data();
    manual.dl_tensor.device = DLDevice{kDLCPU, 0};
    manual.dl_tensor.ndim = 2;
    manual.dl_tensor.dtype = DLDataType{kDLFloat, 32U, 1U};
    manual.dl_tensor.shape = shape_arr;
    manual.dl_tensor.strides = nullptr;
    manual.deleter = nullptr; // no deleter — handle tolerates it
    {
        crd::hesap::tensor::DlpackImported<crd::f32> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import_legacy(&manual, imp) == TensorStatus::Ok);
        REQUIRE(imp.view().stride(0) == 4);
        REQUIRE(imp.view().stride(1) == 1);
    }
    // major-version mismatch: spec says call the deleter, and only it
    g_dlpack_deleted = false;
    DLManagedTensorVersioned future{};
    future.version = DLPackVersion{DLPACK_MAJOR_VERSION + 1U, 0U};
    future.deleter = &mark_deleted;
    {
        crd::hesap::tensor::DlpackImported<crd::f32> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import(&future, imp) == TensorStatus::Unsupported);
        REQUIRE(g_dlpack_deleted);
    }
    // dtype mismatch: no ownership taken — caller still disposes
    DLManagedTensorVersioned* cap = nullptr;
    REQUIRE(crd::hesap::tensor::dlpack_export(t.view(), &alloc, cap) == TensorStatus::Ok);
    {
        crd::hesap::tensor::DlpackImported<crd::f64> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import(cap, imp) == TensorStatus::ShapeMismatch);
    }
    cap->deleter(cap);
    // device mismatch
    DLManagedTensorVersioned gpu{};
    gpu.version = DLPackVersion{DLPACK_MAJOR_VERSION, 0U};
    gpu.dl_tensor.device = DLDevice{kDLCUDA, 0};
    gpu.dl_tensor.ndim = 0;
    gpu.dl_tensor.dtype = DLDataType{kDLFloat, 32U, 1U};
    {
        crd::hesap::tensor::DlpackImported<crd::f32> imp;
        REQUIRE(crd::hesap::tensor::dlpack_import(&gpu, imp) == TensorStatus::Unsupported);
    }
}

TEST_CASE("io: philox_fill is order-independent - the worker moat and the sequential anchor",
          "[v14l][io][philox][moat]")
{
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 shp[1] = {10007U};
    Tensor<crd::f64> serial(&alloc, {shp, 1});
    REQUIRE(crd::hesap::tensor::philox_fill_uniform(serial.view(), 42U, 7U, 1U) == TensorStatus::Ok);
    // anchor: identical bits to sequential PhiloxRng draws
    {
        crd::hesap::stats::PhiloxRng rng(42U, 7U);
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < shp[0]; ++i)
        {
            if (std::bit_cast<crd::u64>(serial.data()[i]) != std::bit_cast<crd::u64>(rng.next_f64()))
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
    // run-twice determinism
    {
        Tensor<crd::f64> again(&alloc, {shp, 1});
        REQUIRE(crd::hesap::tensor::philox_fill_uniform(again.view(), 42U, 7U, 1U) == TensorStatus::Ok);
        REQUIRE(std::memcmp(again.data(), serial.data(), shp[0] * sizeof(crd::f64)) == 0);
    }
    // the {1,2,4,8,16} moat
    for (const crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        Tensor<crd::f64> par(&alloc, {shp, 1});
        const TensorStatus st = crd::hesap::tensor::philox_fill_uniform(par.view(), 42U, 7U, 0U);
        crd::jobs::shutdown();
        REQUIRE(st == TensorStatus::Ok);
        INFO("workers " << nw);
        REQUIRE(std::memcmp(par.data(), serial.data(), shp[0] * sizeof(crd::f64)) == 0);
    }
    // f32 anchor
    {
        const crd::u64 shpf[2] = {31U, 33U};
        Tensor<crd::f32> f(&alloc, {shpf, 2});
        REQUIRE(crd::hesap::tensor::philox_fill_uniform(f.view(), 9U, 0U, 1U) == TensorStatus::Ok);
        crd::hesap::stats::PhiloxRng rng(9U, 0U);
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < f.size(); ++i)
        {
            if (std::bit_cast<crd::u32>(f.data()[i]) != std::bit_cast<crd::u32>(rng.next_f32()))
            {
                ++mism;
            }
        }
        REQUIRE(mism == 0U);
    }
    // strided destination: the key domain is the CANONICAL logical index, so
    // a strided fill equals the contiguous fill element-for-element
    {
        const crd::u64 big[2] = {20U, 80U};
        const crd::u64 small[2] = {20U, 40U};
        Tensor<crd::f64> wide = Tensor<crd::f64>::zeros(&alloc, {big, 2});
        Tensor<crd::f64> dense(&alloc, {small, 2});
        REQUIRE(crd::hesap::tensor::philox_fill_uniform(dense.view(), 5U, 0U, 1U) == TensorStatus::Ok);
        const TensorView<crd::f64> strided = wide.view().slice(1U, 0U, 80U, 2U);
        REQUIRE(crd::hesap::tensor::philox_fill_uniform(strided, 5U, 0U, 1U) == TensorStatus::Ok);
        crd::u64 mism = 0;
        for (crd::u64 i = 0; i < 20U; ++i)
        {
            for (crd::u64 j = 0; j < 40U; ++j)
            {
                if (std::bit_cast<crd::u64>(strided(i, j)) != std::bit_cast<crd::u64>(dense.view()(i, j)))
                {
                    ++mism;
                }
            }
        }
        REQUIRE(mism == 0U);
    }
    // out/ artifact for the python checker (structural validity from numpy)
    {
        const crd::u64 shpn[2] = {64U, 65U};
        Tensor<crd::f64> t(&alloc, {shpn, 2});
        REQUIRE(crd::hesap::tensor::philox_fill_uniform(t.view(), 2026U, 0U, 1U) == TensorStatus::Ok);
        const crd::containers::String outp = corpus_path(&alloc, "out", "philox_fill_f64.npy");
        REQUIRE(crd::hesap::tensor::npy_write_file<crd::f64>(&alloc, crd::containers::StringView{outp.data(),
                                                                                                 outp.size()},
                                                             t.view()) == TensorStatus::Ok);
    }
}

TEST_CASE("io: crdr TNSR cook and load round-trip with the corruption hash", "[v14l][io][crdr]")
{
    using namespace crd::hesap::resources;
    crd::memory::TlsfAllocator alloc(kPool);
    const crd::u64 shp[3] = {3U, 4U, 5U};
    Tensor<crd::f32> t(&alloc, {shp, 3});
    REQUIRE(crd::hesap::tensor::philox_fill_uniform(t.view(), 77U, 0U, 1U) == TensorStatus::Ok);

    const crd::resources::ResourceId id{0x1234U, 0x5678U};
    TensorStatus st = TensorStatus::Ok;
    const crd::containers::Array<crd::u8> blob =
        cook_tensor<crd::f32>(&alloc, id, TensorView<const crd::f32>(t.view()), st);
    REQUIRE(st == TensorStatus::Ok);
    REQUIRE(blob.size() > 0U);

    TensorResource res(&alloc);
    REQUIRE(load_tensor(crd::containers::as_const_span(blob), res, &alloc) == TensorStatus::Ok);
    REQUIRE(res.dtype() == IoDtype::F32);
    REQUIRE(res.rank() == 3U);
    REQUIRE(res.element_count() == 60U);
    const Tensor<crd::f32> back = res.build<crd::f32>(&alloc);
    REQUIRE(back.size() == t.size());
    crd::u64 mism = 0;
    for (crd::u64 i = 0; i < t.size(); ++i)
    {
        if (std::bit_cast<crd::u32>(back.data()[i]) != std::bit_cast<crd::u32>(t.data()[i]))
        {
            ++mism;
        }
    }
    REQUIRE(mism == 0U);

    // storage-dtype cook (bf16 bits) through the type-erased site
    const crd::u64 shp2[1] = {6U};
    const crd::u16 bits[6] = {0x3F80U, 0xBF80U, 0x0000U, 0x8000U, 0x7F80U, 0x4049U};
    const crd::containers::Array<crd::u8> blob2 =
        cook_tensor_bits(&alloc, id, IoDtype::Bf16, {shp2, 1U},
                         {reinterpret_cast<const crd::u8*>(bits), sizeof(bits)}, st);
    REQUIRE(st == TensorStatus::Ok);
    TensorResource res2(&alloc);
    REQUIRE(load_tensor(crd::containers::as_const_span(blob2), res2, &alloc) == TensorStatus::Ok);
    REQUIRE(res2.dtype() == IoDtype::Bf16);
    const Tensor<crd::u16> back2 = res2.build_bits<crd::u16>(&alloc);
    REQUIRE(std::memcmp(back2.data(), bits, sizeof(bits)) == 0);

    // adversaries: truncated container, wrong type fourcc
    TensorResource bad(&alloc);
    REQUIRE(load_tensor({blob.data(), blob.size() / 2U}, bad, &alloc) == TensorStatus::BadInput);
    {
        crd::containers::Array<crd::u8> wrong(blob, &alloc);
        wrong.data()[24] ^= 0xFFU; // type_fourcc byte
        REQUIRE(load_tensor(crd::containers::as_const_span(wrong), bad, &alloc) == TensorStatus::BadInput);
    }
}
