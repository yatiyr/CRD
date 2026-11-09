#!/usr/bin/env python3
"""scipy.special reference vectors for the v12-b Bessel/Airy/Kelvin gates (plain C arrays, no STL).

Run: python3 tests/numerics/hesap-special/gen_bessel_refs.py > tests/numerics/hesap-special/bessel_refs.inc
"""
import mpmath as mp
import numpy as np
from scipy import special as sp


def emit(name, arr):
    vals = ", ".join(f"{float(x):.17g}" for x in np.asarray(arr, dtype=float).ravel())
    print(f"inline const double ref_{name}[] = {{{vals}}};")


def grid(nus, xs):
    NU, X = [], []
    for nu in nus:
        for x in xs:
            NU.append(nu)
            X.append(x)
    return NU, X


def debye_coefficients(count):
    """Debye's polynomials U_k(p), DLMF 10.41.9, as lists of exact rationals by power of p."""
    from fractions import Fraction as F

    polys = [[F(1)]]
    for _ in range(count):
        a = polys[-1]
        new = [F(0)] * (len(a) + 3)
        for j in range(1, len(a)):
            new[j + 1] += F(1, 2) * j * a[j]
            new[j + 3] -= F(1, 2) * j * a[j]
        for j in range(len(a)):
            new[j + 1] += F(1, 8) * a[j] / (j + 1)
            new[j + 3] -= F(5, 8) * a[j] / (j + 3)
        while new and new[-1] == 0:
            new.pop()
        polys.append(new)
    return polys


def debye_recurrence(nu, x):
    """J, Y, J', Y' of order nu at x >= nu: Debye's expansion (DLMF 10.19.6, 28 terms) at the orders 3000 below x and
    the recurrence in order up to nu, all in mpmath at 60 digits."""
    polys = [[mp.mpf(c.numerator) / c.denominator for c in u] for u in debye_coefficients(28)]
    nu = mp.mpf(nu)
    x = mp.mpf(x)

    def debye(order):
        beta = mp.acos(order / x)
        tan_b = mp.tan(beta)
        xi = order * (tan_b - beta) - mp.pi / 4
        q = 1 / tan_b
        even = mp.mpf(0)
        odd = mp.mpf(0)
        for k, u in enumerate(polys):
            term = sum(c * (1j * q) ** i for i, c in enumerate(u)) / order ** k
            if k % 2 == 0:
                even += term
            else:
                odd += term
        amp = mp.sqrt(2 / (mp.pi * order * tan_b))
        return (amp * (mp.cos(xi) * even - 1j * mp.sin(xi) * odd)).real, (amp * (mp.sin(xi) * even + 1j * mp.cos(xi) * odd)).real

    steps = max(0, int(mp.ceil(3000 - (x - nu))))
    start = nu - steps
    j0, y0 = debye(start)
    j1, y1 = debye(start + 1)
    for m in range(1, steps + 1):
        c = 2 * (start + m) / x
        j0, j1 = j1, c * j1 - j0
        y0, y1 = y1, c * y1 - y0
    return j0, y0, nu / x * j0 - j1, nu / x * y0 - y1


