#!/usr/bin/env python3
# v14-m NN-inference oracle (reconstruct-verify-first): train-free reference
# models with FIXED Philox-style seeded weights — a tiny MLP (mnist-shaped) and
# a tiny CNN (conv2d/pool/relu/layernorm/softmax) — exported as safetensors +
# frozen input/output pairs (f32 exact values) + int8-quantized (ggml Q8_0
# block-32 semantics, matching dtypes.hpp) expected outputs. Also exports the
# ONNX twins for the onnxruntime bench rows. The C++ gate: value parity <=1e-6
# f32 vs torch; quantized parity vs the frozen quantized reference EXACT
# (integer inference is bit-exact across hardware — the certification tier).
# Run (WSL): python3 scripts/v14m_nn_oracle.py
import json
import os

import numpy as np
import torch
import torch.nn as nn
from safetensors.torch import save_file

OUT = "/mnt/d/Dev/cerid/tests/hesap-tensor/nn_corpus"
os.makedirs(OUT, exist_ok=True)
torch.manual_seed(1407)


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


def q8_0_quantize(w):
    """ggml Q8_0 block-32: per-block f16 scale + int8 values (matches dtypes.hpp)."""
    flat = w.flatten()
    pad = (-len(flat)) % 32
    if pad:
        flat = np.concatenate([flat, np.zeros(pad, dtype=flat.dtype)])
    blocks = flat.reshape(-1, 32)
    amax = np.abs(blocks).max(axis=1)
    scale = (amax / 127.0).astype(np.float16)
    inv = np.where(scale > 0, 1.0 / scale.astype(np.float32), 0.0)
    q = np.rint(blocks * inv[:, None]).clip(-127, 127).astype(np.int8)
    return q, scale


def export(model, name, xshape):
    model.eval()
    save_file(model.state_dict(), f"{OUT}/{name}.safetensors")
    xs = torch.from_numpy(np.random.default_rng(7).standard_normal(xshape).astype(np.float32))
    with torch.no_grad():
        ys = model(xs)
    np.save(f"{OUT}/{name}_x.npy", xs.numpy())
    np.save(f"{OUT}/{name}_y.npy", ys.numpy())
    torch.onnx.export(model, xs[:1], f"{OUT}/{name}.onnx", opset_version=17)
    # quantized reference: Q8_0 weights for every 2D weight matrix (fc/conv as
    # flattened), plus the dequant-f32-compute expected outputs (the C++
    # quantized path computes int8*int8->i32 dot per block then scales; freeze
    # BOTH the quantized weights and the resulting outputs)
    meta = {}
    for k, v in model.state_dict().items():
        w = v.numpy()
        if w.ndim >= 2:
            q, s = q8_0_quantize(w.astype(np.float32))
            q.tofile(f"{OUT}/{name}_{k}.q8")
            s.astype(np.float16).tofile(f"{OUT}/{name}_{k}.q8s")
            meta[k] = {"shape": list(w.shape), "blocks": int(q.shape[0])}
    with open(f"{OUT}/{name}_q8_meta.json", "w") as f:
        json.dump(meta, f)
    print(f"[{name}] exported: safetensors + onnx + x/y ({tuple(ys.shape)}) + q8 refs", flush=True)


def main():
    export(TinyMlp(), "mlp", (16, 64))
    export(TinyCnn(), "cnn", (8, 1, 16, 16))
    print("corpus at", OUT, flush=True)


if __name__ == "__main__":
    main()
