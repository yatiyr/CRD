#!/usr/bin/env python3
# v14-l tensor I/O oracle (Phase 3.1.6 — crd-hesap-tensor io.hpp gate).
#
# gen   : write the reference corpus (npy / npz / safetensors) + raw-byte
#         sidecars (<file>.bin per payload) + manifest.json into
#         tests/hesap-tensor/io_corpus/ref/. The C++ gate (test_io.cpp)
#         round-trips THESE files bit-exactly.
# check : read the files the C++ gate wrote into io_corpus/out/ back through
#         numpy / torch / safetensors and verify BIT-EXACT equality against
#         the reference corpus. Exit nonzero on any mismatch.
# bench : python-side timing baseline for the v14-l board (npy + safetensors
#         read/write on a ~500 MB f32 corpus in /tmp).
#
# Determinism: all arrays derive from np.random.Philox(seed) — regeneration
# is bit-identical; ref/*.bin are the payload ground truth for the C++ side.
import io
import json
import os
import sys
import time
import zipfile

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(REPO, "tests", "hesap-tensor", "io_corpus")
REF = os.path.join(CORPUS, "ref")
OUT = os.path.join(CORPUS, "out")


def rng(seed):
    return np.random.Generator(np.random.Philox(key=seed))


# ---------------------------------------------------------------------------
# The pinned corpus tables. The C++ gate hardcodes the same names/dtypes/
# shapes (both sides pinned to this table — change together or not at all).
# ---------------------------------------------------------------------------

def make_npy_entries():
    e = []
    e.append(("f32_2x3x4", (rng(1).random((2, 3, 4)) * 2 - 1).astype("<f4")))
    e.append(("f64_5x7", (rng(2).random((5, 7)) * 2 - 1).astype("<f8")))
    e.append(("f16_3x5", (rng(3).random((3, 5)) * 2 - 1).astype("<f2")))
    e.append(("i8_16", rng(4).integers(-128, 128, size=16, dtype=np.int64).astype("|i1")))
    e.append(("i16_9", rng(5).integers(-32768, 32768, size=9, dtype=np.int64).astype("<i2")))
    e.append(("i32_4x4", rng(6).integers(-(2**31), 2**31, size=(4, 4), dtype=np.int64).astype("<i4")))
    e.append(("i64_2x2", rng(7).integers(-(2**62), 2**62, size=(2, 2), dtype=np.int64).astype("<i8")))
    e.append(("u8_8", rng(8).integers(0, 256, size=8, dtype=np.int64).astype("|u1")))
    e.append(("u16_6", rng(9).integers(0, 65536, size=6, dtype=np.int64).astype("<u2")))
    e.append(("u32_3", rng(10).integers(0, 2**32, size=3, dtype=np.uint64).astype("<u4")))
    e.append(("u64_2x5", rng(11).integers(0, 2**63, size=(2, 5), dtype=np.uint64).astype("<u8")))
    e.append(("bool_7", (rng(12).random(7) > 0.5)))
    e.append(("c32_2x3", (rng(13).random((2, 3)) + 1j * rng(14).random((2, 3))).astype("<c8")))
    e.append(("c64_3", (rng(15).random(3) + 1j * rng(16).random(3)).astype("<c16")))
    e.append(("f32_0d", np.float32(3.5)))
    e.append(("f32_empty_0x5", np.zeros((0, 5), dtype="<f4")))
    return e


def make_npz_entries():
    return [
        ("a", (rng(21).random((3, 4)) * 2 - 1).astype("<f4")),
        ("b", rng(22).integers(-(2**31), 2**31, size=(2, 6), dtype=np.int64).astype("<i8")),
        ("c", (rng(23).random(11) * 2 - 1).astype("<f2")),
    ]


def make_st_entries():
    # (name, torch tensor) — torch is the bf16 / f8 source of truth.
    import torch

    ent = []
    ent.append(("w_f32", torch.from_numpy((rng(31).random((16, 32)) * 2 - 1).astype("<f4"))))
    ent.append(("w_f64", torch.from_numpy((rng(32).random((4, 4)) * 2 - 1).astype("<f8"))))
    ent.append(("w_f16", torch.from_numpy((rng(33).random((8, 8)) * 2 - 1).astype("<f4")).to(torch.float16)))
    ent.append(("w_bf16", torch.from_numpy((rng(34).random((8, 8)) * 2 - 1).astype("<f4")).to(torch.bfloat16)))
    ent.append(("b_i8", torch.from_numpy(rng(35).integers(-128, 128, size=24, dtype=np.int64).astype("|i1"))))
    ent.append(("b_u8", torch.from_numpy(rng(36).integers(0, 256, size=24, dtype=np.int64).astype("|u1"))))
    ent.append(("b_i16", torch.from_numpy(rng(37).integers(-32768, 32768, size=5, dtype=np.int64).astype("<i2"))))
    ent.append(("b_i32", torch.from_numpy(rng(38).integers(-(2**31), 2**31, size=5, dtype=np.int64).astype("<i4"))))
    ent.append(("idx_i64", torch.from_numpy(rng(39).integers(0, 2**62, size=(2, 3), dtype=np.uint64).astype("<i8"))))
    ent.append(("scalar_f32", torch.tensor(2.75, dtype=torch.float32)))
    ent.append(("empty_f32", torch.zeros((0, 4), dtype=torch.float32)))
    try:
        f8 = torch.from_numpy((rng(40).random(10) * 2 - 1).astype("<f4")).to(torch.float8_e4m3fn)
        ent.append(("w_f8e4m3", f8))
    except (AttributeError, RuntimeError):
        print("NOTE: torch.float8_e4m3fn unavailable — f8 safetensors entry skipped")
    return ent


