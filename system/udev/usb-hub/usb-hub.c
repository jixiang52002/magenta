// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/completion.h>
#include <ddk/common/usb.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/usb-bus.h>
#include <hw/usb-hub.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <magenta/listnode.h>
#include <threads.h>

//#define TRACE 1
#if TRACE
#define xprintf(fmt...) printf(fmt)
#else
#define xprintf(fmt...) \
    do {                \
    } while (0)
#endif

typedef struct usb_hub {
    // the device we are publishing
    mx_device_t device;

    // Underlying USB device
    mx_device_t* usb_device;

    mx_device_t* bus_device;
    usb_bus_protocol_t* bus_protocol;

    usb_speed_t hub_speed;
    int num_ports;
    // delay after port power in microseconds
    mx_time_t power_on_delay;

    iotxn_t* status_request;
    completion_t completion;
} usb_hub_t;
#define get_hub(dev) containerof(dev, usb_hub_t, device)

static mx_status_t usb_hub_get_port_status(usb_hub_t* hub, int port, usb_port_status_t* status) {
    mx_status_t result = usb_get_status(hub->usb_device, USB_RECIP_PORT, port, status, sizeof(*status));
    if (result == sizeof(*status)) {
        return NO_ERROR;
    } else {
        return -1;
    }
}

static usb_speed_t usb_hub_get_port_speed(usb_hub_t* hub, int port) {
    if (hub->hub_speed == USB_SPEED_SUPER)
        return USB_SPEED_SUPER;

    usb_port_status_t status;
    if (usb_hub_get_port_status(hub, port, &status) == NO_ERROR) {
        if (status.wPortStatus & USB_PORT_LOW_SPEED)
            return USB_SPEED_LOW;
        if (status.wPortStatus & USB_PORT_HIGH_SPEED)
            return USB_SPEED_HIGH;
        return USB_SPEED_FULL;
    } else {
        return USB_SPEED_UNDEFINED;
    }
}

static void usb_hub_interrupt_complete(iotxn_t* txn, void* cookie) {
    xprintf("usb_hub_interrupt_complete got %d %lld\n", txn->status, txn->actual);
    usb_hub_t* hub = (usb_hub_t*)cookie;
    completion_signal(&hub->completion);
}

static void usb_hub_enable_port(usb_hub_t* hub, int port) {
    usb_set_feature(hub->usb_device, USB_RECIP_PORT, USB_FEATURE_PORT_POWER, port);
    usleep(hub->power_on_delay);
}

static void usb_hub_port_connected(usb_hub_t* hub, int port) {
    // USB 2.0 spec section 7.1.7.3 recommends 100ms between connect and reset
    usleep(100 * 1000);

    xprintf("port %d usb_hub_port_connected\n", port);
    usb_set_feature(hub->usb_device, USB_RECIP_PORT, USB_FEATURE_PORT_RESET, port);
}

static void usb_hub_port_disconnected(usb_hub_t* hub, int port) {
    xprintf("port %d usb_hub_port_disconnected\n", port);
    hub->bus_protocol->hub_device_removed(hub->bus_device, hub->usb_device, port);
}

static void usb_hub_port_reset(usb_hub_t* hub, int port) {
    // USB 2.0 spec section 9.1.2 recommends 100ms delay before enumerating
    usleep(100 * 1000);

    xprintf("port %d usb_hub_port_reset\n", port);
    usb_speed_t speed = usb_hub_get_port_speed(hub, port);
    if (speed != USB_SPEED_UNDEFINED) {
        xprintf("calling device_added(%d %d)\n", port, speed);
        hub->bus_protocol->hub_device_added(hub->bus_device, hub->usb_device, port, speed);
    }
}

