// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/i2c.h>
#include <intel-serialio/reg.h>
#include <magenta/types.h>
#include <mxio/util.h>
#include <magenta/listnode.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#include "controller.h"
#include "slave.h"

// Time out after 2 seconds.
static const uint64_t timeout_ns = 2 * 1000 * 1000 * 1000;

//TODO We should be using interrupts and yielding during long operations, but
//the plumbing isn't all there for that apparently.
#define DO_UNTIL(condition, action)                                           \
    ({                                                                        \
        const uint64_t _wait_for_base_time = mx_current_time();         \
        uint64_t _wait_for_last_time = _wait_for_base_time;                   \
        int _wait_for_condition_value = !!(condition);                        \
        while (!_wait_for_condition_value) {                                  \
               _wait_for_condition_value = !!(condition);                     \
               if (_wait_for_last_time - _wait_for_base_time > timeout_ns)    \
                   break;                                                     \
               _wait_for_last_time = mx_current_time();                 \
               {action;}                                                       \
        }                                                                     \
        _wait_for_condition_value;                                            \
    })

#define WAIT_FOR(condition) DO_UNTIL(condition, )

// Implement the functionality of the i2c slave devices.

static int bus_is_idle(intel_serialio_i2c_device_t *controller) {
    uint32_t i2c_sta = *REG32(&controller->regs->i2c_sta);
    return !(i2c_sta & (0x1 << I2C_STA_CA)) &&
           (i2c_sta & (0x1 << I2C_STA_TFCE));
}

static int stop_detected(intel_serialio_i2c_device_t *controller) {
    return *REG32(&controller->regs->raw_intr_stat) &
           (0x1 << INTR_STOP_DETECTION);
}

static int rx_fifo_empty(intel_serialio_i2c_device_t *controller) {
    return !(*REG32(&controller->regs->i2c_sta) & (0x1 << I2C_STA_RFNE));
}

static mx_status_t intel_serialio_i2c_slave_transfer(
    mx_device_t *dev, i2c_slave_segment_t *segments, int segment_count) {
    mx_status_t status = NO_ERROR;

    intel_serialio_i2c_slave_device_t *slave =
        get_intel_serialio_i2c_slave_device(dev);

    if (!dev->parent) {
        printf("Orphaned I2C slave.\n");
        status = ERR_BAD_STATE;
        goto transfer_finish_2;
    }

    intel_serialio_i2c_device_t* controller =
        get_intel_serialio_i2c_device(dev->parent);

    uint32_t ctl_addr_mode_bit;
    uint32_t tar_add_addr_mode_bit;
    if (slave->chip_address_width == I2C_7BIT_ADDRESS) {
        ctl_addr_mode_bit = CTL_ADDRESSING_MODE_7BIT;
        tar_add_addr_mode_bit = TAR_ADD_WIDTH_7BIT;
    } else if (slave->chip_address_width == I2C_10BIT_ADDRESS) {
        ctl_addr_mode_bit = CTL_ADDRESSING_MODE_10BIT;
        tar_add_addr_mode_bit = TAR_ADD_WIDTH_10BIT;
    } else {
        printf("Bad address width.\n");
        status = ERR_INVALID_ARGS;
        goto transfer_finish_2;
    }

    mtx_lock(&controller->mutex);

    if (!WAIT_FOR(bus_is_idle(controller))) {
        status = ERR_TIMED_OUT;
        goto transfer_finish_1;
    }

    // Set the target adress value and width.
    RMWREG32(&controller->regs->ctl, CTL_ADDRESSING_MODE, 1, ctl_addr_mode_bit);
    *REG32(&controller->regs->tar_add) =
        (tar_add_addr_mode_bit << TAR_ADD_WIDTH) |
        (slave->chip_address << TAR_ADD_IC_TAR);

    // Enable the controller.
    RMWREG32(&controller->regs->i2c_en, I2C_EN_ENABLE, 1, 1);

    int last_read = 0;
    if (segment_count)
        last_read = segments->read;

    while (segment_count--) {
        int len = segments->len;
        uint8_t* buf = segments->buf;

        // If this segment is in the same direction as the last, inject a
        // restart at its start.
        uint32_t restart = 0;
        if (last_read == segments->read)
            restart = 1;
        while (len--) {
            // Build the cmd register value.
            uint32_t cmd = (restart << DATA_CMD_RESTART);
            restart = 0;
            if (!segments->read) {
                while (!(*REG32(&controller->regs->i2c_sta) &
                         (0x1 << I2C_STA_TFNF))) {
                    ;
                }
                cmd |= (*buf << DATA_CMD_DAT);
                cmd |= (DATA_CMD_CMD_WRITE << DATA_CMD_CMD);
            } else {
                cmd |= (DATA_CMD_CMD_READ << DATA_CMD_CMD);
            }
            if (!len && !segment_count)
                cmd |= (0x1 << DATA_CMD_STOP);

            // Write the cmd value.
            *REG32(&controller->regs->data_cmd) = cmd;

            // If this is a read, extract the data.
            if (segments->read) {
                while (!(*REG32(&controller->regs->i2c_sta) &
                         (0x1 << I2C_STA_RFNE))) {
                    ;
                }
                *buf = *REG32(&controller->regs->data_cmd);
            }

            buf++;
        }
        last_read = segments->read;
        segments++;
    }

    // Clear out the stop detect interrupt signal.
    if (!DO_UNTIL(!stop_detected(controller),
                  *REG32(&controller->regs->clr_stop_det))) {
        status = ERR_TIMED_OUT;
        goto transfer_finish_1;
    }

    if (!WAIT_FOR(bus_is_idle(controller))) {
        status = ERR_TIMED_OUT;
        goto transfer_finish_1;
    }

    // Read the data_cmd register to pull data out of the RX FIFO.
    if (!DO_UNTIL(rx_fifo_empty(controller),
                  *REG32(&controller->regs->data_cmd))) {
        status = ERR_TIMED_OUT;
        goto transfer_finish_1;
    }

transfer_finish_1:
    if (status < 0)
        intel_serialio_i2c_reset_controller(controller);
    mtx_unlock(&controller->mutex);
transfer_finish_2:
    return status;
}

