/* v11-k: liquid-dsp rresamp (rational resampler) baseline. NOTE: liquid rresamp_rrrf is f32 (Cerid is f64) — an
 * accuracy-vs-speed asterisk, flagged honestly. rresamp_rrrf_execute processes Q inputs -> P outputs per call. */
#include <liquid/liquid.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void run(unsigned int n, unsigned int P, unsigned int Q, int reps)
{
    float* x = malloc(n * sizeof(float));
    for (unsigned int i = 0; i < n; ++i)
        x[i] = sinf(2 * 3.14159265f * 0.05f * (float)i) + 0.5f * sinf(2 * 3.14159265f * 0.13f * (float)i);
    unsigned int nblocks = n / Q;
    float* y = malloc((nblocks * P + P) * sizeof(float));
    rresamp_rrrf q = rresamp_rrrf_create_default(P, Q);
    /* warm */
    for (unsigned int b = 0; b < nblocks; ++b)
        rresamp_rrrf_execute(q, x + b * Q, y + b * P);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double chk = 0;
    for (int r = 0; r < reps; ++r)
    {
        for (unsigned int b = 0; b < nblocks; ++b)
            rresamp_rrrf_execute(q, x + b * Q, y + b * P);
        chk += y[(nblocks * P) / 2];
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / reps;
    printf("liquid rresamp(f32) N=%u up=%u down=%u  %.4f ms/call (out=%u chk=%.5f)\n", n, P, Q, ms, nblocks * P, chk);
    rresamp_rrrf_destroy(q);
    free(x);
    free(y);
}

int main(void)
{
    run(1000000, 3, 2, 30);
    run(1000000, 2, 3, 30);
    return 0;
}