static void usb_hub_handle_port_status(usb_hub_t* hub, int port, usb_port_status_t* status) {
    if (status->wPortChange & USB_PORT_CONNECTION) {
        usb_clear_feature(hub->usb_device, USB_RECIP_PORT, USB_FEATURE_C_PORT_CONNECTION, port);
        if (status->wPortStatus & USB_PORT_CONNECTION) {
            usb_hub_port_connected(hub, port);
        } else {
            usb_hub_port_disconnected(hub, port);
        }
    }
    if (status->wPortChange & USB_PORT_ENABLE) {
        xprintf("USB_PORT_ENABLE\n");
        usb_clear_feature(hub->usb_device, USB_RECIP_PORT, USB_FEATURE_C_PORT_ENABLE, port);
    }
    if (status->wPortChange & USB_PORT_SUSPEND) {
        xprintf("USB_PORT_SUSPEND\n");
        usb_clear_feature(hub->usb_device, USB_RECIP_PORT, USB_FEATURE_C_PORT_SUSPEND, port);
    }
    if (status->wPortChange & USB_PORT_OVER_CURRENT) {
        xprintf("USB_PORT_OVER_CURRENT\n");
        usb_clear_feature(hub->usb_device, USB_RECIP_PORT, USB_FEATURE_C_PORT_OVER_CURRENT, port);
    }
    if (status->wPortChange & USB_PORT_RESET) {
        usb_clear_feature(hub->usb_device, USB_RECIP_PORT, USB_FEATURE_C_PORT_RESET, port);
        if (!(status->wPortStatus & USB_PORT_RESET)) {
            usb_hub_port_reset(hub, port);
        }
    }
}

static void usb_hub_unbind(mx_device_t* device) {
    usb_hub_t* hub = get_hub(device);
    device_remove(&hub->device);
}

static mx_status_t usb_hub_release(mx_device_t* device) {
    usb_hub_t* hub = get_hub(device);
    hub->status_request->ops->release(hub->status_request);
    free(hub);
    return NO_ERROR;
}

static mx_protocol_device_t usb_hub_device_proto = {
    .unbind = usb_hub_unbind,
    .release = usb_hub_release,
};

static int usb_hub_thread(void* arg) {
    usb_hub_t* hub = (usb_hub_t*)arg;
    iotxn_t* txn = hub->status_request;

    usb_hub_descriptor_t desc;
    int desc_type = (hub->hub_speed == USB_SPEED_SUPER ? USB_HUB_DESC_TYPE_SS : USB_HUB_DESC_TYPE);
    mx_status_t result = usb_get_descriptor(hub->usb_device, USB_TYPE_CLASS | USB_RECIP_DEVICE,
                                            desc_type, 0, &desc, sizeof(desc));
    if (result < 0) {
        printf("get hub descriptor failed: %d\n", result);
        return result;
    }

    result = hub->bus_protocol->configure_hub(hub->bus_device, hub->usb_device, hub->hub_speed, &desc);
    if (result < 0) {
        printf("configure_hub failed: %d\n", result);
        return result;
    }

    int num_ports = desc.bNbrPorts;
    // power on delay in microseconds
    hub->power_on_delay = desc.bPowerOn2PwrGood * 2 * 1000;
    if (hub->power_on_delay < 100 * 1000) {
        // USB 2.0 spec section 9.1.2 recommends atleast 100ms delay after power on
        hub->power_on_delay = 100 * 1000;
    }

    for (int i = 1; i <= num_ports; i++) {
        usb_hub_enable_port(hub, i);
    }

    device_set_bindable(&hub->device, false);
    result = device_add(&hub->device, hub->usb_device);
    if (result != NO_ERROR) {
        usb_hub_release(&hub->device);
        return result;
    }

    // bit field for port status bits
    uint8_t status_buf[128 / 8];
    memset(status_buf, 0, sizeof(status_buf));

    // This loop handles events from our interrupt endpoint
    while (1) {
        completion_reset(&hub->completion);
        iotxn_queue(hub->usb_device, txn);
        completion_wait(&hub->completion, MX_TIME_INFINITE);
        if (txn->status != NO_ERROR) {
            break;
        }

        txn->ops->copyfrom(txn, status_buf, txn->actual, 0);
        uint8_t* bitmap = status_buf;
        uint8_t* bitmap_end = bitmap + txn->actual;

        // bit zero is hub status
        if (bitmap[0] & 1) {
            // what to do here?
            printf("usb_hub_interrupt_complete hub status changed\n");
        }

        int port = 1;
        int bit = 1;
        while (bitmap < bitmap_end && port <= num_ports) {
            if (*bitmap & (1 << bit)) {
                usb_port_status_t status;
                mx_status_t result = usb_hub_get_port_status(hub, port, &status);
                if (result == NO_ERROR) {
                    usb_hub_handle_port_status(hub, port, &status);
                }
            }
            port++;
            if (++bit == 8) {
                bitmap++;
                bit = 0;
            }
        }
    }

    return NO_ERROR;
}

