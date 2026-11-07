// The two peers of hesap-net's RaptorQ code (CRD_BUILD_RAPTORQ_VS_PEERS): nanorq through raptorq_nanorq_peer.c, the
// raptorq crate through raptorq_crate_peer/src/lib.rs, both behind plain C so the oracle and the board see one shape.

#ifndef CRD_TESTS_RAPTORQ_PEERS_H
#define CRD_TESTS_RAPTORQ_PEERS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // nanorq. out = K, K', S, H, W, L, P, P1, U, B, J.
    void crd_nanorq_parameters(uint32_t k, uint32_t out[11]);

    // The intermediate symbols of encoding symbol `esi`, at most 32, and how many.
    uint32_t crd_nanorq_mix(uint32_t k, uint32_t esi, uint32_t* columns);

    // A plan for one set of rows: `count` rows of a block of k symbols of t octets are replaced, row `rows[i]` by
    // encoding symbol esis[i]; count 0 is the encoder. Null if nanorq refuses it, or finds the matrix singular.
    struct crd_nanorq_plan;
    struct crd_nanorq_plan* crd_nanorq_plan_new(uint32_t k, uint32_t t, const uint32_t* rows, const uint32_t* esis,
                                                uint32_t count);
    void crd_nanorq_plan_free(struct crd_nanorq_plan* plan);

    // The intermediate symbols from k row symbols, each row's source symbol or its replacement.
    void crd_nanorq_run(struct crd_nanorq_plan* plan, const uint8_t* const* rows);

    // Encoding symbol `esi` of the last run, t octets.
    void crd_nanorq_symbol(struct crd_nanorq_plan* plan, uint32_t esi, uint8_t* out);

    // The raptorq crate (raptorq_crate_peer/src/lib.rs says what each does).
    void* crd_raptorq_crate_plan_new(uint32_t k);
    void crd_raptorq_crate_plan_free(void* plan);
    void crd_raptorq_crate_repair(const void* plan, const uint8_t* source, uint32_t k, uint32_t t, uint32_t first,
                                  uint32_t count, uint8_t* out);
    bool crd_raptorq_crate_decode(uint32_t k, uint32_t t, const uint32_t* esis, const uint8_t* const* symbols,
                                  uint32_t count, uint8_t* out);

#ifdef __cplusplus
}
#endif

#endif
