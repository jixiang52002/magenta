// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2009 Corey Tabaka
// Copyright (c) 2015 Intel Corporation
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <debug.h>
#include <trace.h>
#include <arch/x86.h>
#include <arch/x86/apic.h>
#include <arch/x86/interrupts.h>
#include <arch/x86/descriptor.h>
#include <kernel/thread.h>
#include <platform.h>

#include <lib/user_copy.h>

#if WITH_LIB_MAGENTA
#include <magenta/exception.h>
#endif

extern enum handler_return platform_irq(x86_iframe_t *frame);

static void dump_fault_frame(x86_iframe_t *frame)
{
#if ARCH_X86_32
    dprintf(CRITICAL, " CS:     %04x EIP: %08x EFL: %08x CR2: %08lx\n",
            frame->cs, frame->ip, frame->flags, x86_get_cr2());
    dprintf(CRITICAL, "EAX: %08x ECX: %08x EDX: %08x EBX: %08x\n",
            frame->eax, frame->ecx, frame->edx, frame->ebx);
    dprintf(CRITICAL, "ESP: %08x EBP: %08x ESI: %08x EDI: %08x\n",
            frame->esp, frame->ebp, frame->esi, frame->edi);
    dprintf(CRITICAL, " DS:     %04x  ES:     %04x  FS:   %04x  GS:     %04x\n",
            frame->ds, frame->es, frame->fs, frame->gs);
#elif ARCH_X86_64
    dprintf(CRITICAL, " CS:  %#18llx RIP: %#18llx EFL: %#18llx CR2: %#18lx\n",
            frame->cs, frame->ip, frame->flags, x86_get_cr2());
    dprintf(CRITICAL, " RAX: %#18llx RBX: %#18llx RCX: %#18llx RDX: %#18llx\n",
            frame->rax, frame->rbx, frame->rcx, frame->rdx);
    dprintf(CRITICAL, " RSI: %#18llx RDI: %#18llx RBP: %#18llx RSP: %#18llx\n",
            frame->rsi, frame->rdi, frame->rbp, frame->user_sp);
    dprintf(CRITICAL, "  R8: %#18llx  R9: %#18llx R10: %#18llx R11: %#18llx\n",
            frame->r8, frame->r9, frame->r10, frame->r11);
    dprintf(CRITICAL, " R12: %#18llx R13: %#18llx R14: %#18llx R15: %#18llx\n",
            frame->r12, frame->r13, frame->r14, frame->r15);
    dprintf(CRITICAL, "errc: %#18llx\n",
            frame->err_code);
#endif

    // dump the bottom of the current stack
    void *stack = frame;

    if (frame->cs == CODE_64_SELECTOR) {
        dprintf(CRITICAL, "bottom of kernel stack at %p:\n", stack);
        hexdump(stack, 128);
    }
}

static void exception_die(x86_iframe_t *frame, const char *msg)
{
    dprintf(CRITICAL, "%s", msg);
    dump_fault_frame(frame);

#if ARCH_X86_64
    // try to dump the user stack
    if (is_user_address(frame->user_sp)) {
        uint8_t buf[256];
        if (copy_from_user_unsafe(buf, (void *)frame->user_sp, sizeof(buf)) == NO_ERROR) {
            printf("bottom of user stack at 0x%lx:\n", (vaddr_t)frame->user_sp);
            hexdump_ex(buf, sizeof(buf), frame->user_sp);
        }
    }
#endif

    platform_halt(HALT_ACTION_HALT, HALT_REASON_SW_PANIC);
}

void x86_syscall_handler(x86_iframe_t *frame)
{
    exception_die(frame, "unhandled syscall, halting\n");
}

void x86_gpf_handler(x86_iframe_t *frame)
{
#if WITH_LIB_MAGENTA
    bool from_user = SELECTOR_PL(frame->cs) != 0;
    if (from_user) {
        struct arch_exception_context context = { .frame = frame, .is_page_fault = false };
        arch_enable_ints();
        status_t erc = magenta_exception_handler(EXC_GENERAL, &context, frame->ip);
        arch_disable_ints();
        if (erc == NO_ERROR)
            return;
    }
#endif
    exception_die(frame, "unhandled gpf, halting\n");
}

void x86_invop_handler(x86_iframe_t *frame)
{
#if WITH_LIB_MAGENTA
    bool from_user = SELECTOR_PL(frame->cs) != 0;
    if (from_user) {
        struct arch_exception_context context = { .frame = frame, .is_page_fault = false };
        arch_enable_ints();
        status_t erc = magenta_exception_handler(EXC_UNDEFINED_INSTRUCTION, &context, frame->ip);
        arch_disable_ints();
        if (erc == NO_ERROR)
            return;
    }
#endif
    exception_die(frame, "invalid opcode, halting\n");
}

