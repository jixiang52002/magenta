// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "acpi.h"
#include "devmgr.h"
#include "devhost.h"

#include <stdio.h>

#include <launchpad/launchpad.h>
#include <magenta/processargs.h>

#include <mxio/remoteio.h>
#include <mxio/util.h>

#include <magenta/syscalls.h>

#if DEVMGR
mx_handle_t vfs_create_root_handle(void);
#define LOG_FLAGS MX_LOG_FLAG_DEVMGR
#else
#define LOG_FLAGS MX_LOG_FLAG_DEVICE
#endif

void devmgr_io_init(void) {
    // setup stdout
    mx_handle_t h;
    if ((h = mx_log_create(LOG_FLAGS)) < 0) {
        return;
    }
    mxio_t* logger;
    if ((logger = mxio_logger_create(h)) == NULL) {
        return;
    }
    close(1);
    mxio_bind_to_fd(logger, 1, 0);
}

void devmgr_launch_devhost(const char* name, int argc, char** argv,
                           mx_handle_t hdevice, mx_handle_t hrpc) {
    mx_handle_t hnd[6];
    uint32_t ids[6];
    ids[0] = MX_HND_INFO(MX_HND_TYPE_VDSO_VMO, 0);
    hnd[0] = launchpad_get_vdso_vmo();
    ids[1] = MX_HND_TYPE_USER0;
    hnd[1] = hdevice;
    ids[2] = MX_HND_TYPE_USER1;
    hnd[2] = hrpc;
    ids[3] = MX_HND_TYPE_RESOURCE;
    hnd[3] = mx_handle_duplicate(get_root_resource(), MX_RIGHT_SAME_RIGHTS);
    ids[4] = MX_HND_TYPE_MXIO_ROOT;
#if DEVMGR
    hnd[4] = vfs_create_root_handle();
#else
    hnd[4] = 0;
    uint32_t type;
    mxio_clone_root(hnd + 4, &type);
#endif

    uint32_t hcount = 5;

#if !LIBDRIVER
#if !DEVMGR
    // pass acpi handle if available
    ids[5] = MX_HND_TYPE_USER2;
    hnd[5] = devmgr_acpi_clone();
    if (hnd[5] > 0) {
        hcount++;
    }
#endif
#endif

    printf("devmgr: launch: %s %s %s\n", name, argv[0], argv[1]);
    mx_handle_t proc = launchpad_launch(name, argc, (const char* const*)argv,
                                        (const char* const*)environ, hcount, hnd, ids);
    if (proc < 0) {
        printf("devmgr: launch failed: %d\n", proc);
    } else {
        mx_handle_close(proc);
    }
}