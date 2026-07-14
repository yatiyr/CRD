extern "C" __global__ void ckir(float* buf0, float* buf1) {
  __shared__ float sh2[1056];
  sh2[(((threadIdx.x / 32u) * 33u) + (threadIdx.x % 32u))] = buf0[(((((blockIdx.x / 32u) * 32u) + (threadIdx.x / 32u)) * 1024u) + (((blockIdx.x % 32u) * 32u) + (threadIdx.x % 32u)))];
  __syncthreads();
  buf1[(((((blockIdx.x % 32u) * 32u) + (threadIdx.x / 32u)) * 1024u) + (((blockIdx.x / 32u) * 32u) + (threadIdx.x % 32u)))] = sh2[(((threadIdx.x % 32u) * 33u) + (threadIdx.x / 32u))];
}
