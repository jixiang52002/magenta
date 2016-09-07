// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stddef.h>
#include <stdint.h>

// clang-format off

#define NB_MAGIC              0xAA774217

#define NB_SERVER_PORT        33330
#define NB_ADVERT_PORT        33331

#define NB_COMMAND            1 // arg=0, data=command
#define NB_SEND_FILE          2 // arg=0, data=filename
#define NB_DATA               3 // arg=blocknum, data=data
#define NB_BOOT               4 // arg=0
#define NB_QUERY              5 // arg=0, data=hostname (or "*")
#define NB_SHELL_CMD          6 // arg=0, data=command string
#define NB_OPEN               7 // arg=O_RDONLY|O_WRONLY, data=filename
#define NB_READ               8 // arg=blocknum
#define NB_WRITE              9 // arg=blocknum, data=data
#define NB_CLOSE             10 // arg=0

#define NB_ACK                0 // arg=0 or -err, NB_READ: data=data

#define NB_ADVERTISE          0x77777777

#define NB_ERROR              0x80000000
#define NB_ERROR_BAD_CMD      0x80000001
#define NB_ERROR_BAD_PARAM    0x80000002
#define NB_ERROR_TOO_LARGE    0x80000003

typedef struct nbmsg_t {
    uint32_t magic;
    uint32_t cookie;
    uint32_t cmd;
    uint32_t arg;
    uint8_t  data[0];
} nbmsg;

int netboot_init(void *buf, size_t len);
int netboot_poll(void);

#define DEBUGLOG_PORT         33337
#define DEBUGLOG_ACK_PORT     33338
