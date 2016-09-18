// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "acpi.h"
#include "devmgr.h"
#include "devhost.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/device.h>
#include <ddk/driver.h>

#include <launchpad/launchpad.h>

#include <magenta/compiler.h>
#include <magenta/ktrace.h>
#include <magenta/processargs.h>
#include <magenta/syscalls.h>
#include <magenta/types.h>

#include <mxio/util.h>

static void devhost_io_init(void) {
    // setup stdout
    mx_handle_t h;
    if ((h = mx_log_create(MX_LOG_FLAG_DEVICE)) < 0) {
        return;
    }
    mxio_t* logger;
    if ((logger = mxio_logger_create(h)) == NULL) {
        return;
    }
    close(1);
    mxio_bind_to_fd(logger, 1, 0);
}

// shared with rpc-device.c
extern mxio_dispatcher_t* devhost_rio_dispatcher;

__EXPORT mx_status_t devhost_add_internal(mx_device_t* parent,
                                          const char* name, uint32_t protocol_id,
                                          mx_handle_t* _hdevice, mx_handle_t* _hrpc) {

    size_t len = strlen(name);
    if (len >= MX_DEVICE_NAME_MAX) {
        return ERR_INVALID_ARGS;
    }

    mx_handle_t hdevice[2];
    mx_handle_t hrpc[2];
    mx_status_t status;
    if ((status = mx_msgpipe_create(hdevice, 0)) < 0) {
        printf("devhost_add: failed to create msgpipe: %d\n", status);
        return status;
    }
    if ((status = mx_msgpipe_create(hrpc, 0)) < 0) {
        printf("devhost_add: failed to create msgpipe: %d\n", status);
        mx_handle_close(hdevice[0]);
        mx_handle_close(hdevice[1]);
        return status;
    }

    //printf("devhost_add(%p, %p)\n", dev, parent);
    devhost_msg_t msg;
    msg.op = DH_OP_ADD;
    msg.arg = 0;
    msg.protocol_id = protocol_id;
    memcpy(msg.name, name, len + 1);

    mx_handle_t handles[2] = { hdevice[1], hrpc[1] };
    if ((status = mx_msgpipe_write(parent->rpc, &msg, sizeof(msg), handles, 2, 0)) < 0) {
        printf("devhost_add: failed to write msgpipe: %d\n", status);
        mx_handle_close(hdevice[0]);
        mx_handle_close(hdevice[1]);
        mx_handle_close(hrpc[0]);
        mx_handle_close(hrpc[1]);
        return status;
    }

    *_hdevice = hdevice[0];
    *_hrpc = hrpc[0];

    // far side will close handles if this fails
    return NO_ERROR;
}

static mx_status_t devhost_connect(mx_device_t* dev, mx_handle_t hdevice, mx_handle_t hrpc) {
    iostate_t* ios;
    if ((ios = create_iostate(dev)) == NULL) {
        printf("devhost_connect: cannot alloc iostate\n");
        mx_handle_close(hdevice);
        mx_handle_close(hrpc);
        return ERR_NO_MEMORY;
    }
    dev->rpc = hrpc;
    dev->ctx = ios;
    mx_status_t status;
    if ((status = mxio_dispatcher_add(devhost_rio_dispatcher, hdevice, devhost_rio_handler, ios)) < 0) {
        printf("devhost_connect: cannot add to dispatcher: %d\n", status);
        mx_handle_close(hdevice);
        mx_handle_close(hrpc);
        free(ios);
        dev->rpc = 0;
        dev->ctx = 0;
        return status;
    }
    return NO_ERROR;
}

mx_status_t devhost_add(mx_device_t* parent, mx_device_t* child) {
    mx_handle_t hdevice, hrpc;
    mx_status_t status;
    //printf("devhost_add(%p:%s,%p:%s)\n", parent, parent->name, child, child->name);
    if ((status = devhost_add_internal(parent, child->name, child->protocol_id,
                                       &hdevice, &hrpc)) < 0) {
        return status;
    }
    return devhost_connect(child, hdevice, hrpc);
}

