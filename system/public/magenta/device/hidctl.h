// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <magenta/device/ioctl.h>

#define IOCTL_HID_CTL_CONFIG \
    IOCTL(IOCTL_KIND_DEFAULT, IOCTL_FAMILY_HID, 0)

typedef struct hid_ioctl_config {
    uint8_t dev_num;
    bool boot_device;
    uint8_t dev_class;
    size_t rpt_desc_len;
    uint8_t rpt_desc[];
} hid_ioctl_config_t;
