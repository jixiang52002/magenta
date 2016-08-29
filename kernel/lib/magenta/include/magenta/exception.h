// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <magenta/compiler.h>
#include <sys/types.h>
#include <magenta/types.h>

__BEGIN_CDECLS

// low level to high level exception handler, called at interrupt or exception context
enum {
    EXC_FATAL_PAGE_FAULT,
    EXC_UNDEFINED_INSTRUCTION,
    EXC_GENERAL,
};

typedef struct arch_exception_context arch_exception_context_t;

status_t magenta_exception_handler(uint exception_type,
                                   struct arch_exception_context* context,
                                   mx_vaddr_t ip);

// arch code must implement this to dump the architecturally specific state they passed to magenta_exception_handler
void arch_dump_exception_context(arch_exception_context_t *);

__END_CDECLS
