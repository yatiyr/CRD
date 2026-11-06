// See openfec_peer.h. OpenFEC's verbosity lives in of_openfec_api.c, which also pulls in every other codec.
#include "of_reed-solomon_gf_2_m_includes.h"

#include "openfec_peer.h"

UINT32 of_verbosity = 0;

static of_rs_2_m_cb_t* crd_openfec_codec(uint32_t k, uint32_t n, uint32_t bytes)
{
    of_rs_2_m_cb_t* codec = (of_rs_2_m_cb_t*)calloc(1, sizeof(of_cb_t));
    of_rs_2_m_parameters_t parameters;
    if (codec == NULL || of_rs_2_m_create_codec_instance(&codec) != OF_STATUS_OK)
    {
        return NULL;
    }
    parameters.nb_source_symbols = k;
    parameters.nb_repair_symbols = n - k;
    parameters.encoding_symbol_length = bytes;
    parameters.m = 8;
    if (of_rs_2_m_set_fec_parameters(codec, &parameters) != OF_STATUS_OK)
    {
        of_rs_2_m_release_codec_instance(codec);
        return NULL;
    }
    return codec;
}

uint8_t crd_openfec_mul(uint8_t a, uint8_t b)
{
    return of_gf_2_8_mul_table[a][b];
}

uint8_t crd_openfec_inv(uint8_t a)
{
    return of_gf_2_8_inv[a];
}

uint8_t crd_openfec_alpha_pow(uint32_t e)
{
    return of_gf_2_8_exp[e % 255U];
}

int crd_openfec_invert(uint8_t* matrix, uint32_t k)
{
    of_galois_field_code_cb_t codec;
    memset(&codec, 0, sizeof(codec));
    codec.m = 8;
    codec.field_size = 255;
    return of_galois_field_2_8_invert_mat(&codec, matrix, (int)k);
}

void crd_openfec_matmul(uint8_t* a, uint8_t* b, uint8_t* out, uint32_t rows, uint32_t inner, uint32_t columns)
{
    of_galois_field_2_8_matmul(a, b, out, (int)rows, (int)inner, (int)columns);
}

int crd_openfec_encode(uint32_t k, uint32_t n, uint32_t bytes, uint8_t* const* sources, uint32_t esi, uint8_t* out)
{
    void* symbols[256];
    of_rs_2_m_cb_t* codec = crd_openfec_codec(k, n, bytes);
    of_status_t status;
    if (codec == NULL)
    {
        return -1;
    }
    memset(symbols, 0, sizeof(symbols));
    for (uint32_t i = 0U; i < k; ++i)
    {
        symbols[i] = sources[i];
    }
    symbols[esi] = out;
    status = of_rs_2_m_build_repair_symbol(codec, symbols, esi);
    of_rs_2_m_release_codec_instance(codec);
    return status == OF_STATUS_OK ? 0 : -1;
}

static void* crd_openfec_output(void* context, UINT32 size, UINT32 esi)
{
    (void)size;
    return ((uint8_t* const*)context)[esi];
}

int crd_openfec_decode(uint32_t k, uint32_t n, uint32_t bytes, uint8_t* const* symbols, uint8_t* const* outputs)
{
    void* available[256];
    of_rs_2_m_cb_t* codec = crd_openfec_codec(k, n, bytes);
    of_status_t status;
    if (codec == NULL)
    {
        return -1;
    }
    for (uint32_t j = 0U; j < n; ++j)
    {
        available[j] = symbols[j];
    }
    codec->decoded_source_symbol_callback = crd_openfec_output;
    codec->context_4_callback = (void*)outputs;
    status = of_rs_2_m_set_available_symbols(codec, available);
    if (status == OF_STATUS_OK)
    {
        status = of_rs_2_m_finish_decoding(codec);
    }
    of_rs_2_m_release_codec_instance(codec);
    return status == OF_STATUS_OK ? 0 : -1;
}
