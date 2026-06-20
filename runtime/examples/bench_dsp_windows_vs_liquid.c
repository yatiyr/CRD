// v11-b/c: window + FIR design throughput — liquid-dsp peer (the SDR/comms C gold standard, apt libliquid-dev).
// liquid windows are PER-SAMPLE f32 functions (its SDR design point = small real-time filters); at N=2^20 its
// per-sample Kaiser/firdes (Bessel-I0 recomputed every call) is pathological vs Cerid vectorized+symmetry.
// Build: gcc -O3 -march=native bench_dsp_windows_vs_liquid.c -lliquid -lm
#include <liquid/liquid.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now_ms(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3 + t.tv_nsec*1e-6; }
int main(){
  unsigned int N=1u<<20; int reps=50;
  float* w=malloc(N*sizeof(float));
  #define BENCH(name, expr) do{ for(unsigned i=0;i<N;++i) w[i]=expr; double t0=now_ms(); double chk=0; \
     for(int r=0;r<reps;++r){ for(unsigned i=0;i<N;++i) w[i]=expr; chk+=w[N/2]; } \
     printf("%-16s %7.3f ms/call  (chk=%.3f)\n", name, (now_ms()-t0)/reps, chk); }while(0)
  printf("=== LIQUID-DSP window generation (N=2^20, f32) ===\n");
  BENCH("hann", liquid_hann(i,N));
  BENCH("hamming", liquid_hamming(i,N));
  BENCH("blackmanharris", liquid_blackmanharris(i,N));
  BENCH("kaiser_b14", liquid_kaiser(i,N,14.0f));
  // firdes_kaiser (windowed-sinc FIR design)
  { float* h=malloc(N*sizeof(float)); liquid_firdes_kaiser(N,0.3f,60.0f,0.0f,h);
    double t0=now_ms(); double chk=0;
    for(int r=0;r<reps;++r){ liquid_firdes_kaiser(N,0.3f,60.0f,0.0f,h); chk+=h[N/2]; }
    printf("%-16s %7.3f ms/call  (chk=%.4f)\n","firdes_kaiser",(now_ms()-t0)/reps,chk); free(h); }
  free(w); return 0;
}