// Implement the char protocol for the slave devices.

static ssize_t intel_serialio_i2c_slave_read(
    mx_device_t* dev, void* buf, size_t count, mx_off_t off) {
    i2c_slave_segment_t segment = {
        .read = 1,
        .buf = buf,
        .len = count,
    };
    return intel_serialio_i2c_slave_transfer(dev, &segment, 1);
}

static ssize_t intel_serialio_i2c_slave_write(
    mx_device_t* dev, const void* buf, size_t count, mx_off_t off) {
    i2c_slave_segment_t segment = {
        .read = 0,
        .buf = (void*)buf,
        .len = count,
    };
    return intel_serialio_i2c_slave_transfer(dev, &segment, 1);
}

static ssize_t intel_serialio_i2c_slave_transfer_ioctl(
    mx_device_t* dev, uint32_t op, const void* in_buf, size_t in_len,
    void* out_buf, size_t out_len) {
    mx_status_t status;

    size_t size = in_len;
    size_t read_len = 0;
    size_t write_len = 0;
    int segment_count = 0;
    uintptr_t segment_addr = (uintptr_t)in_buf;
    // Check that the inputs and output buffer are valid.
    while (size) {
        const i2c_slave_ioctl_segment_t* ioctl_segment =
            (const i2c_slave_ioctl_segment_t*)segment_addr;
        size_t base_size = offsetof(i2c_slave_ioctl_segment_t, buf);
        int len = ioctl_segment->len;
        if (size < base_size) {
            status = ERR_INVALID_ARGS;
            goto slave_transfer_ioctl_finish_2;
        }

        size_t consumed = 0;
        if (ioctl_segment->read) {
            read_len += len;
            consumed = base_size;
        } else {
            write_len += len;
            consumed = base_size + len;
        }
        if (consumed > size) {
            status = ERR_INVALID_ARGS;
            goto slave_transfer_ioctl_finish_2;
        }
        segment_addr += consumed;
        size -= consumed;
        segment_count++;
    }
    if (out_len < read_len) {
        status = ERR_INVALID_ARGS;
        goto slave_transfer_ioctl_finish_2;
    }

    if (in_len < write_len) {
        status = ERR_INVALID_ARGS;
        goto slave_transfer_ioctl_finish_2;
    }

    // Build a list of segments to transfer.
    i2c_slave_segment_t* segments =
        calloc(segment_count, sizeof(*segments));
    if (!segments) {
        status = ERR_NO_MEMORY;
        goto slave_transfer_ioctl_finish_2;
    }
    i2c_slave_segment_t* cur_segment = segments;
    segment_addr = (uintptr_t)in_buf;
    uintptr_t out_addr = (uintptr_t)out_buf;
    size = in_len;
    while (size) {
        const i2c_slave_ioctl_segment_t* ioctl_segment =
            (const i2c_slave_ioctl_segment_t*)segment_addr;
        const size_t base_size =
            offsetof(i2c_slave_ioctl_segment_t, buf);
        int len = ioctl_segment->len;

        size_t consumed = 0;
        if (ioctl_segment->read) {
            consumed = base_size;
            cur_segment->read = 1;
            cur_segment->len = len;
            cur_segment->buf = (uint8_t*)out_addr;
            out_addr += len;
        } else {
            consumed = base_size + len;
            cur_segment->read = 0;
            cur_segment->len = len;
            cur_segment->buf = (uint8_t*)(segment_addr + base_size);
        }

        cur_segment++;
        segment_addr += consumed;
        size -= consumed;
    }

    status = intel_serialio_i2c_slave_transfer(dev, segments, segment_count);
    if (status == NO_ERROR)
        status = read_len;

slave_transfer_ioctl_finish_1:
    free(segments);
slave_transfer_ioctl_finish_2:
    return status;
}

static ssize_t intel_serialio_i2c_slave_ioctl(
    mx_device_t* dev, uint32_t op, const void* in_buf, size_t in_len,
    void* out_buf, size_t out_len) {
    switch (op) {
    case IOCTL_I2C_SLAVE_TRANSFER:
        return intel_serialio_i2c_slave_transfer_ioctl(
            dev, op, in_buf, in_len, out_buf, out_len);
        break;
    default:
        return ERR_INVALID_ARGS;
    }
}

// Implement the device protocol for the slave devices.

static mx_protocol_device_t intel_serialio_i2c_slave_device_proto = {
    .read = &intel_serialio_i2c_slave_read,
    .write = &intel_serialio_i2c_slave_write,
    .ioctl = &intel_serialio_i2c_slave_ioctl,
};

// Initialize a slave device structure.

mx_status_t intel_serialio_i2c_slave_device_init(
    mx_device_t* cont, intel_serialio_i2c_slave_device_t* slave,
    uint8_t width, uint16_t address) {
    mx_status_t status = NO_ERROR;

    char name[sizeof(address) * 2 + 2] = {
            [sizeof(name) - 1] = '\0',
    };
    snprintf(name, sizeof(name) - 1, "%04x", address);

    device_init(&slave->device, cont->driver, name,
                &intel_serialio_i2c_slave_device_proto);

    slave->chip_address_width = width;
    slave->chip_address = address;

    return status;
}
