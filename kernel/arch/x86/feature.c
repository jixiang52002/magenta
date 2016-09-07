// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <arch/x86/feature.h>

#include <trace.h>
#include <stdint.h>
#include <assert.h>

#include <arch/ops.h>

#define LOCAL_TRACE 0

struct cpuid_leaf _cpuid[MAX_SUPPORTED_CPUID + 1];
struct cpuid_leaf _cpuid_ext[MAX_SUPPORTED_CPUID_EXT - X86_CPUID_EXT_BASE + 1];
uint32_t max_cpuid = 0;
uint32_t max_ext_cpuid = 0;

static int initialized = 0;

void x86_feature_init(void)
{
    if (atomic_swap(&initialized, 1)) {
        return;
    }
    /* test for cpuid count */
    cpuid(0, &_cpuid[0].a, &_cpuid[0].b, &_cpuid[0].c, &_cpuid[0].d);

    max_cpuid = _cpuid[0].a;
    if (max_cpuid > MAX_SUPPORTED_CPUID)
        max_cpuid = MAX_SUPPORTED_CPUID;

    LTRACEF("max cpuid 0x%x\n", max_cpuid);

    /* read in the base cpuids */
    for (uint32_t i = 1; i <= max_cpuid; i++) {
        cpuid_c(i, 0, &_cpuid[i].a, &_cpuid[i].b, &_cpuid[i].c, &_cpuid[i].d);
    }

    /* test for extended cpuid count */
    cpuid(X86_CPUID_EXT_BASE, &_cpuid_ext[0].a, &_cpuid_ext[0].b, &_cpuid_ext[0].c, &_cpuid_ext[0].d);

    max_ext_cpuid = _cpuid_ext[0].a;
    LTRACEF("max extended cpuid 0x%x\n", max_ext_cpuid);
    if (max_ext_cpuid > MAX_SUPPORTED_CPUID_EXT)
        max_ext_cpuid = MAX_SUPPORTED_CPUID_EXT;

    /* read in the extended cpuids */
    for (uint32_t i = X86_CPUID_EXT_BASE + 1; i - 1 < max_ext_cpuid; i++) {
        uint32_t index = i - X86_CPUID_EXT_BASE;
        cpuid_c(i, 0, &_cpuid_ext[index].a, &_cpuid_ext[index].b, &_cpuid_ext[index].c, &_cpuid_ext[index].d);
    }

#if LK_DEBUGLEVEL > 1
    x86_feature_debug();
#endif
}

bool x86_get_cpuid_subleaf(
        enum x86_cpuid_leaf_num num, uint32_t subleaf, struct cpuid_leaf *leaf)
{
    if (num < X86_CPUID_EXT_BASE) {
        if (num > max_cpuid)
            return false;
    } else if (num > max_ext_cpuid) {
        return false;
    }

    cpuid_c((uint32_t)num, subleaf, &leaf->a, &leaf->b, &leaf->c, &leaf->d);
    return true;
}

bool x86_topology_enumerate(uint8_t level, struct x86_topology_level *info)
{
    DEBUG_ASSERT(info);

    uint32_t eax, ebx, ecx, edx;
    cpuid_c(X86_CPUID_TOPOLOGY, level, &eax, &ebx, &ecx, &edx);

    uint8_t type = (ecx >> 8) & 0xff;
    if (type == X86_TOPOLOGY_INVALID) {
        return false;
    }

    info->right_shift = eax & 0x1f;
    info->type = type;
    return true;
}

void x86_feature_debug(void)
{
    static struct {
        struct x86_cpuid_bit bit;
        const char *name;
    } features[] = {
        { X86_FEATURE_FPU, "fpu" },
        { X86_FEATURE_SSE, "sse" },
        { X86_FEATURE_SSE2, "sse2" },
        { X86_FEATURE_SSE3, "sse3" },
        { X86_FEATURE_SSSE3, "ssse3" },
        { X86_FEATURE_SSE4_1, "sse4.1" },
        { X86_FEATURE_SSE4_2, "sse4.2" },
        { X86_FEATURE_MMX, "mmx" },
        { X86_FEATURE_AVX, "avx" },
        { X86_FEATURE_AVX2, "avx2" },
        { X86_FEATURE_FXSR, "fxsr" },
        { X86_FEATURE_XSAVE, "xsave" },
        { X86_FEATURE_AESNI, "aesni" },
        { X86_FEATURE_TSC_ADJUST, "tsc_adj" },
        { X86_FEATURE_SMEP, "smep" },
        { X86_FEATURE_SMAP, "smap" },
        { X86_FEATURE_RDRAND, "rdrand" },
        { X86_FEATURE_RDSEED, "rdseed" },
        { X86_FEATURE_PKU, "pku" },
        { X86_FEATURE_SYSCALL, "syscall" },
        { X86_FEATURE_NX, "nx" },
        { X86_FEATURE_HUGE_PAGE, "huge" },
        { X86_FEATURE_RDTSCP, "rdtscp" },
        { X86_FEATURE_INVAR_TSC, "invar_tsc" },
        { X86_FEATURE_TSC_DEADLINE, "tsc_deadline" },
    };

    printf("Features:");
    for (uint i = 0; i < countof(features); ++i) {
        if (x86_feature_test(features[i].bit))
            printf(" %s", features[i].name);
    }
    printf("\n");
}
