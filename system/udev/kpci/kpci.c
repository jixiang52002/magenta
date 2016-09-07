// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/pci.h>

#include <hw/pci.h>
#include <magenta/syscalls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <magenta/compiler.h>

#include "kpci-private.h"

// kpci is a driver that communicates with the kernel to publish a list of pci devices.

static mx_device_t* kpci_root_dev;

extern pci_protocol_t _pci_protocol;

static mx_status_t kpci_release(mx_device_t* dev) {
    kpci_device_t* device = get_kpci_device(dev);
    mx_handle_close(device->handle);
    free(device);
    return NO_ERROR;
}

static mx_protocol_device_t kpci_device_proto = {
    .release = kpci_release,
};

extern mx_handle_t root_resource_handle;

static mx_status_t kpci_init_child(mx_driver_t* drv, mx_device_t** out, uint32_t index) {
    mx_pcie_get_nth_info_t info;

    mx_handle_t handle = mx_pci_get_nth_device(root_resource_handle, index, &info);
    if (handle < 0) {
        return handle;
    }

    kpci_device_t* device = calloc(1, sizeof(kpci_device_t));
    if (!device) {
        mx_handle_close(handle);
        return ERR_NO_MEMORY;
    }

    char name[20];
    snprintf(name, sizeof(name), "%02x:%02x:%02x", info.bus_id, info.dev_id, info.func_id);
    device_init(&device->device, drv, name, &kpci_device_proto);

    device->device.protocol_id = MX_PROTOCOL_PCI;
    device->device.protocol_ops = &_pci_protocol;
    device->handle = handle;
    device->index = index;
    *out = &device->device;

    device->props[0] = (mx_device_prop_t){ BIND_PROTOCOL, 0, MX_PROTOCOL_PCI };
    device->props[1] = (mx_device_prop_t){ BIND_PCI_VID, 0, info.vendor_id };
    device->props[2] = (mx_device_prop_t){ BIND_PCI_DID, 0, info.device_id };
    device->props[3] = (mx_device_prop_t){ BIND_PCI_CLASS, 0, info.base_class };
    device->props[4] = (mx_device_prop_t){ BIND_PCI_SUBCLASS, 0, info.sub_class };
    device->props[5] = (mx_device_prop_t){ BIND_PCI_INTERFACE, 0, info.program_interface };
    device->props[6] = (mx_device_prop_t){ BIND_PCI_REVISION, 0, info.revision_id };
    device->device.props = device->props;
    device->device.prop_count = countof(device->props);

    memcpy(&device->info, &info, sizeof(info));

    return NO_ERROR;
}

static mx_status_t kpci_init_children(mx_driver_t* drv, mx_device_t* parent) {
    for (uint32_t index = 0;; index++) {
        mx_device_t* device;
        if (kpci_init_child(drv, &device, index) != NO_ERROR) {
            break;
        }
        device_add(device, parent);
    }

    return NO_ERROR;
}

static mx_status_t kpci_drv_init(mx_driver_t* drv) {
    mx_status_t status;

    if ((status = device_create(&kpci_root_dev, drv, "pci", &kpci_device_proto))) {
        return status;
    }

    // make the pci root non-bindable
    device_set_bindable(kpci_root_dev, false);

    if (device_add(kpci_root_dev, NULL) < 0) {
        free(kpci_root_dev);
        return NO_ERROR;
    } else {
        return kpci_init_children(drv, kpci_root_dev);
    }
}

mx_driver_t _driver_kpci BUILTIN_DRIVER = {
    .name = "pci",
    .ops = {
        .init = kpci_drv_init,
    },
};

mx_status_t devmgr_create_pcidev(mx_device_t** out, uint32_t index) {
    return kpci_init_child(&_driver_kpci, out, index);
}

int devmgr_get_pcidev_index(mx_device_t* dev, uint16_t* vid, uint16_t* did) {
    if (dev->parent == kpci_root_dev) {
        kpci_device_t* pcidev = get_kpci_device(dev);
        *vid = pcidev->info.vendor_id;
        *did = pcidev->info.device_id;
        return (int)pcidev->index;
    } else {
        return -1;
    }
}