def hankel_reference(nu, x):
    """J, Y, J', Y' of order nu at x by Hankel's expansion (DLMF 10.17.3, 10.17.4) in mpmath at 300 digits, for nu^2 <= 12 x
    (no hump, so the sums converge to 1e-40); the phase is formed from the exact x and nu, and J' = (nu/x) J - J_{nu+1}."""
    def pair(order):
        order = mp.mpf(order)
        mu = 4 * order * order
        omega = x - (order / 2 + mp.mpf(1) / 4) * mp.pi
        p = mp.mpf(1)
        q = mp.mpf(0)
        ak = mp.mpf(1)
        xpow = mp.mpf(1)
        for k in range(1, 200):
            ak *= (mu - (2 * k - 1) ** 2) / (8 * k)
            xpow /= x
            term = ak * xpow
            if k % 2 == 0:
                p += term if (k // 2) % 2 == 0 else -term
            else:
                q += term if ((k - 1) // 2) % 2 == 0 else -term
            if abs(term) < mp.mpf(10) ** -60:
                break
        amp = mp.sqrt(2 / (mp.pi * x))
        return amp * (p * mp.cos(omega) - q * mp.sin(omega)), amp * (p * mp.sin(omega) + q * mp.cos(omega))

    mp.mp.dps = 300
    nu = mp.mpf(nu)
    x = mp.mpf(x)
    j0, y0 = pair(nu)
    j1, y1 = pair(nu + 1)
    out = (j0, y0, nu / x * j0 - j1, nu / x * y0 - y1)
    mp.mp.dps = 60
    return out


def extra():
    """References added after the third round of the review (the order and argument ranges of its defect fixes)."""
    mp.mp.dps = 60
    # the Hankel reference against mpmath's own J and Y where mpmath can sum them
    for nu, x in [(3.5, 100.0), (12.3, 400.0)]:
        r = hankel_reference(nu, x)
        assert abs(r[0] - mp.besselj(nu, x)) < mp.mpf(10) ** -25 and abs(r[1] - mp.bessely(nu, x)) < mp.mpf(10) ** -25

    # the smallest normal binade below 2^-1021, where 0.5*x is a subnormal: ln(x/2) from the integer count of x
    sn, sx = grid([0.0, 0.5, 1.0], [2.3e-308, 3e-308, 4.4e-308])
    emit("bessy_nu", sn); emit("bessy_x", sx)
    msx = [mp.mpf(x) for x in sx]
    emit("bessy_k", [mp.besselk(n, x) for n, x in zip(sn, msx)])
    emit("bessy_y", [mp.bessely(n, x) for n, x in zip(sn, msx)])
    emit("bessy_i", [mp.besseli(n, x) for n, x in zip(sn, msx)])
    emit("bessy_j", [mp.besselj(n, x) for n, x in zip(sn, msx)])

    # order from 2^52 and argument from 1e30 (Hankel at the order itself: the phase of nu/2 + 1/4 and the order nu + 1 are
    # exact only if handled so)
    PH = [(4503599627370497.0, 1e32), (1e16, 1e33), (9007199254740994.0, 5e33), (1e20, 1e45), (1e30, 1e63), (1e70, 1e150),
          (1e75, 1e150), (3e5 + 0.5, 1e20), (2.5e5 + 0.25, 1e17), (2500.5, 1e6), (5000.3, 3e6), (20000.7, 5e7)]
    emit("besw_nu", [t[0] for t in PH]); emit("besw_x", [t[1] for t in PH])
    ref = [hankel_reference(n, x) for n, x in PH]
    emit("besw_j", [r[0] for r in ref]); emit("besw_y", [r[1] for r in ref])
    emit("besw_jp", [r[2] for r in ref]); emit("besw_yp", [r[3] for r in ref])

    # complex J and Y at large imaginary part (the series cannot be summed in 400 terms; e^{Im z} near the doubles) and
    # at a few points where the series for Y alternates through J_{-nu} (mpmath, 60 digits)
    c5 = [(0.3, 300.0, 689.0), (5.5, 300.0, 700.0), (0.3, 0.5, 689.0), (20.2, 100.0, 705.0), (35.2, 10.0, 650.0),
          (70.5, 0.5, 600.0), (70.5, 30.0, 600.0), (70.5, 100.0, 705.0), (0.3, 30.0, 711.0), (5.5, 100.0, 712.0),
          (0.3, 30.0, -711.0), (-5.5, 100.0, 700.0)]
    emit("cbes5_nu", [t[0] for t in c5]); emit("cbes5_zr", [t[1] for t in c5]); emit("cbes5_zi", [t[2] for t in c5])
    cz5 = [mp.mpc(t[1], t[2]) for t in c5]
    for name, fn in (("cbes5j", mp.besselj), ("cbes5y", mp.bessely)):
        vals = [fn(t[0], z) for t, z in zip(c5, cz5)]
        emit(name + "_re", [v.real for v in vals]); emit(name + "_im", [v.imag for v in vals])

    # complex J at order 35.2 and Im z = 5, Re z 31 .. 44, where the ascending series loses more than the bar of the
    # module from Re z = 33.5 on (1e-9 at 36, 1e-6 at 42) and Hankel's expansion at the base order with the recurrence
    # in order serves (mpmath, 60 digits)
    c6 = [(35.2, x, 5.0) for x in (31.0, 35.0, 36.0, 38.0, 40.0, 42.0, 44.0)]
    emit("cbes6_nu", [t[0] for t in c6]); emit("cbes6_zr", [t[1] for t in c6]); emit("cbes6_zi", [t[2] for t in c6])
    vals = [mp.besselj(t[0], mp.mpc(t[1], t[2])) for t in c6]
    emit("cbes6j_re", [v.real for v in vals]); emit("cbes6j_im", [v.imag for v in vals])

    # the regressions of the review of the head: complex Y at a negative integer order with Re z < 0, complex K at a
    # half-integer order with Re z < 0 (points where the ascending series of I carries the bar of K), and I, I' at an order
    # above the argument and an argument where the core's recurrence or e^{-x} leaves the doubles (mpmath, 80 digits)
    mp.mp.dps = 80
    r1 = [(-78.0, -10.390696641490749, 1.8959149212151196), (-8.0, -11.52692684460591, 0.8946768150244537),
          (-56.0, -4.483711066427055, -51.85128938159879), (-85.0, -31.384079757808998, -1.5245179413709629),
          (-33.0, -22.907938755636927, -4.512724864552098)]
    emit("cbes7_nu", [t[0] for t in r1]); emit("cbes7_zr", [t[1] for t in r1]); emit("cbes7_zi", [t[2] for t in r1])
    vals = [mp.bessely(mp.mpf(t[0]), mp.mpc(t[1], t[2])) for t in r1]
    emit("cbes7y_re", [v.real for v in vals]); emit("cbes7y_im", [v.imag for v in vals])
    r2 = [(80.5, -33.25832562630909, -57.21479401211252), (39.5, -39.71090676908675, 47.31766921338493),
          (90.5, -48.8520240796976, -60.37838763750946), (85.5, -66.96144425708692, 65.22010212153938),
          (56.5, -20.2008047374602, -48.452060249538604)]
    emit("cbes8_nu", [t[0] for t in r2]); emit("cbes8_zr", [t[1] for t in r2]); emit("cbes8_zi", [t[2] for t in r2])
    vals = [mp.besselk(mp.mpf(t[0]), mp.mpc(t[1], t[2])) for t in r2]
    emit("cbes8k_re", [v.real for v in vals]); emit("cbes8k_im", [v.imag for v in vals])
    c1 = [(3000.0, 2000.0), (2000.0, 1000.0), (5000.0, 3000.0), (4000.5, 3000.0)]
    emit("besc1_nu", [t[0] for t in c1]); emit("besc1_x", [t[1] for t in c1])
    emit("besc1_i", [mp.besseli(mp.mpf(n), mp.mpf(x)) for n, x in c1])
    emit("besc1_ip", [(mp.besseli(mp.mpf(n) - 1, mp.mpf(x)) + mp.besseli(mp.mpf(n) + 1, mp.mpf(x))) / 2 for n, x in c1])
    # complex Y within 1e-12 of a negative integer with Re z < 0 (base's integer series; the reflection through J's series
    # is wrong by up to 1e8 times the bar there): mpmath, 80 digits, with the order as the exact double.
    r1n = [(-19.999999999999968, -11.094598497002632, 9.732943969410005), (-11.00000000000086, -11.34699299704204, 3.913714006760326),
           (-2.9999999999999063, -12.257502365516995, 5.353286859491519), (-30.000000000000078, -21.747263874899282, 12.197431740052714)]
    emit("cbes13_nu", [t[0] for t in r1n]); emit("cbes13_zr", [t[1] for t in r1n]); emit("cbes13_zi", [t[2] for t in r1n])
    vals = [mp.bessely(mp.mpf(t[0]), mp.mpc(t[1], t[2])) for t in r1n]
    emit("cbes13y_re", [v.real for v in vals]); emit("cbes13y_im", [v.imag for v in vals])


def main():
    print("// GENERATED by gen_bessel_refs.py — scipy.special Bessel/Airy/Kelvin refs (v12-b). Plain C arrays (no STL).")
    print("#pragma once")

    nus = [0.0, 1.0, 2.0, 5.0, 0.5, 1.5, 2.5, 1.0 / 3, 2.0 / 3]
    NU, X = grid(nus, [0.5, 1.0, 2.0, 5.0, 10.0, 20.0])
    emit("besj_nu", NU); emit("besj_x", X)
    emit("besj_y", [sp.jv(n, x) for n, x in zip(NU, X)])
    emit("besy_y", [sp.yv(n, x) for n, x in zip(NU, X)])
    emit("besjp_y", [sp.jvp(n, x) for n, x in zip(NU, X)])
    emit("besyp_y", [sp.yvp(n, x) for n, x in zip(NU, X)])

    NUik, Xik = grid(nus, [0.5, 1.0, 2.0, 5.0, 10.0])
    emit("besi_nu", NUik); emit("besi_x", Xik)
    emit("besi_y", [sp.iv(n, x) for n, x in zip(NUik, Xik)])
    emit("besk_y", [sp.kv(n, x) for n, x in zip(NUik, Xik)])
    emit("besip_y", [sp.ivp(n, x) for n, x in zip(NUik, Xik)])
    emit("beskp_y", [sp.kvp(n, x) for n, x in zip(NUik, Xik)])

    nneg = [-0.5, -1.5, -2.5, -1.0 / 3]
    NUn, Xn = grid(nneg, [0.5, 1.0, 2.0, 5.0, 10.0])
    emit("besjn_nu", NUn); emit("besjn_x", Xn)
    emit("besjn_y", [sp.jv(n, x) for n, x in zip(NUn, Xn)])
    emit("besyn_y", [sp.yv(n, x) for n, x in zip(NUn, Xn)])
    emit("besin_y", [sp.iv(n, x) for n, x in zip(NUn, Xn)])

    SN, SX = [], []
    for n in [0, 1, 2, 3, 5]:
        for x in [0.5, 1.0, 2.0, 5.0, 10.0]:
            SN.append(n); SX.append(x)
    emit("sphn", SN); emit("sphx", SX)
    emit("sphj_y", [sp.spherical_jn(n, x) for n, x in zip(SN, SX)])
    emit("sphy_y", [sp.spherical_yn(n, x) for n, x in zip(SN, SX)])

    ax = [-10.0, -5.0, -2.0, -1.0, 0.0, 1.0, 2.0, 5.0, 10.0]
    Ai, Aip, Bi, Bip = sp.airy(ax)
    emit("airy_x", ax); emit("airy_ai", Ai); emit("airy_aip", Aip); emit("airy_bi", Bi); emit("airy_bip", Bip)

    # complex argument
    cnus = [0.0, 1.0, 2.0, 0.5, 1.5, 2.5]
    zpts = [(1.0, 1.0), (3.0, -2.0), (5.0, 4.0), (2.0, 8.0), (0.5, 0.5), (10.0, 3.0), (20.0, -5.0)]
    CNU, ZR, ZI = [], [], []
    for nu in cnus:
        for (a, b) in zpts:
            CNU.append(nu); ZR.append(a); ZI.append(b)
    emit("cbes_nu", CNU); emit("cbes_zr", ZR); emit("cbes_zi", ZI)
    cz = [complex(a, b) for a, b in zip(ZR, ZI)]

    def cemit(name, vals):
        emit(name + "_re", [v.real for v in vals]); emit(name + "_im", [v.imag for v in vals])

    cemit("cbesj", [sp.jv(n, z) for n, z in zip(CNU, cz)])
    cemit("cbesy", [sp.yv(n, z) for n, z in zip(CNU, cz)])
    cemit("cbesi", [sp.iv(n, z) for n, z in zip(CNU, cz)])
    cemit("cbesk", [sp.kv(n, z) for n, z in zip(CNU, cz)])

    kx = [0.5, 1.0, 2.0, 3.0, 5.0, 8.0, 20.0, 30.0]
    emit("kelvin_x", kx)
    emit("kelvin_ber", [sp.ber(x) for x in kx]); emit("kelvin_bei", [sp.bei(x) for x in kx])
    emit("kelvin_ker", [sp.ker(x) for x in kx]); emit("kelvin_kei", [sp.kei(x) for x in kx])

    # derivatives at negative order: the reflection coefficients do not depend on x, so they carry over to the primes
    emit("besjpn_y", [sp.jvp(n, x) for n, x in zip(NUn, Xn)])
    emit("besypn_y", [sp.yvp(n, x) for n, x in zip(NUn, Xn)])
    emit("besipn_y", [sp.ivp(n, x) for n, x in zip(NUn, Xn)])
    emit("beskpn_y", [sp.kvp(n, x) for n, x in zip(NUn, Xn)])

    # small argument at integer and half-integer negative orders: sin(νπ) and cos(νπ) must be exact there, because they
    # multiply a Y_ν or K_ν that is huge (they are exactly 0 or ±1)
    nsm = [-1.0, -2.0, -3.0, -0.5, -1.5, -2.5, -4.5]
    NUs, Xs = grid(nsm, [1e-3, 1e-2, 0.1, 1.0])
    emit("besjs_nu", NUs); emit("besjs_x", Xs)
    emit("besjs_y", [sp.jv(n, x) for n, x in zip(NUs, Xs)])
    emit("besys_y", [sp.yv(n, x) for n, x in zip(NUs, Xs)])
    emit("besis_y", [sp.iv(n, x) for n, x in zip(NUs, Xs)])

    # J at large order and large argument (the Hankel asymptotic needs x ≫ ν²/2, not just x ≥ 2ν)
    JL = [(10.5, 21.6), (10.5, 25.9), (12.3, 25.0), (20.0, 45.0), (20.0, 60.0), (35.2, 80.0), (50.0, 110.6),
          (50.0, 174.0), (100.3, 208.7), (100.3, 300.0)]
    emit("besjl_nu", [t[0] for t in JL]); emit("besjl_x", [t[1] for t in JL])
    emit("besjl_y", [sp.jv(n, x) for n, x in JL])

    # the Maclaurin series loses ~e^ζ·ε for x < 0: these points sit between its old edge (−9) and where it is accurate,
    # and beyond x = −600 (the continued fraction of the Bessel core needs about ζ steps, ζ = (2/3)|x|^{3/2})
    ax2 = [-8.999999, -8.9, -8.7, -8.5, -8.3, -8.0, -7.7, -7.3, -7.0, -6.5, -6.0, -5.7,
           -150.0, -300.0, -600.0, -800.0, -1000.0]  # the last five: ζ ≥ 1000, Hankel's expansion (scipy loses ε·ζ beyond)
    Ai2, Aip2, Bi2, Bip2 = sp.airy(ax2)
    emit("airy2_x", ax2); emit("airy2_ai", Ai2); emit("airy2_aip", Aip2); emit("airy2_bi", Bi2); emit("airy2_bip", Bip2)

    # large argument: the CF1 of the Steed method needs about x steps (its cap is 1e4), so J, Y, J' and Y' come from the
    # Hankel expansion there; the phase x − (ν/2 + ¼)π must not be formed as a difference of large numbers
    NUb, Xb = grid([0.0, 0.5, 2.7, 30.0], [1e3, 5e3, 1.2e4, 3e4, 1e5, 1e6, 1e8])
    emit("besb_nu", NUb); emit("besb_x", Xb)
    emit("besb_j", [sp.jv(n, x) for n, x in zip(NUb, Xb)])
    emit("besb_y", [sp.yv(n, x) for n, x in zip(NUb, Xb)])
    emit("besb_jp", [sp.jvp(n, x) for n, x in zip(NUb, Xb)])
    emit("besb_yp", [sp.yvp(n, x) for n, x in zip(NUb, Xb)])

    # complex argument in the left half-plane and near the imaginary axis (asymptotic region): the expansions of J and Y
    # are taken through the reflection z → −z, and I carries a second exponential there
    c2nus = [0.0, 1.0, 2.5, -0.7]
    z2pts = [(-30.0, 4.0), (-30.0, -4.0), (-20.0, 30.0), (-3.0, 30.0), (0.5, 30.0), (5.0, 30.0), (3.0, 25.0), (-40.0, 0.5)]
    C2NU, Z2R, Z2I = [], [], []
    for nu in c2nus:
        for (a, b) in z2pts:
            C2NU.append(nu); Z2R.append(a); Z2I.append(b)
    emit("cbes2_nu", C2NU); emit("cbes2_zr", Z2R); emit("cbes2_zi", Z2I)
    cz2 = [complex(a, b) for a, b in zip(Z2R, Z2I)]
    cemit("cbes2j", [sp.jv(n, z) for n, z in zip(C2NU, cz2)])
    cemit("cbes2y", [sp.yv(n, z) for n, z in zip(C2NU, cz2)])
    cemit("cbes2i", [sp.iv(n, z) for n, z in zip(C2NU, cz2)])
    cemit("cbes2k", [sp.kv(n, z) for n, z in zip(C2NU, cz2)])

    # small argument: the CF1 recurrence loses digits as x falls and its seed then underflows, so J, J', I, I' come from
    # their ascending series below x = 1e-3 (I(0.5, 1e-20) was +inf, I' was 3e5 times over the limit at 1e-10, J'(0, 1e-35)
    # was 0)
    NUj, Xj = grid([0.0, 0.5, 1.0, 2.5, 7.3, -0.5, -2.5], [1e-4, 1e-6, 1e-10, 1e-20, 1e-35, 1e-60])
    emit("bessm_nu", NUj); emit("bessm_x", Xj)
    emit("bessm_j", [sp.jv(n, x) for n, x in zip(NUj, Xj)])
    emit("bessm_jp", [sp.jvp(n, x) for n, x in zip(NUj, Xj)])
    emit("bessm_i", [sp.iv(n, x) for n, x in zip(NUj, Xj)])
    emit("bessm_ip", [sp.ivp(n, x) for n, x in zip(NUj, Xj)])
    NUy, Xy = grid([0.0, 0.5, 1.0, 2.5, -0.5, -1.5], [1e-4, 1e-6, 1e-10, 1e-20, 1e-35, 1e-60])
    emit("bessmy_nu", NUy); emit("bessmy_x", Xy)
    emit("bessmy_y", [sp.yv(n, x) for n, x in zip(NUy, Xy)])
    emit("bessmy_yp", [sp.yvp(n, x) for n, x in zip(NUy, Xy)])
    emit("bessmy_k", [sp.kv(n, x) for n, x in zip(NUy, Xy)])
    emit("bessmy_kp", [sp.kvp(n, x) for n, x in zip(NUy, Xy)])

    # Kelvin ker/kei in the crossover: the ascending series loses e^{√2 x}·ε (1e-5 at x = 18), so the asymptotic takes over
    # from x = 9
    kx2 = [9.5, 10.5, 12.0, 14.0, 16.0, 17.5, 17.9]
    emit("kelvin2_x", kx2)
    emit("kelvin2_ker", [sp.ker(x) for x in kx2]); emit("kelvin2_kei", [sp.kei(x) for x in kx2])

    # order > ~100 at x > ~9000 (mpmath, 30 digits: scipy is not the reference where the order is this large): the
    # continued fraction of the Steed method needs about x steps and Hankel's expansion at the order itself needs
    # x ≫ ν²/2; the expansion at the base order and the recurrence in order serve (ν ≤ x)
    mp.mp.dps = 30
    PH = [(100.3, 9.5e3), (100.3, 1e5), (300.0, 9.5e3), (300.0, 2e4), (300.0, 1e6), (1000.5, 2e4), (1000.5, 1e5),
          (4000.0, 1e6)]
    NUh, Xh = [t[0] for t in PH], [t[1] for t in PH]
    emit("besw_nu", NUh); emit("besw_x", Xh)
    emit("besw_j", [mp.besselj(n, x) for n, x in zip(NUh, Xh)])
    emit("besw_y", [mp.bessely(n, x) for n, x in zip(NUh, Xh)])
    emit("besw_jp", [(mp.besselj(n - 1, x) - mp.besselj(n + 1, x)) / 2 for n, x in zip(NUh, Xh)])
    emit("besw_yp", [(mp.bessely(n - 1, x) - mp.bessely(n + 1, x)) / 2 for n, x in zip(NUh, Xh)])

    # complex argument with the order near |z| (scipy): both the series and Hankel's expansion are ~1e-7 there; the
    # expansion at the base order with the recurrence in order (J, Y), and Steed's CF2 with the recurrence (K between |z| = 8
    # and the rounding of the asymptotic) serve
    c3 = [(35.2, 40.0, 10.0), (50.0, 70.0, 15.0), (50.0, 60.0, 10.0), (50.0, 60.0, 3.0), (50.0, 100.0, 10.0),
          (20.0, 30.0, 30.0), (12.3, 35.0, 5.0), (-35.2, 40.0, 10.0), (60.5, 80.0, 12.0)]
    emit("cbes3_nu", [t[0] for t in c3]); emit("cbes3_zr", [t[1] for t in c3]); emit("cbes3_zi", [t[2] for t in c3])
    cz3 = [complex(t[1], t[2]) for t in c3]
    cemit("cbes3j", [sp.jv(t[0], z) for t, z in zip(c3, cz3)])
    cemit("cbes3y", [sp.yv(t[0], z) for t, z in zip(c3, cz3)])
    k3 = [(5.0, 9.0, 0.5), (5.0, 9.0, 3.0), (0.0, 9.0, 0.5), (12.3, 10.0, 0.0), (5.5, 8.5, 8.0), (5.0, 0.5, 12.0),
          (2.0, 9.5, 1.0), (-7.2, 11.0, 2.0)]
    emit("cbes3k_nu", [t[0] for t in k3]); emit("cbes3k_zr", [t[1] for t in k3]); emit("cbes3k_zi", [t[2] for t in k3])
    kz3 = [complex(t[1], t[2]) for t in k3]
    cemit("cbes3k", [sp.kv(t[0], z) for t, z in zip(k3, kz3)])

    # J' and I' at x = 1e-50, order 7.3: the result is a subnormal (about 1000 times 4.9e-324), so it is compared
    # to within one subnormal step (mpmath, rounded to the nearest double)
    mp.mp.dps = 60
    dn = [(7.3, 1e-50), (7.3, 1.5e-50), (7.3, 2e-50)]
    emit("bessd_nu", [t[0] for t in dn]); emit("bessd_x", [t[1] for t in dn])
    emit("bessd_jp", [(mp.besselj(mp.mpf(n) - 1, mp.mpf(x)) - mp.besselj(mp.mpf(n) + 1, mp.mpf(x))) / 2 for n, x in dn])
    emit("bessd_ip", [(mp.besseli(mp.mpf(n) - 1, mp.mpf(x)) + mp.besseli(mp.mpf(n) + 1, mp.mpf(x))) / 2 for n, x in dn])

    # K, Y, I, J and their slopes at the smallest subnormal arguments (mpmath, 60 digits; a value beyond the doubles is
    # written as the largest double and expected to be an infinity): 0.5 x rounds to 0 for x = 4.94e-324, and the series
    # and Temme's head took the logarithm of it
    mp.mp.dps = 60
    dmax = 1.7976931348623157e308

    def emit_tiny(name, arr):
        vals = ", ".join(f"{(dmax if v > dmax else -dmax if v < -dmax else float(v)):.17g}" for v in arr)
        print(f"inline const double ref_{name}[] = {{{vals}}};")

    def bi(n, x):  # mpmath has no I of a negative integer order: I_{-n} = I_n
        return mp.besseli(abs(n) if n == int(n) else n, x)

    tn, tx = grid([0.0, 0.5, 1.0], [4.9406564584124654e-324, 1e-323, 1e-320, 1e-309])
    mx = [mp.mpf(x) for x in tx]
    emit("bessx_nu", tn); emit("bessx_x", tx)
    emit_tiny("bessx_j", [mp.besselj(n, x) for n, x in zip(tn, mx)])
    emit_tiny("bessx_y", [mp.bessely(n, x) for n, x in zip(tn, mx)])
    emit_tiny("bessx_i", [mp.besseli(n, x) for n, x in zip(tn, mx)])
    emit_tiny("bessx_k", [mp.besselk(n, x) for n, x in zip(tn, mx)])
    emit_tiny("bessx_jp", [(mp.besselj(n - 1, x) - mp.besselj(n + 1, x)) / 2 for n, x in zip(tn, mx)])
    emit_tiny("bessx_yp", [(mp.bessely(n - 1, x) - mp.bessely(n + 1, x)) / 2 for n, x in zip(tn, mx)])
    emit_tiny("bessx_ip", [(bi(n - 1, x) + bi(n + 1, x)) / 2 for n, x in zip(tn, mx)])
    emit_tiny("bessx_kp", [-(mp.besselk(n - 1, x) + mp.besselk(n + 1, x)) / 2 for n, x in zip(tn, mx)])

    # order above 2e5 at x >= order (the recurrence from the base order is capped at 2e5 steps): Debye's expansion
    # (DLMF 10.19.6, 28 terms, 60 digits; it agrees with scipy.jv to scipy's own phase error, 1e-11, and has no hump) at
    # the orders a margin below x and the recurrence in order in mpmath up to the order
    PL = [(200000.3, 200000.8), (200000.3, 201000.3), (200000.3, 203000.3), (250000.7, 250100.7), (1000000.5, 1000000.5),
          (1000000.5, 1002400.5), (1000000.5, 1500000.75), (10000000.25, 10001000.25), (50000000.5, 100000000.0),
          (250000.5, 1e9), (300000.5, 1e10), (1000000.5, 1e12), (200000.1, 1e14), (200000.1, 3e15), (4000000.25, 1e17)]
    NUl, Xl = [t[0] for t in PL], [t[1] for t in PL]
    emit("besl2_nu", NUl); emit("besl2_x", Xl)
    ref = [debye_recurrence(n, x) for n, x in PL]
    emit("besl2_j", [r[0] for r in ref]); emit("besl2_y", [r[1] for r in ref])
    emit("besl2_jp", [r[2] for r in ref]); emit("besl2_yp", [r[3] for r in ref])

    # complex J and Y with the order >= 3/4 Re z and Im z >= 15, where the recurrence from the base order amplifies its
    # start by e^{2(Im z - Im phi)} (mpmath, 60 digits)
    c4 = [(35.2, 45.0, 20.0), (35.2, 40.0, 30.0), (50.0, 60.0, 30.0), (50.0, 60.0, 20.0), (70.5, 60.0, 20.0),
          (50.0, 60.0, -30.0), (-50.5, 70.0, 25.0), (100.0, 130.0, 30.0), (60.3, 50.0, 28.0), (12.3, 5.0, 28.0)]
    emit("cbes4_nu", [t[0] for t in c4]); emit("cbes4_zr", [t[1] for t in c4]); emit("cbes4_zi", [t[2] for t in c4])
    cz4 = [mp.mpc(t[1], t[2]) for t in c4]
    cemit("cbes4j", [mp.besselj(t[0], z) for t, z in zip(c4, cz4)])
    cemit("cbes4y", [mp.bessely(t[0], z) for t, z in zip(c4, cz4)])
    extra()


if __name__ == "__main__":
    import sys

    if len(sys.argv) > 1 and sys.argv[1] == "--extra-only":  # the part that main() ends with, to append to the file
        extra()
    else:
        main()
