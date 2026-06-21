// liquid-dsp peer for the v11c comms benchmark (modem + eqlms + firinterp + fft/ofdm). f32. Built by
// scripts/run_bench_comms.sh. Throughput-only (correctness gated by the Cerid test suite).
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <liquid/liquid.h>

static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1e9 + t.tv_nsec; }
static float frand(void){ return 2.0f*((float)rand()/(float)RAND_MAX) - 1.0f; }

int main(void)
{
    srand(1);
    const unsigned n = 1u << 20;

    // 1) modem QAM64 modulate + demodulate
    {
        modemcf m = modemcf_create(LIQUID_MODEM_QAM64);
        unsigned *sy = malloc(sizeof(unsigned)*n);
        liquid_float_complex *x = malloc(sizeof(liquid_float_complex)*n);
        for (unsigned i=0;i<n;++i) sy[i] = rand() % 64;
        double t0=now_ns();
        for (unsigned i=0;i<n;++i) modemcf_modulate(m, sy[i], &x[i]);
        double t1=now_ns();
        unsigned chk=0, s;
        double t2=now_ns();
        for (unsigned i=0;i<n;++i){ modemcf_demodulate(m, x[i], &s); chk ^= s; }
        double t3=now_ns();
        printf("liquid modem QAM64 modulate   %.2f ns/sym\n",(t1-t0)/n);
        printf("liquid modem QAM64 demodulate %.2f ns/sym (chk=%u)\n",(t3-t2)/n,chk);
        free(sy); free(x); modemcf_destroy(m);
    }

    // 2) eqlms 15-tap per-sample filter + step
    {
        const unsigned ntaps=15, ns=1u<<20;
        eqlms_cccf eq = eqlms_cccf_create(NULL, ntaps);
        liquid_float_complex *in = malloc(sizeof(liquid_float_complex)*ns);
        for (unsigned i=0;i<ns;++i) in[i] = frand()+ _Complex_I*frand();
        volatile float sink=0; liquid_float_complex y;
        double t0=now_ns();
        for (unsigned i=0;i<ns;++i){ eqlms_cccf_push(eq,in[i]); eqlms_cccf_execute(eq,&y);
            liquid_float_complex d = (crealf(y)>0?1:-1) + _Complex_I*(cimagf(y)>0?1:-1);
            eqlms_cccf_step(eq, d, y); sink += crealf(y); }
        double t1=now_ns(); (void)sink;
        printf("liquid eqlms 15-tap           %.2f ns/sym\n",(t1-t0)/ns);
        free(in); eqlms_cccf_destroy(eq);
    }

    // 3) firinterp rrcos x4
    {
        const unsigned nsym=1u<<18, sps=4, span=10;
        firinterp_crcf fi = firinterp_crcf_create_prototype(LIQUID_FIRFILT_RRC, sps, span, 0.35f, 0.0f);
        liquid_float_complex *sym=malloc(sizeof(liquid_float_complex)*nsym);
        liquid_float_complex *out=malloc(sizeof(liquid_float_complex)*sps);
        for (unsigned i=0;i<nsym;++i) sym[i]=frand()+_Complex_I*frand();
        volatile float s=0;
        double t0=now_ns();
        for (unsigned i=0;i<nsym;++i){ firinterp_crcf_execute(fi, sym[i], out); s += crealf(out[0]); }
        double t1=now_ns(); (void)s;
        printf("liquid rrc interp x4          %.2f ns/out-sample\n",(t1-t0)/((double)nsym*sps));
        free(sym); free(out); firinterp_crcf_destroy(fi);
    }

    // 4) fft 1024 ifft+fft (the OFDM core)
    {
        const unsigned nfft=1024, nsym=4096;
        liquid_float_complex *X=malloc(sizeof(liquid_float_complex)*nfft);
        liquid_float_complex *t=malloc(sizeof(liquid_float_complex)*nfft);
        liquid_float_complex *Y=malloc(sizeof(liquid_float_complex)*nfft);
        for (unsigned k=0;k<nfft;++k) X[k]=frand()+_Complex_I*frand();
        fftplan pf = fft_create_plan(nfft, t, Y, LIQUID_FFT_FORWARD, 0);
        fftplan pi = fft_create_plan(nfft, X, t, LIQUID_FFT_BACKWARD, 0);
        volatile float sink=0;
        double t0=now_ns();
        for (unsigned s=0;s<nsym;++s){ fft_execute(pi); /* CP trivial */ fft_execute(pf); sink+=crealf(Y[0]); }
        double t1=now_ns(); (void)sink;
        printf("liquid ofdm 1024 mod+demod    %.2f us/symbol (%u symbols)\n",(t1-t0)/1e3/nsym,nsym);
        fft_destroy_plan(pf); fft_destroy_plan(pi); free(X); free(t); free(Y);
    }
    return 0;
}
