// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2015 Google, Inc. All rights reserved
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef __LIB_PAGE_ALLOC_H
#define __LIB_PAGE_ALLOC_H

#include <stddef.h>
#include <sys/types.h>
#include <magenta/compiler.h>

// to pick up PAGE_SIZE, PAGE_ALIGN, etc
#if WITH_KERNEL_VM
#include <kernel/vm.h>
#else
#include <kernel/novm.h>
#endif

/* A simple page-aligned wrapper around the pmm or novm implementation of
 * the underlying physical page allocator. Used by system heaps or any
 * other user that wants pages of memory but doesn't want to use LK
 * specific apis.
 */

__BEGIN_CDECLS;

void *page_alloc(size_t pages);
void page_free(void *ptr, size_t pages);

// You can call this once at the start, and it will either return a page or it
// will return some non-page-aligned memory that would otherwise go to waste.
void *page_first_alloc(size_t *size_return);

__END_CDECLS;

#endif