static mx_status_t usb_hub_bind(mx_driver_t* driver, mx_device_t* device) {
    mx_device_t* bus_device = device->parent;
    usb_bus_protocol_t* bus_protocol;
    if (device_get_protocol(bus_device, MX_PROTOCOL_USB_BUS, (void**)&bus_protocol)) {
        printf("usb_hub_bind could not find bus device\n");
        return ERR_NOT_SUPPORTED;
    }

    // find our interrupt endpoint
    usb_desc_iter_t iter;
    mx_status_t result = usb_desc_iter_init(device, &iter);
    if (result < 0) return result;

    usb_interface_descriptor_t* intf = usb_desc_iter_next_interface(&iter, true);
    if (!intf || intf->bNumEndpoints != 1) {
        usb_desc_iter_release(&iter);
        return ERR_NOT_SUPPORTED;
    }

    uint8_t ep_addr = 0;
    uint16_t max_packet_size = 0;
    usb_endpoint_descriptor_t* endp = usb_desc_iter_next_endpoint(&iter);
    if (endp && usb_ep_type(endp) == USB_ENDPOINT_INTERRUPT) {
        ep_addr = endp->bEndpointAddress;
        max_packet_size = usb_ep_max_packet(endp);
    }
    usb_desc_iter_release(&iter);

    if (!ep_addr) {
        return ERR_NOT_SUPPORTED;
    }

    usb_hub_t* hub = calloc(1, sizeof(usb_hub_t));
    if (!hub) {
        printf("Not enough memory for usb_hub_t.\n");
        return ERR_NO_MEMORY;
    }

    device_init(&hub->device, driver, "usb-hub", &usb_hub_device_proto);

    hub->usb_device = device;
    hub->hub_speed = usb_get_speed(device);
    hub->bus_device = bus_device;
    hub->bus_protocol = bus_protocol;

    mx_status_t status;
    iotxn_t* txn = usb_alloc_iotxn(ep_addr, max_packet_size, 0);
    if (!txn) {
        status = ERR_NO_MEMORY;
        goto fail;
    }
    txn->length = max_packet_size;
    txn->complete_cb = usb_hub_interrupt_complete;
    txn->cookie = hub;
    hub->status_request = txn;

    thrd_t thread;
    int ret = thrd_create_with_name(&thread, usb_hub_thread, hub, "usb_hub_thread");
    if (ret != thrd_success) {
        goto fail;
    }
    thrd_detach(thread);
    return NO_ERROR;

fail:
    if (txn) {
        txn->ops->release(txn);
    }
    free(hub);
    return status;
}

static mx_bind_inst_t binding[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, MX_PROTOCOL_USB_DEVICE),
    BI_MATCH_IF(EQ, BIND_USB_CLASS, USB_CLASS_HUB),
};

mx_driver_t _driver_usb_hub BUILTIN_DRIVER = {
    .name = "usb-hub",
    .ops = {
        .bind = usb_hub_bind,
    },
    .binding = binding,
    .binding_size = sizeof(binding),
};
