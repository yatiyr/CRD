import numpy as np
from scipy.fft import dct, dst

np.random.seed(0)
N = 8
x = np.random.randn(N)


# DCT-II via Makhoul N-point complex FFT (candidate)
def dctII_makhoul(x):
    N = len(x)
    w = np.empty(N)
    for n in range(N // 2):
        w[n] = x[2 * n]
        w[N - 1 - n] = x[2 * n + 1]
    W = np.fft.fft(w)  # forward: sum w e^{-i 2pi k m / N}
    k = np.arange(N)
    return 2.0 * np.real(np.exp(-1j * np.pi * k / (2 * N)) * W)


# DCT-III via Makhoul N-point IFFT. DCT-III = 2N*(DCT-II)^{-1}; invert via the conj-symmetry of the forward:
# G[k]=e^{-i pi k/2N}W[k], y[k]=2Re G[k], and G[N-k]=-i conj(G[k]) => Im G[k] = -y[N-k]/2. Recover W, IFFT, unshuffle.
def dctIII_makhoul(X):
    N = len(X)
    G = np.empty(N, dtype=complex)
    G[0] = X[0] / 2.0
    for k in range(1, N):
        G[k] = X[k] / 2.0 - 1j * X[N - k] / 2.0
    k = np.arange(N)
    W = np.exp(1j * np.pi * k / (2 * N)) * G
    w = np.fft.ifft(W)  # includes 1/N
    y = np.empty(N)
    for n in range(N // 2):
        y[2 * n] = np.real(w[n])
        y[2 * n + 1] = np.real(w[N - 1 - n])
    return 2 * N * y


# DST-II via Makhoul (candidate): reverse-and-negate the odd samples
def dstII_makhoul(x):
    N = len(x)
    w = np.empty(N)
    for n in range(N // 2):
        w[n] = x[2 * n]
        w[N - 1 - n] = -x[2 * n + 1]
    W = np.fft.fft(w)
    k = np.arange(1, N + 1)
    return -2.0 * np.imag(np.exp(-1j * np.pi * k / (2 * N)) * W[k % N])


print("DCT-II  match:", np.allclose(dct(x, type=2, norm=None), dctII_makhoul(x)))
print("  scipy:", np.round(dct(x, type=2, norm=None), 4))
print("  mine :", np.round(dctII_makhoul(x), 4))
print("DCT-III match:", np.allclose(dct(x, type=3, norm=None), dctIII_makhoul(x)))
print("  scipy:", np.round(dct(x, type=3, norm=None), 4))
print("  mine :", np.round(dctIII_makhoul(x), 4))
print("DST-II  match:", np.allclose(dst(x, type=2, norm=None), dstII_makhoul(x)))
print("  scipy:", np.round(dst(x, type=2, norm=None), 4))
print("  mine :", np.round(dstII_makhoul(x), 4))


# DST-III = 2N*(DST-II)^{-1}. Forward DST-II: H[j]=e^{-i pi j/2N}W[j], y[m]=-2 Im(H[m+1]); W conj-sym =>
# H[N-j]=-i conj(H[j]) => Re(H[j]) = y[N-j-1]/2, Im(H[j]) = -y[j-1]/2. Recover W=e^{+i pi j/2N}H, IFFT, unshuffle.
def dstIII_makhoul(X):
    N = len(X)
    H = np.empty(N, dtype=complex)  # H indexed 1..N-1
    for j in range(1, N):
        H[j] = X[N - j - 1] / 2.0 - 1j * X[j - 1] / 2.0
    W = np.empty(N, dtype=complex)
    for j in range(1, N):
        W[j] = np.exp(1j * np.pi * j / (2 * N)) * H[j]
    W[0] = X[N - 1] / 2.0  # H[N]=-i W[0] (W[0] real), y[N-1]=-2 Im(H[N])=2 W[0] => W[0]=X[N-1]/2
    w = np.fft.ifft(W)
    y = np.empty(N)
    for n in range(N // 2):
        y[2 * n] = np.real(w[n])
        y[2 * n + 1] = -np.real(w[N - 1 - n])
    return 2 * N * y


print("DST-III match:", np.allclose(dst(x, type=3, norm=None), dstIII_makhoul(x)))
print("  scipy:", np.round(dst(x, type=3, norm=None), 4))
print("  mine :", np.round(dstIII_makhoul(x), 4))
print()


# direct O(N^2) definitions (the C++ GATE oracle) — must match scipy norm=None exactly
def direct_dct2(x):
    N = len(x); return np.array([2*sum(x[n]*np.cos(np.pi*(2*n+1)*k/(2*N)) for n in range(N)) for k in range(N)])
def direct_dct3(x):
    N = len(x); return np.array([x[0]+2*sum(x[n]*np.cos(np.pi*n*(2*k+1)/(2*N)) for n in range(1,N)) for k in range(N)])
def direct_dst2(x):
    N = len(x); return np.array([2*sum(x[n]*np.sin(np.pi*(2*n+1)*(k+1)/(2*N)) for n in range(N)) for k in range(N)])
def direct_dst3(x):
    N = len(x)
    return np.array([((-1)**k)*x[N-1]+2*sum(x[n]*np.sin(np.pi*(n+1)*(2*k+1)/(2*N)) for n in range(N-1)) for k in range(N)])


# DCT-II via REAL FFT (half-spectrum) — ~2x less work than the complex-FFT Makhoul. w real => rfft gives
# Wh[0..N/2]; full W[k]=Wh[k] (k<=N/2), W[k]=conj(Wh[N-k]) (k>N/2). y[k]=2 Re(e^{-i th_k} W[k]).
def dctII_rfft(x):
    N = len(x)
    w = np.empty(N)
    for n in range(N // 2):
        w[n] = x[2 * n]
        w[N - 1 - n] = x[2 * n + 1]
    Wh = np.fft.rfft(w)  # length N/2+1
    y = np.empty(N)
    for k in range(N):
        Wk = Wh[k] if k <= N // 2 else np.conj(Wh[N - k])
        th = np.pi * k / (2 * N)
        y[k] = 2.0 * np.real(np.exp(-1j * th) * Wk)
    return y


print("DCT-II rfft match:", np.allclose(dctII_rfft(x), dct(x, type=2, norm=None)))
print("direct DCT-II :", np.allclose(direct_dct2(x), dct(x, type=2, norm=None)))
print("direct DCT-III:", np.allclose(direct_dct3(x), dct(x, type=3, norm=None)))
print("direct DST-II :", np.allclose(direct_dst2(x), dst(x, type=2, norm=None)))
print("direct DST-III:", np.allclose(direct_dst3(x), dst(x, type=3, norm=None)))
