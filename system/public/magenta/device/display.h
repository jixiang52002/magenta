// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>
#include <magenta/device/ioctl.h>

#define MX_DISPLAY_FLAG_HW_FRAMEBUFFER (1 << 0)

typedef struct mx_display_info {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixelsize;
    uint32_t flags;
} mx_display_info_t;

#define IOCTL_DISPLAY_GET_FB \
    IOCTL(IOCTL_KIND_GET_HANDLE, IOCTL_FAMILY_DISPLAY, 1)
#define IOCTL_DISPLAY_FLUSH_FB \
    IOCTL(IOCTL_KIND_DEFAULT, IOCTL_FAMILY_DISPLAY, 2)
#define IOCTL_DISPLAY_FLUSH_FB_REGION \
    IOCTL(IOCTL_KIND_DEFAULT, IOCTL_FAMILY_DISPLAY, 3)

typedef struct {
    mx_handle_t vmo;
    mx_display_info_t info;
} ioctl_display_get_fb_t;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} ioctl_display_region_t;

