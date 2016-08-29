// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddk/device.h>
#include <hw/usb.h>
#include <hw/usb-hub.h>

// protocol data for iotxns
typedef struct usb_protocol_data {
    usb_setup_t setup;      // for control transactions
    uint32_t device_id;
    uint8_t ep_address;     // bEndpointAddress from endpoint descriptor
} usb_protocol_data_t;