mx_status_t devhost_remove(mx_device_t* dev) {
    devhost_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.op = DH_OP_REMOVE;
    //printf("devhost_remove(%p:%s) ios=%p\n", dev, dev->name, dev->ctx);

    // ensure we don't pull the rug out from under devhost_rio_handler()
    iostate_t* ios = dev->ctx;
    mtx_lock(&ios->lock);
    dev->ctx = NULL;
    ios->dev = NULL;
    mtx_unlock(&ios->lock);

    mx_msgpipe_write(dev->rpc, &msg, sizeof(msg), 0, 0, 0);
    mx_handle_close(dev->rpc);
    dev->rpc = 0;
    return NO_ERROR;
}

mx_handle_t root_resource_handle;

__EXPORT mx_handle_t get_root_resource(void) {
    return root_resource_handle;
}

static mx_driver_t root_driver = {
    .name = "root",
};
static mx_protocol_device_t root_ops;

static mx_handle_t hdevice;
static mx_handle_t hrpc;
static mx_handle_t hacpi;

__EXPORT int devhost_init(void) {
    devhost_io_init();

    root_resource_handle = mxio_get_startup_handle(MX_HND_INFO(MX_HND_TYPE_RESOURCE, 0));
    hdevice = mxio_get_startup_handle(MX_HND_INFO(MX_HND_TYPE_USER0, 0));
    hrpc = mxio_get_startup_handle(MX_HND_INFO(MX_HND_TYPE_USER1, 0));
    hacpi = mxio_get_startup_handle(MX_HND_INFO(MX_HND_TYPE_USER2, 0));

    //TODO: figure out why we need to do this
    mx_handle_t vmo = mxio_get_startup_handle(MX_HND_INFO(MX_HND_TYPE_VDSO_VMO, 0));
    vmo = launchpad_set_vdso_vmo(vmo);

    if (root_resource_handle <= 0) {
        fprintf(stderr, "devhost: missing root resource handle\n");
        return -1;
    }
    if ((hdevice <= 0) || (hrpc <= 0)) {
        fprintf(stderr, "devhost: missing device handle(s)\n");
        return -1;
    }
    if (hacpi <= 0) {
        fprintf(stderr, "devhost: missing acpi handle\n");
    }

    mxio_dispatcher_create(&devhost_rio_dispatcher, mxrio_handler);
    return 0;
}

static bool as_root = false;

__EXPORT int devhost_cmdline(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "devhost: missing command line argument\n");
        return -1;
    }

    mx_device_t* dev;
    mx_status_t status;
    if (!strcmp(argv[1], "root")) {
        // The "root" devhost is launched by devmgr and currently hosts
        // the drivers without bind programs (singletons like null or console,
        // bus drivers like pci, etc)
        if ((status = device_create(&dev, &root_driver, "root", &root_ops)) < 0) {
            printf("devhost: cannot create root device: %d\n", status);
            return -1;
        }
        as_root = true;
    } else if (!strncmp(argv[1], "pci=", 4)) {
        // The pci bus driver launches devhosts for pci devices.
        // Later we'll support other bus driver devhost launching.
        uint32_t index = strtoul(argv[1] + 4, NULL, 10);
        if ((status = devhost_create_pcidev(&dev, index)) < 0) {
            printf("devhost: cannot create pci device: %d\n", status);
            return -1;
        }
    } else {
        printf("devhost: unsupported mode: %s\n", argv[1]);
        return -1;
    }

    if ((status = devhost_device_add_root(dev)) < 0) {
        printf("devhost: cannot install root device: %d\n", status);
        return -1;
    }
    if ((status = devhost_connect(dev, hdevice, hrpc)) < 0) {
        printf("devhost: cannot connect root device: %d\n", status);
        return -1;
    }
    return 0;
}

__EXPORT int devhost_start(void) {
    mxio_dispatcher_run(devhost_rio_dispatcher);
    printf("devhost: rio dispatcher exited?\n");
    return 0;
}