void x86_unhandled_exception(x86_iframe_t *frame)
{
#if WITH_LIB_MAGENTA
    bool from_user = SELECTOR_PL(frame->cs) != 0;
    if (from_user) {
        struct arch_exception_context context = { .frame = frame, .is_page_fault = false };
        arch_enable_ints();
        status_t erc = magenta_exception_handler(EXC_GENERAL, &context, frame->ip);
        arch_disable_ints();
        if (erc == NO_ERROR)
            return;
    }
#endif
    printf("vector %u\n", (uint)frame->vector);
    exception_die(frame, "unhandled exception, halting\n");
}

static void x86_dump_pfe(x86_iframe_t *frame, ulong cr2)
{
    uint32_t error_code = frame->err_code;

    addr_t v_addr = cr2;
    addr_t ssp = frame->user_ss & X86_8BYTE_MASK;
    addr_t sp = frame->user_sp;
    addr_t cs  = frame->cs & X86_8BYTE_MASK;
    addr_t ip = frame->ip;

    dprintf(CRITICAL, "<PAGE FAULT> Instruction Pointer   = 0x%lx:0x%lx\n",
            (ulong)cs,
            (ulong)ip);
    dprintf(CRITICAL, "<PAGE FAULT> Stack Pointer         = 0x%lx:0x%lx\n",
            (ulong)ssp,
            (ulong)sp);
    dprintf(CRITICAL, "<PAGE FAULT> Fault Linear Address  = 0x%lx\n",
            (ulong)v_addr);
    dprintf(CRITICAL, "<PAGE FAULT> Error Code Value      = 0x%lx\n",
            (ulong)error_code);
    dprintf(CRITICAL, "<PAGE FAULT> Error Code Type       = %s %s %s%s, %s\n",
            error_code & PFEX_U ? "user" : "supervisor",
            error_code & PFEX_W ? "write" : "read",
            error_code & PFEX_I ? "instruction" : "data",
            error_code & PFEX_RSV ? " rsv" : "",
            error_code & PFEX_P ? "protection violation" : "page not present");
}


static void x86_fatal_pfe_handler(x86_iframe_t *frame, ulong cr2)
{
    x86_dump_pfe(frame, cr2);

    uint32_t error_code = frame->err_code;

    dump_thread(get_current_thread());

    if (error_code & PFEX_U) {
        // User mode page fault
        switch (error_code) {
            case 4:
            case 5:
            case 6:
            case 7:
                exception_die(frame, "User Page Fault exception, halting\n");
                break;
        }
    } else {
        // Supervisor mode page fault
        switch (error_code) {

            case 0:
            case 1:
            case 2:
            case 3:
                exception_die(frame, "Supervisor Page Fault exception, halting\n");
                break;
        }
    }

    exception_die(frame, "unhandled page fault, halting\n");
}

void x86_pfe_handler(x86_iframe_t *frame)
{
    /* Handle a page fault exception */
    uint32_t error_code = frame->err_code;
    vaddr_t va = x86_get_cr2();

    /* reenable interrupts */
    arch_enable_ints();

    /* check for flags we're not prepared to handle */
    if (unlikely(error_code & ~(PFEX_I | PFEX_U | PFEX_W | PFEX_P))) {
        printf("x86_pfe_handler: unhandled error code bits set, error code 0x%x\n", error_code);
        x86_fatal_pfe_handler(frame, va);
    }

    /* convert the PF error codes to page fault flags */
    uint flags = 0;
    flags |= (error_code & PFEX_W) ? VMM_PF_FLAG_WRITE : 0;
    flags |= (error_code & PFEX_U) ? VMM_PF_FLAG_USER : 0;
    flags |= (error_code & PFEX_I) ? VMM_PF_FLAG_INSTRUCTION : 0;
    flags |= (error_code & PFEX_P) ? 0 : VMM_PF_FLAG_NOT_PRESENT;

    status_t pf_err = vmm_page_fault_handler(va, flags);
    if (unlikely(pf_err < 0)) {
        /* if the high level page fault handler can't deal with it,
         * resort to trying to recover first, before bailing */

#ifdef ARCH_X86_64
        /* Check if a resume address is specified, and just return to it if so */
        thread_t *current_thread = get_current_thread();
        if (unlikely(current_thread->arch.page_fault_resume)) {
            frame->ip = (uintptr_t)current_thread->arch.page_fault_resume;
            return;
        }
#endif

        /* let high level code deal with this */
#if WITH_LIB_MAGENTA
        bool from_user = SELECTOR_PL(frame->cs) != 0;
        if (from_user) {
            struct arch_exception_context context = { .frame = frame, .is_page_fault = true, .cr2 = va };
            status_t erc = magenta_exception_handler(EXC_FATAL_PAGE_FAULT, &context, frame->ip);
            arch_disable_ints();
            if (erc == NO_ERROR)
                return;
        }
#else
        arch_disable_ints();
#endif

        /* fatal (for now) */
        x86_fatal_pfe_handler(frame, va);
    }
}