ST_META = {"framework": "cerid-v14l-oracle", "note": "bit-exact interop gate"}


def st_raw_bytes(t):
    import torch

    if t.dtype == torch.bfloat16:
        return t.view(torch.uint16).numpy().tobytes()
    if t.dtype in (getattr(torch, "float8_e4m3fn", None), getattr(torch, "float8_e5m2", None)):
        return t.view(torch.uint8).numpy().tobytes()
    return t.numpy().tobytes()


# ---------------------------------------------------------------------------
def gen():
    os.makedirs(REF, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    manifest = {"npy": [], "npz": [], "safetensors": []}

    for name, arr in make_npy_entries():
        arr = np.asarray(arr)
        path = os.path.join(REF, name + ".npy")
        np.save(path, arr)
        with open(path + ".bin", "wb") as f:
            f.write(arr.tobytes(order="C"))
        manifest["npy"].append({"name": name, "descr": arr.dtype.str, "shape": list(arr.shape)})

    # forced version-(2,0) header variant
    arr = np.asarray(make_npy_entries()[0][1])
    with open(os.path.join(REF, "f32_2x3x4_v2.npy"), "wb") as f:
        np.lib.format.write_array(f, arr, version=(2, 0))
    with open(os.path.join(REF, "f32_2x3x4_v2.npy.bin"), "wb") as f:
        f.write(arr.tobytes(order="C"))
    manifest["npy"].append({"name": "f32_2x3x4_v2", "descr": arr.dtype.str, "shape": list(arr.shape)})

    # fortran-order negative case (our reader must return a clean status)
    fa = np.asfortranarray((rng(20).random((4, 3)) * 2 - 1).astype("<f4"))
    np.save(os.path.join(REF, "f32_fortran.npy"), fa)
    manifest["npy"].append({"name": "f32_fortran", "descr": fa.dtype.str, "shape": list(fa.shape),
                            "fortran_order": True})

    npz = dict(make_npz_entries())
    np.savez(os.path.join(REF, "ref_stored.npz"), **npz)
    np.savez_compressed(os.path.join(REF, "ref_deflate.npz"), **npz)
    for key, arr in npz.items():
        with open(os.path.join(REF, "npz_%s.bin" % key), "wb") as f:
            f.write(arr.tobytes(order="C"))
        manifest["npz"].append({"name": key, "descr": arr.dtype.str, "shape": list(arr.shape)})
    for fn in ("ref_stored.npz", "ref_deflate.npz"):
        z = zipfile.ZipFile(os.path.join(REF, fn))
        methods = sorted(set(i.compress_type for i in z.infolist()))
        print("%s members=%d methods=%s" % (fn, len(z.infolist()), methods))

    import torch
    from safetensors.torch import save_file

    ent = make_st_entries()
    save_file(dict(ent), os.path.join(REF, "ref.safetensors"), metadata=ST_META)
    for name, t in ent:
        with open(os.path.join(REF, "st_%s.bin" % name), "wb") as f:
            f.write(st_raw_bytes(t))
        manifest["safetensors"].append({"name": name, "dtype": str(t.dtype).replace("torch.", ""),
                                        "shape": list(t.shape)})
    # spec re-derivation: parse the file with plain byte code and check offsets
    raw = open(os.path.join(REF, "ref.safetensors"), "rb").read()
    hlen = int.from_bytes(raw[:8], "little")
    hdr = json.loads(raw[8:8 + hlen].decode("utf-8"))
    buf = raw[8 + hlen:]
    for name, t in ent:
        b, e = hdr[name]["data_offsets"]
        assert buf[b:e] == st_raw_bytes(t), "safetensors offset slice mismatch for %s" % name
    print("ref.safetensors header_len=%d tensors=%d (offset slices verified)" % (hlen, len(ent)))

    with open(os.path.join(REF, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    print("corpus written to %s" % REF)


# ---------------------------------------------------------------------------
def check():
    import torch
    from safetensors.torch import load_file

    fails = []

    def board(tag, ok):
        print("%-44s %s" % (tag, "PASS" if ok else "FAIL"))
        if not ok:
            fails.append(tag)

    # 1. npy files the C++ side wrote from the ref corpus
    names = [n for n, _ in make_npy_entries()] + ["f32_2x3x4_v2"]
    for name in names:
        out_path = os.path.join(OUT, name + ".npy")
        if not os.path.exists(out_path):
            board("npy out/%s.npy (missing)" % name, False)
            continue
        ref = np.load(os.path.join(REF, name + ".npy"))
        got = np.load(out_path)
        ok = (got.dtype == ref.dtype and got.shape == ref.shape
              and got.tobytes(order="C") == ref.tobytes(order="C"))
        board("npy roundtrip %s" % name, ok)

    # 2. the npz the C++ side wrote (STORED members)
    out_npz = os.path.join(OUT, "roundtrip_stored.npz")
    if os.path.exists(out_npz):
        z = np.load(out_npz)
        for key, ref_arr in make_npz_entries():
            got = z[key]
            ok = (got.dtype == ref_arr.dtype and got.shape == ref_arr.shape
                  and got.tobytes(order="C") == np.asarray(ref_arr).tobytes(order="C"))
            board("npz roundtrip member %s" % key, ok)
        zf = zipfile.ZipFile(out_npz)
        board("npz members are STORED", all(i.compress_type == 0 for i in zf.infolist()))
    else:
        board("npz out/roundtrip_stored.npz (missing)", False)

    # 2b. the npz the C++ side re-wrote from the DEFLATED reference (inflate gate)
    out_npz2 = os.path.join(OUT, "roundtrip_deflate.npz")
    if os.path.exists(out_npz2):
        z = np.load(out_npz2)
        for key, ref_arr in make_npz_entries():
            got = z[key]
            ok = (got.dtype == ref_arr.dtype and got.shape == ref_arr.shape
                  and got.tobytes(order="C") == np.asarray(ref_arr).tobytes(order="C"))
            board("npz(deflate->stored) member %s" % key, ok)
    else:
        board("npz out/roundtrip_deflate.npz (missing)", False)

    # 3. the safetensors file the C++ side wrote
    out_st = os.path.join(OUT, "roundtrip.safetensors")
    if os.path.exists(out_st):
        ref_t = dict(make_st_entries())
        got_t = load_file(out_st)
        for name, ref in ref_t.items():
            if name not in got_t:
                board("safetensors member %s (missing)" % name, False)
                continue
            got = got_t[name]
            ok = (got.dtype == ref.dtype and got.shape == ref.shape
                  and st_raw_bytes(got) == st_raw_bytes(ref))
            board("safetensors roundtrip %s" % name, ok)
        # metadata round-trip
        from safetensors import safe_open
        with safe_open(out_st, framework="pt") as f:
            meta = f.metadata() or {}
        board("safetensors metadata roundtrip", all(meta.get(k) == v for k, v in ST_META.items()))
    else:
        board("safetensors out/roundtrip.safetensors (missing)", False)

    # 4. C++-generated philox_fill tensor written as npy — structural validity
    pf = os.path.join(OUT, "philox_fill_f64.npy")
    if os.path.exists(pf):
        a = np.load(pf)
        board("philox_fill npy loads (f64, finite, [0,1))",
              a.dtype == np.float64 and np.isfinite(a).all() and (a >= 0).all() and (a < 1).all())
    else:
        board("npy out/philox_fill_f64.npy (missing)", False)

    print("----")
    if fails:
        print("CHECK FAILED (%d):" % len(fails))
        for f in fails:
            print("  " + f)
        return 1
    print("ALL CHECKS PASSED")
    return 0


# ---------------------------------------------------------------------------
def bench():
    import torch
    from safetensors.torch import save_file, load_file

    n = 128 * 1024 * 1024  # 128 Mi f32 elements = 512 MB payload
    arr = (rng(99).random(n) * 2 - 1).astype("<f4")
    tmp = "/tmp/crd_v14l_bench"
    os.makedirs(tmp, exist_ok=True)
    npy = os.path.join(tmp, "big.npy")
    st = os.path.join(tmp, "big.safetensors")
    gb = arr.nbytes / 1e9

    def timed(fn, reps=5):
        best = 1e30
        for _ in range(reps):
            t0 = time.perf_counter()
            fn()
            best = min(best, time.perf_counter() - t0)
        return best

    tw = timed(lambda: np.save(npy, arr))
    tr = timed(lambda: np.load(npy))
    print("numpy   npy  write %.3fs (%.2f GB/s)  read %.3fs (%.2f GB/s)" % (tw, gb / tw, tr, gb / tr))
    t = torch.from_numpy(arr)
    tw = timed(lambda: save_file({"w": t}, st))
    # load_file mmaps lazily — clone() forces full materialization (the honest
    # equivalent of "file -> owned tensor", matching numpy and our reader)
    tr = timed(lambda: load_file(st)["w"].clone())
    print("safetensors write %.3fs (%.2f GB/s)  read %.3fs (%.2f GB/s)" % (tw, gb / tw, tr, gb / tr))
    print("payload bytes: %d" % arr.nbytes)


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "gen"
    if mode == "gen":
        gen()
    elif mode == "check":
        sys.exit(check())
    elif mode == "bench":
        bench()
    else:
        print("usage: v14l_io_oracle.py [gen|check|bench]")
        sys.exit(2)
