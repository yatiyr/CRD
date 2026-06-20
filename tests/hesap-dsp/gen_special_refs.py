#!/usr/bin/env python3
"""scipy savgol_coeffs reference for v11-d special FIR. Plain C arrays (no STL). RRC refs from MATLAB separately.
Run: python3 tests/hesap-dsp/gen_special_refs.py  (savgol rows; concatenated with the MATLAB rcos rows + pragma)."""
from scipy.signal import savgol_coeffs


def emit(name, arr):
    san = name.replace(".", "_").replace("-", "_")
    print(f"inline const double ref_{san}[] = {{{', '.join(f'{x:.17g}' for x in arr)}}};")


def main():
    emit("savgol_5_2", savgol_coeffs(5, 2))
    emit("savgol_11_3", savgol_coeffs(11, 3))
    emit("savgol_21_4", savgol_coeffs(21, 4))
    emit("savgol_11_2_d1", savgol_coeffs(11, 2, deriv=1))


if __name__ == "__main__":
    main()