/* top level x86 exception handler for most exceptions and irqs */
void x86_exception_handler(x86_iframe_t *frame)
{
    THREAD_STATS_INC(interrupts);

    // did we come from user or kernel space?
    bool from_user = SELECTOR_PL(frame->cs) != 0;

    // deliver the interrupt
    enum handler_return ret = INT_NO_RESCHEDULE;

    switch (frame->vector) {
        case X86_INT_INVALID_OP:
            x86_invop_handler(frame);
            break;

        case X86_INT_DEVICE_NA: {
            exception_die(frame, "device na fault\n");
        }

        case X86_INT_FPU_FP_ERROR: {
            uint16_t fsw;
            __asm__ __volatile__("fnstsw %0" : "=m" (fsw));
            TRACEF("fsw 0x%hx\n", fsw);
            exception_die(frame, "x87 math fault\n");
            //asm volatile("fnclex");
            break;
        }
        case X86_INT_SIMD_FP_ERROR: {
            uint32_t mxcsr;
            __asm__ __volatile__("stmxcsr %0" : "=m" (mxcsr));
            TRACEF("mxcsr 0x%x\n", mxcsr);
            exception_die(frame, "simd math fault\n");
            break;
        }
        case X86_INT_GP_FAULT:
            x86_gpf_handler(frame);
            break;

        case X86_INT_PAGE_FAULT:
            x86_pfe_handler(frame);
            break;

        /* ignore spurious APIC irqs */
        case X86_INT_APIC_SPURIOUS: break;
        case X86_INT_APIC_ERROR: {
            ret = apic_error_interrupt_handler();
            apic_issue_eoi();
            break;
        }
        case X86_INT_APIC_TIMER: {
            ret = apic_timer_interrupt_handler();
            apic_issue_eoi();
            break;
        }
#if WITH_SMP
        case X86_INT_IPI_GENERIC: {
            ret = x86_ipi_generic_handler();
            apic_issue_eoi();
            break;
        }
        case X86_INT_IPI_RESCHEDULE: {
            ret = x86_ipi_reschedule_handler();
            apic_issue_eoi();
            break;
        }
        case X86_INT_IPI_HALT: {
            x86_ipi_halt_handler();
            /* no return */
            break;
        }
#endif
        /* pass all other non-Intel defined irq vectors to the platform */
        case X86_INT_PLATFORM_BASE  ... X86_INT_PLATFORM_MAX: {
            ret = platform_irq(frame);
            break;
        }
        default:
            x86_unhandled_exception(frame);
    }

    /* if we came from user space, check to see if we have any signals to handle */
    if (unlikely(from_user)) {
        /* in the case of receiving a kill signal, this function may not return,
         * but the scheduler would have been invoked so it's fine.
         */
        thread_process_pending_signals();
    }

    if (ret != INT_NO_RESCHEDULE)
        thread_preempt(true);
}

__WEAK uint64_t x86_64_syscall(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,
                               uint64_t arg5, uint64_t arg6, uint64_t arg7, uint64_t arg8,
                               uint64_t syscall_num)
{
    PANIC_UNIMPLEMENTED;
}

#if WITH_LIB_MAGENTA
void arch_dump_exception_context(const arch_exception_context_t *context)
{
    if (context->is_page_fault) {
        x86_dump_pfe(context->frame, context->cr2);
    }

    dump_fault_frame(context->frame);

    // try to dump the user stack
    if (context->frame->cs != CODE_64_SELECTOR && is_user_address(context->frame->user_sp)) {
        uint8_t buf[256];
        if (copy_from_user_unsafe(buf, (void *)context->frame->user_sp, sizeof(buf)) == NO_ERROR) {
            printf("bottom of user stack at 0x%lx:\n", (vaddr_t)context->frame->user_sp);
            hexdump_ex(buf, sizeof(buf), context->frame->user_sp);
        }
    }
}

void arch_fill_in_exception_context(const arch_exception_context_t *arch_context, mx_exception_context_t *mx_context)
{
    mx_context->arch_id = ARCH_ID_X86_64;

    mx_context->arch.u.x86_64.vector = arch_context->frame->vector;
    mx_context->arch.u.x86_64.err_code = arch_context->frame->err_code;
    mx_context->arch.u.x86_64.cr2 = arch_context->cr2;
}

#endif
