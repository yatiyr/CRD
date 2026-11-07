// nanorq's core API behind the peers' C shape (raptorq_peers.h). Its own benchmark_core.c is the model of the calls.

#include "nanorq_core.h"
#include "nanorq_ops.h"
#include "raptorq_peers.h"

#include <stdlib.h>
#include <string.h>

struct crd_nanorq_plan
{
    nanorq_core rq;
    uint8_t* prepare;
    uint8_t* work;
    uint8_t* schedule_memory;
    schedule ops;
    uint8_t* d;
    uint8_t* mixed;
    uint32_t t;
    uint32_t stride;
    size_t d_bytes;
    uint32_t k;
};

void crd_nanorq_parameters(uint32_t k, uint32_t out[11])
{
    const params p = params_init(k);
    const uint32_t values[11] = {p.K, p.Kprime, p.S, p.H, p.W, p.L, p.P, p.P1, p.U, p.B, p.J};
    memcpy(out, values, sizeof(values));
}

uint32_t crd_nanorq_mix(uint32_t k, uint32_t esi, uint32_t* columns)
{
    nanorq_core rq = {0};
    if (!nanorq_core_encoder_new(k, 0, &rq))
    {
        return 0U;
    }
    return nanorq_core_get_packet_mix(&rq, esi, columns, GENC_MAX);
}

void crd_nanorq_plan_free(struct crd_nanorq_plan* plan)
{
    if (plan != NULL)
    {
        free(plan->prepare);
        free(plan->work);
        free(plan->schedule_memory);
        free(plan->d);
        free(plan->mixed);
        free(plan);
    }
}

struct crd_nanorq_plan* crd_nanorq_plan_new(uint32_t k, uint32_t t, const uint32_t* rows, const uint32_t* esis,
                                            uint32_t count)
{
    nanorq_core_init();
    struct crd_nanorq_plan* plan = calloc(1U, sizeof(*plan));
    if (plan == NULL || !nanorq_core_encoder_new(k, 0, &plan->rq))
    {
        crd_nanorq_plan_free(plan);
        return NULL;
    }
    plan->t = t;
    plan->k = k;
    plan->stride = nanorq_core_recommended_stride(t);
    struct nanorq_core_mem_reqs reqs;
    nanorq_core_get_memory_reqs(k, 0, plan->stride, &reqs);
    plan->prepare = malloc(reqs.prepare_bytes);
    plan->work = malloc(reqs.work_bytes);
    plan->schedule_memory = malloc(reqs.schedule_bytes);
    plan->d_bytes = reqs.matrix_bytes;
    plan->d = aligned_alloc(64U, (reqs.matrix_bytes + 63U) & ~(size_t)63U);
    plan->mixed = aligned_alloc(64U, ((size_t)plan->stride + 63U) & ~(size_t)63U);
    if (plan->prepare == NULL || plan->work == NULL || plan->schedule_memory == NULL || plan->d == NULL ||
        plan->mixed == NULL || !schedule_init(&plan->ops, plan->schedule_memory, reqs.schedule_bytes) ||
        !nanorq_core_prepare(&plan->rq, plan->prepare, reqs.prepare_bytes))
    {
        crd_nanorq_plan_free(plan);
        return NULL;
    }
    nanorq_core_set_op_callback(&plan->rq, &plan->ops, ops_push);
    for (uint32_t i = 0U; i < count; ++i)
    {
        nanorq_core_replace_symbol(&plan->rq, rows[i], esis[i]);
    }
    if ((count != 0U && !nanorq_core_patch_matrix(&plan->rq)) ||
        !nanorq_core_precalculate(&plan->rq, plan->work, reqs.work_bytes) || plan->ops.overflowed)
    {
        crd_nanorq_plan_free(plan);
        return NULL;
    }
    return plan;
}

void crd_nanorq_run(struct crd_nanorq_plan* plan, const uint8_t* const* rows)
{
    memset(plan->d, 0, plan->d_bytes);
    for (uint32_t i = 0U; i < plan->k; ++i)
    {
        memcpy(nanorq_core_get_symbol_ptr(&plan->rq, plan->d, plan->stride, i), rows[i], plan->t);
    }
    ops_run(&plan->rq, plan->d, plan->stride, &plan->ops);
}

void crd_nanorq_symbol(struct crd_nanorq_plan* plan, uint32_t esi, uint8_t* out)
{
    ops_mix(&plan->rq, plan->d, plan->stride, esi, plan->mixed);
    memcpy(out, plan->mixed, plan->t);
}
