/* ISA-L's erasure encoders as arms (isal_arms.h). The bodies are ISA-L's own (erasure_code/ec_base.c, and
 * ec_highlevel_func.c over its assembly); they are declared here with the prototypes erasure_code.h and
 * ec_highlevel_func.c give them, because that header marks the per-ISA names deprecated for applications (on MSVC
 * whatever the caller defines). */
#include "isal_arms.h"

#define CRD_ISAL_BODY(name)                                                                                            \
    void name(int len, int k, int rows, unsigned char* g_tbls, unsigned char** data, unsigned char** coding)
#define CRD_ISAL_TABLES(name) void name(int k, int rows, unsigned char* a, unsigned char* g_tbls)

CRD_ISAL_TABLES(ec_init_tables_base);
CRD_ISAL_BODY(ec_encode_data_base);
#if CRD_ISAL_ASSEMBLY
CRD_ISAL_TABLES(ec_init_tables);
CRD_ISAL_TABLES(ec_init_tables_gfni);
CRD_ISAL_BODY(ec_encode_data);
CRD_ISAL_BODY(ec_encode_data_sse);
CRD_ISAL_BODY(ec_encode_data_avx);
CRD_ISAL_BODY(ec_encode_data_avx2);
CRD_ISAL_BODY(ec_encode_data_avx2_gfni);
CRD_ISAL_BODY(ec_encode_data_avx512);
CRD_ISAL_BODY(ec_encode_data_avx512_gfni);
#endif

const struct crd_isal_arm crd_isal_arms[] = {
    {"base", ec_init_tables_base, ec_encode_data_base, CRD_ISAL_NEEDS_NONE},
#if CRD_ISAL_ASSEMBLY
    {"sse", ec_init_tables_base, ec_encode_data_sse, CRD_ISAL_NEEDS_SSE42},
    {"avx", ec_init_tables_base, ec_encode_data_avx, CRD_ISAL_NEEDS_AVX},
    {"avx2", ec_init_tables_base, ec_encode_data_avx2, CRD_ISAL_NEEDS_AVX2},
    {"avx2-gfni", ec_init_tables_gfni, ec_encode_data_avx2_gfni, CRD_ISAL_NEEDS_AVX2_GFNI},
    {"avx512", ec_init_tables_base, ec_encode_data_avx512, CRD_ISAL_NEEDS_AVX512},
    {"avx512-gfni", ec_init_tables_gfni, ec_encode_data_avx512_gfni, CRD_ISAL_NEEDS_AVX512_GFNI},
    {"dispatched", ec_init_tables, ec_encode_data, CRD_ISAL_NEEDS_NONE},
#endif
};

const unsigned crd_isal_arm_count = sizeof(crd_isal_arms) / sizeof(crd_isal_arms[0]);
