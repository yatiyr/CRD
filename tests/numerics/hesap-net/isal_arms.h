/* ISA-L's erasure encoders, one arm per body (HESAP-NET; CRD_BUILD_ERASURE_VS_ISAL): the portable ec_encode_data_base
 * and, where its assembly is built (CRD_ISAL_ASSEMBLY, x86-64), every ISA body and the dispatched ec_encode_data, each
 * with the table builder it reads (32 bytes per coefficient: the split nibble tables, or the GFNI affine matrices).
 * isal_arms.c declares the bodies itself, so no caller meets ISA-L's deprecation of its per-ISA names. A caller runs an
 * arm only when the CPU has what `needs` names. */
#ifndef CRD_TESTS_HESAP_COMMS_ISAL_ARMS_H
#define CRD_TESTS_HESAP_COMMS_ISAL_ARMS_H

#ifdef __cplusplus
extern "C" {
#endif

/* What an arm's instructions need (ISA-L's own dispatch asks the same of each body). */
enum crd_isal_needs
{
    CRD_ISAL_NEEDS_NONE,
    CRD_ISAL_NEEDS_SSE42,
    CRD_ISAL_NEEDS_AVX,
    CRD_ISAL_NEEDS_AVX2,
    CRD_ISAL_NEEDS_AVX2_GFNI,
    CRD_ISAL_NEEDS_AVX512,
    CRD_ISAL_NEEDS_AVX512_GFNI
};

typedef void (*crd_isal_tables_fn)(int k, int rows, unsigned char* matrix, unsigned char* tables);
typedef void (*crd_isal_encode_fn)(int len, int k, int rows, unsigned char* tables, unsigned char** data,
                                   unsigned char** coding);

struct crd_isal_arm
{
    const char* name;
    crd_isal_tables_fn tables;
    crd_isal_encode_fn encode;
    enum crd_isal_needs needs;
};

extern const struct crd_isal_arm crd_isal_arms[];
extern const unsigned crd_isal_arm_count;

#ifdef __cplusplus
}

#include <crd/core/cpu_features.hpp>

// Whether this process's CPUs execute `arm` (the usable features every CPU reports).
inline bool crd_isal_arm_runs(const crd_isal_arm& arm) noexcept
{
    using crd::CpuFeature;
    const crd::CpuFeatureSet usable = crd::cpu_selection().features.usable;
    const bool avx512 = usable.has(CpuFeature::Avx512F) && usable.has(CpuFeature::Avx512Vl) &&
                        usable.has(CpuFeature::Avx512Bw) && usable.has(CpuFeature::Avx512Cd) &&
                        usable.has(CpuFeature::Avx512Dq);
    switch (arm.needs)
    {
    case CRD_ISAL_NEEDS_NONE:
        return true;
    case CRD_ISAL_NEEDS_SSE42:
        return usable.has(CpuFeature::Sse42);
    case CRD_ISAL_NEEDS_AVX:
        return usable.has(CpuFeature::Avx);
    case CRD_ISAL_NEEDS_AVX2:
        return usable.has(CpuFeature::Avx2);
    case CRD_ISAL_NEEDS_AVX2_GFNI:
        return usable.has(CpuFeature::Avx2) && usable.has(CpuFeature::Gfni);
    case CRD_ISAL_NEEDS_AVX512:
        return avx512;
    case CRD_ISAL_NEEDS_AVX512_GFNI:
        return avx512 && usable.has(CpuFeature::Gfni);
    }
    return false;
}
#endif

#endif
