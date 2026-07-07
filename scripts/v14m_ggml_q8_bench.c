// Same-format peer row: ggml (llama.cpp) Q8_0 x Q8_0 matmul on the v14-m MLP
// linear shapes at batch 4096, 1 thread. ggml quantizes the f32 activations to
// q8_0 internally for q8_0 weights (vec_dot_type) — the same scheme as ours.
#include "ggml.h"
#include "ggml-cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void fill_rand(float* p, int n, unsigned seed)
{
    srand(seed);
    for (int i = 0; i < n; ++i)
    {
        p[i] = 2.0f * ((float)rand() / (float)RAND_MAX) - 1.0f;
    }
}

// one linear layer: W[out,in] q8_0, X[batch,in] f32 -> Y[batch,out] f32
static double bench_linear(int in, int out, int batch)
{
    const size_t mem = 512u * 1024u * 1024u;
    struct ggml_init_params ip = {mem, NULL, 0};
    struct ggml_context* ctx = ggml_init(ip);

    struct ggml_tensor* wf = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, in, out);
    fill_rand((float*)wf->data, in * out, 7);
    struct ggml_tensor* wq = ggml_new_tensor_2d(ctx, GGML_TYPE_Q8_0, in, out);
    // quantize weights to q8_0
    ggml_quantize_chunk(GGML_TYPE_Q8_0, (const float*)wf->data, wq->data, 0, out, in, NULL);

    struct ggml_tensor* x = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, in, batch);
    fill_rand((float*)x->data, in * batch, 11);

    struct ggml_tensor* y = ggml_mul_mat(ctx, wq, x);
    struct ggml_cgraph* gf = ggml_new_graph(ctx);
    ggml_build_forward_expand(gf, y);

    // warmup + best-of-50, 1 thread
    for (int i = 0; i < 10; ++i)
    {
        ggml_graph_compute_with_ctx(ctx, gf, 1);
    }
    double best = 1e300;
    for (int r = 0; r < 50; ++r)
    {
        const double t0 = now_ns();
        ggml_graph_compute_with_ctx(ctx, gf, 1);
        const double t1 = now_ns();
        if (t1 - t0 < best)
        {
            best = t1 - t0;
        }
    }
    ggml_free(ctx);
    return best;
}

int main(void)
{
    const int batch = 4096;
    const struct
    {
        int in, out;
    } layers[3] = {{64, 128}, {128, 32}, {32, 10}};
    double total = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const double ns = bench_linear(layers[i].in, layers[i].out, batch);
        total += ns;
        printf("ggml q8_0 linear %3dx%3d batch %d : %10.0f ns/batch  %7.1f ns/row\n", layers[i].in,
               layers[i].out, batch, ns, ns / batch);
    }
    printf("ggml q8_0 mlp-linears total: %10.0f ns/batch  %7.1f ns/row\n", total, total / batch);
    return 0;
}
