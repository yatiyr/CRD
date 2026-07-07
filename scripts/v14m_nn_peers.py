#!/usr/bin/env python3
# v14-m peer bench: torch-CPU + onnxruntime-CPU on the frozen tiny models
# (weights = the frozen corpus safetensors), tiny batch + scaled 4096.
# Matched threading: torch.set_num_threads(1), ort intra_op_num_threads=1.
# Run pinned: taskset -c 4 python3 scripts/v14m_nn_peers.py
# The corpus .onnx twins are batch-1 (frozen export); this script exports
# DYNAMIC-batch twins from the SAME frozen weights to /tmp for the ort rows
# (corpus untouched) and cross-checks ort vs torch before timing.
import time

import numpy as np
import onnxruntime as ort
import torch
import torch.nn as nn
from safetensors.torch import load_file

CORPUS = "/mnt/d/Dev/cerid/tests/hesap-tensor/nn_corpus"
torch.set_num_threads(1)


class TinyMlp(nn.Module):
    def __init__(self):
        super().__init__()
        self.fc1 = nn.Linear(64, 128)
        self.fc2 = nn.Linear(128, 32)
        self.fc3 = nn.Linear(32, 10)
        self.ln = nn.LayerNorm(32)

    def forward(self, x):
        x = torch.relu(self.fc1(x))
        x = torch.relu(self.fc2(x))
        x = self.ln(x)
        x = self.fc3(x)
        return torch.softmax(x, dim=-1)


class TinyCnn(nn.Module):
    def __init__(self):
        super().__init__()
        self.c1 = nn.Conv2d(1, 8, 3, padding=1)
        self.c2 = nn.Conv2d(8, 16, 3, padding=1)
        self.fc = nn.Linear(16 * 4 * 4, 10)

    def forward(self, x):
        x = torch.relu(self.c1(x))
        x = torch.max_pool2d(x, 2)
        x = torch.relu(self.c2(x))
        x = torch.max_pool2d(x, 2)
        x = x.flatten(1)
        x = self.fc(x)
        return torch.softmax(x, dim=-1)


def philox_like_input(shape, seed=11):
    # timing-only input (matches bench_nn.cpp's distribution class; values
    # need not be identical across harnesses — the work is shape-driven)
    rng = np.random.default_rng(seed)
    return (2.0 * rng.random(shape) - 1.0).astype(np.float32)


def best_of_ns(fn, reps=50, warm=10):
    for _ in range(warm):
        fn()
    best = float("inf")
    for _ in range(reps):
        t0 = time.perf_counter_ns()
        fn()
        t1 = time.perf_counter_ns()
        best = min(best, t1 - t0)
    return best


def bench_model(name, model_cls, tiny_shape, big_shape):
    model = model_cls()
    model.load_state_dict(load_file(f"{CORPUS}/{name}.safetensors"))
    model.eval()

    # frozen-output sanity: torch here == the frozen _y.npy
    x_frozen = torch.from_numpy(np.load(f"{CORPUS}/{name}_x.npy"))
    y_frozen = np.load(f"{CORPUS}/{name}_y.npy")
    with torch.no_grad():
        y_here = model(x_frozen).numpy()
    print(f"[{name}] torch vs frozen _y.npy max abs diff: {np.abs(y_here - y_frozen).max():.3e}")

    # dynamic-batch onnx twin (frozen weights; corpus onnx is batch-1)
    dyn_path = f"/tmp/{name}_dyn.onnx"
    torch.onnx.export(
        model,
        x_frozen[:1],
        dyn_path,
        opset_version=17,
        input_names=["input"],
        output_names=["out"],
        dynamic_axes={"input": {0: "batch"}, "out": {0: "batch"}},
    )
    so = ort.SessionOptions()
    so.intra_op_num_threads = 1
    so.inter_op_num_threads = 1
    sess = ort.InferenceSession(dyn_path, so, providers=["CPUExecutionProvider"])

    # ort sanity vs torch on the frozen input
    y_ort = sess.run(None, {"input": x_frozen.numpy()})[0]
    print(f"[{name}] ort vs torch max abs diff: {np.abs(y_ort - y_here).max():.3e}")

    # quantized peers (the fair opponents of our Q8_0 path): torch dynamic
    # int8 (fbgemm/qnnpack Linear) + ort dynamic-quantized int8
    qmodel = None
    try:
        qmodel = torch.ao.quantization.quantize_dynamic(model, {nn.Linear}, dtype=torch.qint8)
    except Exception as e:  # noqa: BLE001
        print(f"[{name}] torch dynamic quant unavailable: {e}")
    qsess = None
    try:
        from onnxruntime.quantization import QuantType, quantize_dynamic

        # the dynamo exporter's graph trips ort's shape inference during
        # quantization; export a legacy (torchscript) twin for the int8 row
        legacy_path = f"/tmp/{name}_legacy.onnx"
        torch.onnx.export(
            model,
            x_frozen[:1],
            legacy_path,
            opset_version=17,
            input_names=["input"],
            output_names=["out"],
            dynamic_axes={"input": {0: "batch"}, "out": {0: "batch"}},
            dynamo=False,
        )
        qpath = f"/tmp/{name}_dyn_int8.onnx"
        quantize_dynamic(legacy_path, qpath, weight_type=QuantType.QInt8)
        qsess = ort.InferenceSession(qpath, so, providers=["CPUExecutionProvider"])
        y_ortq = qsess.run(None, {"input": x_frozen.numpy()})[0]
        print(f"[{name}] ort-int8 vs torch max abs diff: {np.abs(y_ortq - y_here).max():.3e}")
    except Exception as e:  # noqa: BLE001
        print(f"[{name}] ort dynamic quant unavailable: {e}")

    for shape in (tiny_shape, big_shape):
        x = philox_like_input(shape)
        xt = torch.from_numpy(x)
        with torch.no_grad():
            ns_t = best_of_ns(lambda: model(xt))
        ns_o = best_of_ns(lambda: sess.run(None, {"input": x}))
        b = shape[0]
        print(f"{name} torch     batch {b:5d} : {ns_t:12.0f} ns/batch  {ns_t / b:9.1f} ns/sample")
        print(f"{name} ort       batch {b:5d} : {ns_o:12.0f} ns/batch  {ns_o / b:9.1f} ns/sample")
        if qmodel is not None:
            with torch.no_grad():
                ns_tq = best_of_ns(lambda: qmodel(xt))
            print(f"{name} torch-int8 batch {b:5d} : {ns_tq:12.0f} ns/batch  {ns_tq / b:9.1f} ns/sample")
        if qsess is not None:
            ns_oq = best_of_ns(lambda: qsess.run(None, {"input": x}))
            print(f"{name} ort-int8  batch {b:5d} : {ns_oq:12.0f} ns/batch  {ns_oq / b:9.1f} ns/sample")


def main():
    print(f"torch {torch.__version__}  onnxruntime {ort.__version__}  (1 thread each)")
    bench_model("mlp", TinyMlp, (16, 64), (4096, 64))
    bench_model("cnn", TinyCnn, (8, 1, 16, 16), (4096, 1, 16, 16))


if __name__ == "__main__":
    main()
