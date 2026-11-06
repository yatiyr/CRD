// The C surface of crd-openfec-peer: OpenFEC's GF(2^8) tables, matrix routines and its real Reed-Solomon codec, for
// test_erasure_openfec.cpp. Every function reads OpenFEC's own code; nothing here computes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    uint8_t crd_openfec_mul(uint8_t a, uint8_t b);
    uint8_t crd_openfec_inv(uint8_t a);
    uint8_t crd_openfec_alpha_pow(uint32_t e);

    // of_galois_field_2_8_invert_mat: `matrix` (k x k, row-major) in place. 0 on success.
    int crd_openfec_invert(uint8_t* matrix, uint32_t k);
    // of_galois_field_2_8_matmul: out = a (rows x inner) * b (inner x columns), row-major.
    void crd_openfec_matmul(uint8_t* a, uint8_t* b, uint8_t* out, uint32_t rows, uint32_t inner, uint32_t columns);

    // of_rs_2_m_build_repair_symbol over `sources` (k symbols of `bytes`): symbol `esi` of the code (k, n). 0 on
    // success.
    int crd_openfec_encode(uint32_t k, uint32_t n, uint32_t bytes, uint8_t* const* sources, uint32_t esi, uint8_t* out);
    // The real decoder: symbols[j] is symbol j of the code (k, n), or null when it was lost. Each lost source comes
    // back in outputs[i]. 0 on success.
    int crd_openfec_decode(uint32_t k, uint32_t n, uint32_t bytes, uint8_t* const* symbols, uint8_t* const* outputs);

#ifdef __cplusplus
}
#endif
