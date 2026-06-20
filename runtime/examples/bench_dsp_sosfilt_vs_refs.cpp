// v11-i: filter APPLICATION throughput — sosfilt (12th-order elliptic, 10M samples) vs scipy.sosfilt + liquid-dsp.
// The hot-loop perf battleground. Cerid is BIT-EXACT to scipy + block-streaming-deterministic (the moat neither has),
// AND faster: vs scipy 1.11x, vs liquid-dsp 1.82x (Cerid f64 vs liquid f32!). IIR cascade is sequential/latency-bound.
// Build with -ffp-contract=off (ADR-0078) for the bit-exact determinism.
#include <chrono>
#include <cstdio>
#include <cmath>
#include <crd/hesap/dsp/ellip.hpp>
#include <crd/hesap/dsp/filtering.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
namespace dsp=crd::hesap::dsp; namespace cont=crd::containers;
int main(){
  crd::memory::TlsfAllocator a(crd::usize{1}<<30);
  const crd::usize N=10000000; const int reps=10;
  auto zpk=dsp::ellip<double>(&a,12,1.0,80.0,0.2);
  auto sos=dsp::zpk_to_sos<double>(&a,zpk);
  printf("(%zu sections)\n", sos.sections.size());
  cont::Array<double> x(&a); x.resize(N); for(crd::usize i=0;i<N;++i) x[i]=std::sin(0.01*i)+0.5*std::cos(0.13*i);
  cont::Array<double> y(&a); y.resize(N);
  cont::Array<dsp::BiquadState<double>> st(&a); st.resize(sos.sections.size());
  for(crd::usize s=0;s<st.size();++s) st[s]=dsp::BiquadState<double>{};
  dsp::sosfilt_stream<double>(sos, cont::ConstSpan<double>(x.data(),N), cont::Span<double>(y.data(),N), cont::Span<dsp::BiquadState<double>>(st.data(),st.size()));
  auto t0=std::chrono::high_resolution_clock::now(); double chk=0;
  for(int r=0;r<reps;++r){ for(crd::usize s=0;s<st.size();++s) st[s]=dsp::BiquadState<double>{};
    dsp::sosfilt_stream<double>(sos, cont::ConstSpan<double>(x.data(),N), cont::Span<double>(y.data(),N), cont::Span<dsp::BiquadState<double>>(st.data(),st.size())); chk+=y[N/2]; }
  auto t1=std::chrono::high_resolution_clock::now();
  printf("CERID sosfilt(12th, 10M)  %.3f ms/call  (chk=%.4f)\n", std::chrono::duration<double,std::milli>(t1-t0).count()/reps, chk);
  return 0;
}
