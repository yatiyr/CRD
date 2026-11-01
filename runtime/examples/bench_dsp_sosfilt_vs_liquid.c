// v11-i: the liquid-dsp peer of the sosfilt board: iirfilt_rrrf on a 12th-order cascade of second-order sections,
// 10M f32 samples (Cerid is f64). Built by CRD_BUILD_HESAP_VS_LIQUID (runtime/CMakeLists.txt), run by
// scripts/run_bench_dsp_sosfilt.sh.
#include <liquid/liquid.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
static double ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6;
}
int main(void)
{
    unsigned N = 10000000;
    int reps = 10;
    unsigned nsos = 6;
    float B[18], A[18];
    for (unsigned s = 0; s < nsos; ++s)
    {
        B[3*s] = 0.1f; B[3*s+1] = 0.2f; B[3*s+2] = 0.1f;
        A[3*s] = 1.0f; A[3*s+1] = -1.1f; A[3*s+2] = 0.5f;
    }
    iirfilt_rrrf q = iirfilt_rrrf_create_sos(B, A, nsos);
    float* x = malloc(N * sizeof(float));
    float* y = malloc(N * sizeof(float));
    for (unsigned i = 0; i < N; ++i)
    {
        x[i] = sinf(0.01f*(float)i) + 0.5f*cosf(0.13f*(float)i);
    }
    iirfilt_rrrf_execute_block(q, x, N, y);
    double t0 = ms();
    double chk = 0;
    for (int r = 0; r < reps; ++r)
    {
        iirfilt_rrrf_execute_block(q, x, N, y);
        chk += (double)y[N/2];
    }
    printf("LIQUID iirfilt(12th SOS, 10M)  %.3f ms/call  (chk=%.4f)\n", (ms() - t0) / reps, chk);
    iirfilt_rrrf_destroy(q);
    free(x); free(y);
    return 0;
}
