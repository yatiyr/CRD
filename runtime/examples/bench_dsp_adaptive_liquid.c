/* v11-t: liquid-dsp eqlms (LMS adaptive FIR) baseline. f32 (Cerid f64) — flagged. m=32 taps, N=1M samples. */
#include <liquid/liquid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
    unsigned int n = 1000000, m = 32;
    float* x = malloc(n * sizeof(float));
    for (unsigned int i = 0; i < n; ++i)
        x[i] = sinf(0.017f * (float)i) + 0.3f * cosf(0.05f * (float)i);
    int reps = 20;
    double chk = 0;
    struct timespec t0, t1;
    /* warm */
    eqlms_rrrf eq = eqlms_rrrf_create(NULL, m);
    eqlms_rrrf_set_bw(eq, 0.5f);
    eqlms_rrrf_destroy(eq);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < reps; ++r)
    {
        eq = eqlms_rrrf_create(NULL, m);
        eqlms_rrrf_set_bw(eq, 0.5f);
        float y = 0;
        for (unsigned int i = 0; i < n; ++i)
        {
            eqlms_rrrf_push(eq, x[i]);
            eqlms_rrrf_execute(eq, &y);
            eqlms_rrrf_step(eq, x[i], y); /* desired = x[i] (identity), error = d - y */
        }
        chk += y;
        eqlms_rrrf_destroy(eq);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / reps;
    printf("liquid eqlms(f32) m=%u N=%u  %.4f ms/call (chk=%.4f)\n", m, n, ms, chk);
    free(x);
    return 0;
}
