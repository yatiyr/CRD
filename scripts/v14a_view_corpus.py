import numpy as np

# The v14-a view-semantics corpus: element strides (numpy strides are BYTES -> divide by itemsize),
# shapes, contiguity, and flattened element readouts for a set of view compositions on
# arange(24, dtype=f64).reshape(2,3,4). Baked into tests/hesap-tensor/test_tensor_view.cpp.

a = np.arange(24, dtype=np.float64).reshape(2, 3, 4)

def report(name, v):
    es = tuple(s // v.itemsize for s in v.strides)
    print(f"{name}: shape={v.shape} estrides={es} c_contig={v.flags['C_CONTIGUOUS']}")
    print(f"  flat={list(v.reshape(-1) if v.flags['C_CONTIGUOUS'] else v.flatten())}")

report("base", a)
report("permute(2,0,1)", np.transpose(a, (2, 0, 1)))
report("slice[:,1:3,::2]", a[:, 1:3, ::2])
report("flip(dim2)", a[:, :, ::-1])
report("slice[1,:,:] (rank drop)", a[1, :, :])
report("slice[:,2,:] (mid drop)", a[:, 2, :])
b = np.arange(3, dtype=np.float64).reshape(3, 1)
report("broadcast (3,1)->(2,3,4)", np.broadcast_to(b, (2, 3, 4)))
report("reshape(6,4)", a.reshape(6, 4))
report("permute then slice", np.transpose(a, (2, 0, 1))[1:4:2, :, 1:])
