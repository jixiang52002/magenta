// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2009 Corey Tabaka
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <stdarg.h>
#include <reg.h>
#include <stdio.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <arch/x86.h>
#include <arch/x86/apic.h>
#include <lib/cbuf.h>
#include <dev/interrupt.h>
#include <platform.h>
#include <platform/pc.h>
#include <platform/pc/memmap.h>
#include <platform/console.h>
#include <platform/debug.h>

static const int uart_baud_rate = 115200;
static const int uart_io_port = 0x3f8;

cbuf_t console_input_buf;

enum handler_return platform_drain_debug_uart_rx(void)
{
    unsigned char c;
    bool resched = false;

    while (inp(uart_io_port + 5) & (1<<0)) {
        c = inp(uart_io_port + 0);
        cbuf_write_char(&console_input_buf, c, false);
        resched = true;
    }

    return resched ? INT_RESCHEDULE : INT_NO_RESCHEDULE;
}

static enum handler_return uart_irq_handler(void *arg)
{
    return platform_drain_debug_uart_rx();
}

void platform_init_debug_early(void)
{
    /* configure the uart */
    int divisor = 115200 / uart_baud_rate;

    /* get basic config done so that tx functions */
    outp(uart_io_port + 3, 0x80); // set up to load divisor latch
    outp(uart_io_port + 0, divisor & 0xff); // lsb
    outp(uart_io_port + 1, divisor >> 8); // msb
    outp(uart_io_port + 3, 3); // 8N1
    outp(uart_io_port + 2, 0x07); // enable FIFO, clear, 14-byte threshold
}

void platform_init_debug(void)
{
    /* finish uart init to get rx going */
    cbuf_initialize(&console_input_buf, 1024);

    uint32_t irq = apic_io_isa_to_global(ISA_IRQ_SERIAL1);
    register_int_handler(irq, uart_irq_handler, NULL);
    unmask_interrupt(irq);

    outp(uart_io_port + 1, 0x1); // enable receive data available interrupt
}

static void debug_uart_putc(char c)
{
    while ((inp(uart_io_port + 5) & (1<<6)) == 0)
        ;
    outp(uart_io_port + 0, c);
}

void platform_dputc(char c)
{
    if (c == '\n')
        platform_dputc('\r');

    cputc(c);
    debug_uart_putc(c);
}

int platform_dgetc(char *c, bool wait)
{
    return cbuf_read_char(&console_input_buf, c, wait);
}

// panic time polling IO for the panic shell
void platform_pputc(char c)
{
    platform_dputc(c);
}

int platform_pgetc(char *c, bool wait)
{
    if (inp(uart_io_port + 5) & (1<<0)) {
        *c = inp(uart_io_port + 0);
        return 0;
    }

    return -1;
}

