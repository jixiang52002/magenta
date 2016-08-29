// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2008-2014 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#ifndef ASSEMBLY

#include <stdbool.h>
#include <magenta/compiler.h>
#include <reg.h>
#include <arch/arm.h>

__BEGIN_CDECLS

#if ARM_ISA_ARMV7 || (ARM_ISA_ARMV6 && !__thumb__)
#define ENABLE_CYCLE_COUNTER 1

// override of some routines
static inline void arch_enable_ints(void)
{
    CF;
    __asm__ volatile("cpsie i");
}

static inline void arch_disable_ints(void)
{
    __asm__ volatile("cpsid i");
    CF;
}

static inline bool arch_ints_disabled(void)
{
    unsigned int state;

#if ARM_ISA_ARMV7M
    __asm__ volatile("mrs %0, primask" : "=r"(state));
    state &= 0x1;
#else
    __asm__ volatile("mrs %0, cpsr" : "=r"(state));
    state &= (1<<7);
#endif

    return !!state;
}

static inline void arch_enable_fiqs(void)
{
    CF;
    __asm__ volatile("cpsie f");
}

static inline void arch_disable_fiqs(void)
{
    __asm__ volatile("cpsid f");
    CF;
}

static inline bool arch_fiqs_disabled(void)
{
    unsigned int state;

    __asm__ volatile("mrs %0, cpsr" : "=r"(state));
    state &= (1<<6);

    return !!state;
}

static inline bool arch_in_int_handler(void)
{
    /* set by the interrupt glue to track that the cpu is inside a handler */
    extern bool __arm_in_handler;

    return __arm_in_handler;
}

static inline void arch_spinloop_pause(void)
{
    __asm__ volatile("wfe");
}

static inline void arch_spinloop_signal(void)
{
    __asm__ volatile("sev");
}

static inline uint32_t arch_cycle_count(void)
{
#if ARM_ISA_ARMV7M
#if ENABLE_CYCLE_COUNTER
#define DWT_CYCCNT (0xE0001004)
    return *REG32(DWT_CYCCNT);
#else
    return 0;
#endif
#elif ARM_ISA_ARMV7
    uint32_t count;
    __asm__ volatile("mrc		p15, 0, %0, c9, c13, 0"
                     : "=r" (count)
                    );
    return count;
#else
//#warning no arch_cycle_count implementation
    return 0;
#endif
}

#if WITH_SMP && ARM_ISA_ARMV7
static inline uint arch_curr_cpu_num(void)
{
    uint32_t mpidr = arm_read_mpidr();
    return ((mpidr & ((1U << SMP_CPU_ID_BITS) - 1)) >> 8 << SMP_CPU_CLUSTER_SHIFT) | (mpidr & 0xff);
}

extern uint arm_num_cpus;
static inline uint arch_max_num_cpus(void)
{
    return arm_num_cpus;
}
#else
static inline uint arch_curr_cpu_num(void)
{
    return 0;
}
static inline uint arch_max_num_cpus(void)
{
    return 1;
}
#endif

/* defined in kernel/thread.h */

#if !ARM_ISA_ARMV7M
/* use the cpu local thread context pointer to store current_thread */
static inline struct thread *get_current_thread(void)
{
    return (struct thread *)arm_read_tpidrprw();
}

static inline void set_current_thread(struct thread *t)
{
    arm_write_tpidrprw((uint32_t)t);
}
#else // ARM_ISA_ARM7M

/* use a global pointer to store the current_thread */
extern struct thread *_current_thread;

static inline struct thread *get_current_thread(void)
{
    return _current_thread;
}

static inline void set_current_thread(struct thread *t)
{
    _current_thread = t;
}

#endif // !ARM_ISA_ARMV7M

#elif ARM_ISA_ARMV6M // cortex-m0 cortex-m0+
#error "ARMV6M not supported"
#else // pre-armv6 || (armv6 & thumb)
#error "pre-ARMV6 not supported"
#endif

#define mb()        DSB
#define wmb()       DSB
#define rmb()       DSB

#ifdef WITH_SMP
#define smp_mb()    DMB
#define smp_wmb()   DMB
#define smp_rmb()   DMB
#else
#define smp_mb()    CF
#define smp_wmb()   CF
#define smp_rmb()   CF
#endif

__END_CDECLS

#endif // ASSEMBLY
