// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/common/hid.h>
#include <ddk/protocol/input.h>
#include <hw/inout.h>

#include <magenta/syscalls.h>
#include <magenta/syscalls-ddk.h>
#include <magenta/types.h>

#include <hid/usages.h>

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include <magenta/listnode.h>

#define MXDEBUG 0
#include <mxio/debug.h>

typedef struct i8042_device {
    mx_device_t device;
    mx_driver_t* drv;

    mx_handle_t irq;
    thrd_t irq_thread;

    int last_code;

    int type;
    union {
        boot_kbd_report_t kbd;
        boot_mouse_report_t mouse;
    } report;

    // list of opened devices
    struct list_node instance_list;
    mtx_t instance_lock;
} i8042_device_t;

typedef struct i8042_instance {
    mx_device_t device;
    i8042_device_t *root;

    mx_hid_fifo_t fifo;
    struct list_node node;
} i8042_instance_t;

#define foreach_instance(root, instance) \
    list_for_every_entry(&root->instance_list, instance, i8042_instance_t, node)

static inline bool is_kbd_modifier(uint8_t usage) {
    return (usage >= HID_USAGE_KEY_LEFT_CTRL && usage <= HID_USAGE_KEY_RIGHT_GUI);
}

#define MOD_SET 1
#define MOD_EXISTS 2
#define MOD_ROLLOVER 3
static int i8042_modifier_key(i8042_device_t* dev, uint8_t mod, bool down) {
    int bit = mod - HID_USAGE_KEY_LEFT_CTRL;
    if (bit < 0 || bit > 7) return MOD_ROLLOVER;
    if (down) {
        if (dev->report.kbd.modifier & 1 << bit) {
            return MOD_EXISTS;
        } else {
            dev->report.kbd.modifier |= 1 << bit;
        }
    } else {
        dev->report.kbd.modifier &= ~(1 << bit);
    }
    return MOD_SET;
}

#define KEY_ADDED 1
#define KEY_EXISTS 2
#define KEY_ROLLOVER 3
static int i8042_add_key(i8042_device_t* dev, uint8_t usage) {
    for (int i = 0; i < 6; i++) {
        if (dev->report.kbd.usage[i] == usage) return KEY_EXISTS;
        if (dev->report.kbd.usage[i] == 0) {
            dev->report.kbd.usage[i] = usage;
            return KEY_ADDED;
        }
    }
    return KEY_ROLLOVER;
}

#define KEY_REMOVED 1
#define KEY_NOT_FOUND 2
static int i8042_rm_key(i8042_device_t* dev, uint8_t usage) {
    int idx = -1;
    for (int i = 0; i < 6; i++) {
        if (dev->report.kbd.usage[i] == usage) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return KEY_NOT_FOUND;
    for (int i = idx; i < 5; i++) {
        dev->report.kbd.usage[i] = dev->report.kbd.usage[i+1];
    }
    dev->report.kbd.usage[5] = 0;
    return KEY_REMOVED;
}

#define get_i8042_device(dev) containerof(dev, i8042_device_t, device)
#define get_i8042_instance(dev) containerof(dev, i8042_instance_t, device)

#define I8042_COMMAND_REG 0x64
#define I8042_STATUS_REG 0x64
#define I8042_DATA_REG 0x60

#define ISA_IRQ_KEYBOARD 0x1
#define ISA_IRQ_MOUSE 0x0c

static inline int i8042_read_data(void) {
    return inp(I8042_DATA_REG);
}

static inline int i8042_read_status(void) {
    return inp(I8042_STATUS_REG);
}

static inline void i8042_write_data(int val) {
    outp(I8042_DATA_REG, val);
}

static inline void i8042_write_command(int val) {
    outp(I8042_COMMAND_REG, val);
}

/*
 * timeout in milliseconds
 */
#define I8042_CTL_TIMEOUT 500

/*
 * status register bits
 */
#define I8042_STR_PARITY 0x80
#define I8042_STR_TIMEOUT 0x40
#define I8042_STR_AUXDATA 0x20
#define I8042_STR_KEYLOCK 0x10
#define I8042_STR_CMDDAT 0x08
#define I8042_STR_MUXERR 0x04
#define I8042_STR_IBF 0x02
#define I8042_STR_OBF 0x01

/*
 * control register bits
 */
#define I8042_CTR_KBDINT 0x01
#define I8042_CTR_AUXINT 0x02
#define I8042_CTR_IGNKEYLK 0x08
#define I8042_CTR_KBDDIS 0x10
#define I8042_CTR_AUXDIS 0x20
#define I8042_CTR_XLATE 0x40

/*
 * commands
 */
#define I8042_CMD_CTL_RCTR 0x0120
#define I8042_CMD_CTL_WCTR 0x1060
#define I8042_CMD_CTL_TEST 0x01aa
#define I8042_CMD_CTL_AUX  0x00d4

// Identity response will be ACK + 0, 1, or 2 bytes
#define I8042_CMD_IDENTIFY 0x03f2
#define I8042_CMD_SCAN_DIS 0x01f5
#define I8042_CMD_SCAN_EN 0x01f4

#define I8042_CMD_CTL_KBD_DIS 0x00ad
#define I8042_CMD_CTL_KBD_EN 0x00ae
#define I8042_CMD_CTL_KBD_TEST 0x01ab
#define I8042_CMD_KBD_MODE 0x01f0

#define I8042_CMD_CTL_MOUSE_DIS 0x00a7
#define I8042_CMD_CTL_MOUSE_EN 0x00a8
#define I8042_CMD_CTL_MOUSE_TEST 0x01a9

/*
 * used for flushing buffers. the i8042 internal buffer shoudn't exceed this.
 */
#define I8042_BUFFER_LENGTH 32

static const uint8_t kbd_hid_report_desc[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x05, 0x07,  //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,  //   Usage Minimum (0xE0)
    0x29, 0xE7,  //   Usage Maximum (0xE7)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x05,  //   Report Count (5)
    0x75, 0x01,  //   Report Size (1)
    0x05, 0x08,  //   Usage Page (LEDs)
    0x19, 0x01,  //   Usage Minimum (Num Lock)
    0x29, 0x05,  //   Usage Maximum (Kana)
    0x91, 0x02,  //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x03,  //   Report Size (3)
    0x91, 0x01,  //   Output (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x05, 0x07,  //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,  //   Usage Minimum (0x00)
    0x29, 0x65,  //   Usage Maximum (0x65)
    0x81, 0x00,  //   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,        // End Collection
};

static const uint8_t pc_set1_usage_map[128] = {
    /* 0x00 */ 0, HID_USAGE_KEY_ESC, HID_USAGE_KEY_1, HID_USAGE_KEY_2,
    /* 0x04 */ HID_USAGE_KEY_3, HID_USAGE_KEY_4, HID_USAGE_KEY_5, HID_USAGE_KEY_6,
    /* 0x08 */ HID_USAGE_KEY_7, HID_USAGE_KEY_8, HID_USAGE_KEY_9, HID_USAGE_KEY_0,
    /* 0x0c */ HID_USAGE_KEY_MINUS, HID_USAGE_KEY_EQUAL, HID_USAGE_KEY_BACKSPACE, HID_USAGE_KEY_TAB,
    /* 0x10 */ HID_USAGE_KEY_Q, HID_USAGE_KEY_W, HID_USAGE_KEY_E, HID_USAGE_KEY_R,
    /* 0x14 */ HID_USAGE_KEY_T, HID_USAGE_KEY_Y, HID_USAGE_KEY_U, HID_USAGE_KEY_I,
    /* 0x18 */ HID_USAGE_KEY_O, HID_USAGE_KEY_P, HID_USAGE_KEY_LEFTBRACE, HID_USAGE_KEY_RIGHTBRACE,
    /* 0x1c */ HID_USAGE_KEY_ENTER, HID_USAGE_KEY_LEFT_CTRL, HID_USAGE_KEY_A, HID_USAGE_KEY_S,
    /* 0x20 */ HID_USAGE_KEY_D, HID_USAGE_KEY_F, HID_USAGE_KEY_G, HID_USAGE_KEY_H,
    /* 0x24 */ HID_USAGE_KEY_J, HID_USAGE_KEY_K, HID_USAGE_KEY_L, HID_USAGE_KEY_SEMICOLON,
    /* 0x28 */ HID_USAGE_KEY_APOSTROPHE, HID_USAGE_KEY_GRAVE, HID_USAGE_KEY_LEFT_SHIFT, HID_USAGE_KEY_BACKSLASH,
    /* 0x2c */ HID_USAGE_KEY_Z, HID_USAGE_KEY_X, HID_USAGE_KEY_C, HID_USAGE_KEY_V,
    /* 0x30 */ HID_USAGE_KEY_B, HID_USAGE_KEY_N, HID_USAGE_KEY_M, HID_USAGE_KEY_COMMA,
    /* 0x34 */ HID_USAGE_KEY_DOT, HID_USAGE_KEY_SLASH, HID_USAGE_KEY_RIGHT_SHIFT, HID_USAGE_KEY_KP_ASTERISK,
    /* 0x38 */ HID_USAGE_KEY_LEFT_ALT, HID_USAGE_KEY_SPACE, HID_USAGE_KEY_CAPSLOCK, HID_USAGE_KEY_F1,
    /* 0x3c */ HID_USAGE_KEY_F2, HID_USAGE_KEY_F3, HID_USAGE_KEY_F4, HID_USAGE_KEY_F5,
    /* 0x40 */ HID_USAGE_KEY_F6, HID_USAGE_KEY_F7, HID_USAGE_KEY_F8, HID_USAGE_KEY_F9,
    /* 0x44 */ HID_USAGE_KEY_F10, HID_USAGE_KEY_NUMLOCK, HID_USAGE_KEY_SCROLLLOCK, HID_USAGE_KEY_KP_7,
    /* 0x48 */ HID_USAGE_KEY_KP_8, HID_USAGE_KEY_KP_9, HID_USAGE_KEY_KP_MINUS, HID_USAGE_KEY_KP_4,
    /* 0x4c */ HID_USAGE_KEY_KP_5, HID_USAGE_KEY_KP_6, HID_USAGE_KEY_KP_PLUS, HID_USAGE_KEY_KP_1,
    /* 0x50 */ HID_USAGE_KEY_KP_2, HID_USAGE_KEY_KP_3, HID_USAGE_KEY_KP_0, HID_USAGE_KEY_KP_DOT,
    /* 0x54 */ 0, 0, 0, HID_USAGE_KEY_F11,
    /* 0x58 */ HID_USAGE_KEY_F12, 0, 0, 0,
};

static const uint8_t pc_set1_usage_map_e0[128] = {
    /* 0x00 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x08 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x10 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x18 */ 0, 0, 0, 0, HID_USAGE_KEY_KP_ENTER, HID_USAGE_KEY_RIGHT_CTRL, 0, 0,
    /* 0x20 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x28 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x30 */ 0, 0, 0, 0, 0, HID_USAGE_KEY_KP_SLASH, 0, HID_USAGE_KEY_PRINTSCREEN,
    /* 0x38 */ HID_USAGE_KEY_RIGHT_ALT, 0, 0, 0, 0, 0, 0, 0,
    /* 0x40 */ 0, 0, 0, 0, 0, 0, 0, HID_USAGE_KEY_HOME,
    /* 0x48 */ HID_USAGE_KEY_UP, HID_USAGE_KEY_PAGEUP, 0, HID_USAGE_KEY_LEFT, 0, HID_USAGE_KEY_RIGHT, 0, HID_USAGE_KEY_END,
    /* 0x50 */ HID_USAGE_KEY_DOWN, HID_USAGE_KEY_PAGEDOWN, HID_USAGE_KEY_INSERT, 0, 0, 0, 0, 0,
    /* 0x58 */ 0, 0, 0, HID_USAGE_KEY_LEFT_GUI, HID_USAGE_KEY_RIGHT_GUI, 0 /* MENU */, 0, 0,
};

static const uint8_t mouse_hid_report_desc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (0x01)
    0x29, 0x03,        //     Usage Maximum (0x03)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x01,        //     Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x81,        //     Logical Minimum (129)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x06,        //     Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

static int i8042_wait_read(void) {
    int i = 0;
    while ((~i8042_read_status() & I8042_STR_OBF) && (i < I8042_CTL_TIMEOUT)) {
        usleep(10);
        i++;
    }
    return -(i == I8042_CTL_TIMEOUT);
}

static int i8042_wait_write(void) {
    int i = 0;
    while ((i8042_read_status() & I8042_STR_IBF) && (i < I8042_CTL_TIMEOUT)) {
        usleep(10);
        i++;
    }
    return -(i == I8042_CTL_TIMEOUT);
}

static int i8042_flush(void) {
    unsigned char data __UNUSED;
    int i = 0;

    while ((i8042_read_status() & I8042_STR_OBF) && (i++ < I8042_BUFFER_LENGTH)) {
        usleep(10);
        data = i8042_read_data();
    }

    return i;
}

static int i8042_command_data(uint8_t* param, int command) {
    int retval = 0, i = 0;

    for (i = 0; i < ((command >> 12) & 0xf); i++) {
        if ((retval = i8042_wait_write())) {
            break;
        }

        i8042_write_data(param[i]);
    }

    int expected = (command >> 8) & 0xf;
    if (!retval) {
        for (i = 0; i < expected; i++) {
            if ((retval = i8042_wait_read())) {
                xprintf("i8042: timeout reading; got %d bytes\n", i);
                return i;
            }

            // TODO: do we need to distinguish keyboard and aux data?
            param[i] = i8042_read_data();
        }
    }

    return retval ? retval : expected;
}

static int i8042_command(uint8_t* param, int command) {
    xprintf("i8042 ctl command 0x%04x\n", command & 0xffff);
    int retval = 0;

    retval = i8042_wait_write();
    if (!retval) {
        i8042_write_command(command & 0xff);
    }

    if (!retval) {
        retval = i8042_command_data(param, command);
    }

    return retval;
}

static int i8042_selftest(void) {
    uint8_t param;
    int i = 0;
    do {
        if (i8042_command(&param, I8042_CMD_CTL_TEST) < 0) {
            return -1;
        }
        if (param == 0x55)
            return 0;
        usleep(50 * 1000);
    } while (i++ < 5);
    return -1;
}

static int i8042_dev_command(uint8_t* param, int command) {
    xprintf("i8042 dev command 0x%04x\n", command & 0xffff);
    int retval = 0;

    retval = i8042_wait_write();
    if (!retval) {
        i8042_write_data(command & 0xff);
    }

    if (!retval) {
        retval = i8042_command_data(param, command);
    }

    return retval;
}

static int i8042_aux_command(uint8_t* param, int command) {
    xprintf("i8042 aux command\n");
    int retval = 0;

    retval = i8042_wait_write();
    if (!retval) {
        i8042_write_command(I8042_CMD_CTL_AUX);
    }

    if (!retval) {
        return i8042_dev_command(param, command);
    }

    return retval;
}

static void i8042_process_scode(i8042_device_t* dev, uint8_t scode, unsigned int flags) {
    // is this a multi code sequence?
    bool multi = (dev->last_code == 0xe0);

    // update the last received code
    dev->last_code = scode;

    // save the key up event bit
    bool key_up = !!(scode & 0x80);
    scode &= 0x7f;

    // translate the key based on our translation table
    uint8_t usage;
    if (multi) {
        usage = pc_set1_usage_map_e0[scode];
    } else {
        usage = pc_set1_usage_map[scode];
    }
    if (!usage) return;

    bool rollover = false;
    if (is_kbd_modifier(usage)) {
        switch (i8042_modifier_key(dev, usage, !key_up)) {
        case MOD_EXISTS:
            return;
        case MOD_ROLLOVER:
            rollover = true;
            break;
        case MOD_SET:
        default:
            break;
        }
    } else if (key_up) {
        if (i8042_rm_key(dev, usage) != KEY_REMOVED) {
            rollover = true;
        }
    } else {
        switch (i8042_add_key(dev, usage)) {
        case KEY_EXISTS:
            return;
        case KEY_ROLLOVER:
            rollover = true;
            break;
        case KEY_ADDED:
        default:
            break;
        }
    }

    //cprintf("i8042: scancode=0x%x, keyup=%u, multi=%u: usage=0x%x\n", scode, !!key_up, multi, usage);

    const boot_kbd_report_t* report = rollover ? &report_err_rollover : &dev->report.kbd;
    i8042_instance_t* instance;
    mtx_lock(&dev->instance_lock);
    foreach_instance(dev, instance) {
        mtx_lock(&instance->fifo.lock);
        bool set_readable = (mx_hid_fifo_size(&instance->fifo) == 0);
        mx_hid_fifo_write(&instance->fifo, (uint8_t*)report, sizeof(*report));
        if (set_readable) {
            device_state_set(&instance->device, DEV_STATE_READABLE);
        }
        mtx_unlock(&instance->fifo.lock);
    }
    mtx_unlock(&dev->instance_lock);
}

static void i8042_process_mouse(i8042_device_t* dev, uint8_t data, unsigned int flags) {
    switch (dev->last_code) {
    case 0:
        if (!(data & 0x08)) {
            // The first byte always has bit 3 set, so skip this packet.
            return;
        }
        dev->report.mouse.buttons = data;
        break;
    case 1: {
        int state = dev->report.mouse.buttons;
        int d = data;
        dev->report.mouse.rel_x = d - ((state << 4) & 0x100);
        break;
        }
    case 2: {
        int state = dev->report.mouse.buttons;
        int d = data;
        dev->report.mouse.rel_y = d - ((state << 3) & 0x100);
        dev->report.mouse.buttons &= 0x7;

        i8042_instance_t* instance;
        mtx_lock(&dev->instance_lock);
        foreach_instance(dev, instance) {
            mtx_lock(&instance->fifo.lock);
            bool set_readable = (mx_hid_fifo_size(&instance->fifo) == 0);
            mx_hid_fifo_write(&instance->fifo, (uint8_t*)&dev->report.mouse, sizeof(dev->report.mouse));
            if (set_readable) {
                device_state_set(&instance->device, DEV_STATE_READABLE);
            }
            mtx_unlock(&instance->fifo.lock);
        }
        mtx_unlock(&dev->instance_lock);
        memset(&dev->report.mouse, 0, sizeof(dev->report.mouse));
        break;
        }
    }
    dev->last_code = (dev->last_code + 1) % 3;
}

static int i8042_irq_thread(void* arg) {
    i8042_device_t* device = (i8042_device_t*)arg;

    // enable I/O port access
    // TODO
    mx_status_t status;
    status = mx_mmap_device_io(I8042_COMMAND_REG, 1);
    if (status)
        return 0;
    status = mx_mmap_device_io(I8042_DATA_REG, 1);
    if (status)
        return 0;

    for (;;) {
        status = mx_interrupt_event_wait(device->irq);
        if (status == NO_ERROR) {
            // ack IRQ so we don't lose any IRQs that arrive while processing
            // (as this is an edge-triggered IRQ)
            mx_interrupt_event_complete(device->irq);

            // keep handling status on the controller until no bits are set we care about
            bool retry;
            do {
                retry = false;

                uint8_t str = i8042_read_status();

                // check for incoming data from the controller
                // TODO: deal with potential race between IRQ1 and IRQ12
                if (str & I8042_STR_OBF) {
                    uint8_t data = i8042_read_data();
                    // TODO: should we check (str & I8042_STR_AUXDATA) before
                    // handling this byte?
                    if (device->type == INPUT_PROTO_KBD) {
                        i8042_process_scode(device, data,
                                            ((str & I8042_STR_PARITY) ? I8042_STR_PARITY : 0) |
                                                ((str & I8042_STR_TIMEOUT) ? I8042_STR_TIMEOUT : 0));
                    } else if (device->type == INPUT_PROTO_MOUSE) {
                        i8042_process_mouse(device, data, 0);
                    }
                    retry = true;
                }
                // TODO check other status bits here
            } while (retry);
        }
    }
    return 0;
}

static ssize_t i8042_read(mx_device_t* dev, void* buf, size_t count, mx_off_t off) {
    i8042_instance_t* instance = get_i8042_instance(dev);
    i8042_device_t* root = instance->root;

    size_t size = (root->type == INPUT_PROTO_KBD) ? sizeof(boot_kbd_report_t) : sizeof(boot_mouse_report_t);
    if (count < size || (count % size != 0))
        return ERR_INVALID_ARGS;

    uint8_t* data = buf;
    mtx_lock(&instance->fifo.lock);
    while (count > 0) {
        if (mx_hid_fifo_read(&instance->fifo, data, size) < (ssize_t)size)
            break;
        data += size;
        count -= size;
    }
    if (mx_hid_fifo_size(&instance->fifo) == 0) {
        device_state_clr(dev, DEV_STATE_READABLE);
    }
    mtx_unlock(&instance->fifo.lock);
    return data - (uint8_t*)buf;
}

static ssize_t i8042_ioctl(mx_device_t* dev, uint32_t op, const void* in_buf, size_t in_len,
                           void* out_buf, size_t out_len) {
    i8042_device_t* root = get_i8042_instance(dev)->root;
    switch (op) {
    case IOCTL_INPUT_GET_PROTOCOL: {
        if (out_len < sizeof(int)) return ERR_INVALID_ARGS;
        int* reply = out_buf;
        *reply = root->type;
        return sizeof(*reply);
    }

    case IOCTL_INPUT_GET_REPORT_DESC_SIZE: {
        if (out_len < sizeof(size_t)) return ERR_INVALID_ARGS;
        size_t* reply = out_buf;
        switch (root->type) {
        case INPUT_PROTO_KBD:
            *reply = sizeof(kbd_hid_report_desc);
            break;
        case INPUT_PROTO_MOUSE:
            *reply = sizeof(mouse_hid_report_desc);
            break;
        default:
            return ERR_BAD_STATE;
        }
        return sizeof(*reply);
    }

    case IOCTL_INPUT_GET_REPORT_DESC: {
        switch (root->type) {
        case INPUT_PROTO_KBD:
            if (out_len < sizeof(kbd_hid_report_desc)) return ERR_INVALID_ARGS;
            memcpy(out_buf, &kbd_hid_report_desc, sizeof(kbd_hid_report_desc));
            return sizeof(kbd_hid_report_desc);
        case INPUT_PROTO_MOUSE:
            if (out_len < sizeof(mouse_hid_report_desc)) return ERR_INVALID_ARGS;
            memcpy(out_buf, &mouse_hid_report_desc, sizeof(mouse_hid_report_desc));
            return sizeof(mouse_hid_report_desc);
        default:
            return ERR_BAD_STATE;
        }
    }

    case IOCTL_INPUT_GET_NUM_REPORTS: {
        if (out_len < sizeof(size_t)) return ERR_INVALID_ARGS;
        size_t* reply = out_buf;
        *reply = 1;
        return sizeof(*reply);
    }

    case IOCTL_INPUT_GET_REPORT_IDS: {
        if (out_len < sizeof(input_report_id_t)) return ERR_INVALID_ARGS;
        input_report_id_t* reply = out_buf;
        *reply = 0;
        return sizeof(*reply);
    }

    case IOCTL_INPUT_GET_REPORT_SIZE:
    case IOCTL_INPUT_GET_MAX_REPORTSIZE: {
        if (out_len < sizeof(input_report_size_t)) return ERR_INVALID_ARGS;
        input_report_size_t* reply = out_buf;
        switch (root->type) {
        case INPUT_PROTO_KBD:
            *reply = sizeof(boot_kbd_report_t);
            break;
        case INPUT_PROTO_MOUSE:
            *reply = sizeof(boot_mouse_report_t);
            break;
        default:
            return ERR_BAD_STATE;
        }
        return sizeof(*reply);
    }
    }
    return ERR_NOT_SUPPORTED;
}

static mx_status_t i8042_instance_release(mx_device_t* dev) {
    i8042_instance_t* inst = get_i8042_instance(dev);
    mtx_lock(&inst->root->instance_lock);
    list_delete(&inst->node);
    mtx_unlock(&inst->root->instance_lock);
    free(inst);
    return NO_ERROR;
}

static mx_protocol_device_t i8042_instance_proto = {
    .read = i8042_read,
    .ioctl = i8042_ioctl,
    .release = i8042_instance_release,
};

static mx_status_t i8042_open(mx_device_t* dev, mx_device_t** dev_out, uint32_t flags) {
    i8042_device_t* i8042 = get_i8042_device(dev);

    i8042_instance_t* inst = calloc(1, sizeof(i8042_instance_t));
    if (!inst)
        return ERR_NO_MEMORY;

    mx_hid_fifo_init(&inst->fifo);

    const char* name = (i8042->type == INPUT_PROTO_MOUSE) ? "i8042-mouse" : "i8042-keyboard";
    device_init(&inst->device, i8042->drv, name, &i8042_instance_proto);

    inst->device.protocol_id = MX_PROTOCOL_INPUT;
    mx_status_t status = device_add_instance(&inst->device, dev);
    if (status != NO_ERROR) {
        free(inst);
        return status;
    }
    inst->root = i8042;

    mtx_lock(&i8042->instance_lock);
    list_add_tail(&i8042->instance_list, &inst->node);
    mtx_unlock(&i8042->instance_lock);

    *dev_out = &inst->device;
    return NO_ERROR;
}

static mx_status_t i8042_dev_release(mx_device_t* dev) {
    i8042_device_t* device = get_i8042_device(dev);
    free(device);
    return NO_ERROR;
}

static mx_protocol_device_t i8042_device_proto = {
    .open = i8042_open,
    .release = i8042_dev_release,
};

static mx_status_t i8042_setup(uint8_t* ctr) {
    // enable I/O port access
    mx_status_t status = mx_mmap_device_io(I8042_COMMAND_REG, 1);
    if (status)
        return status;
    status = mx_mmap_device_io(I8042_DATA_REG, 1);
    if (status)
        return status;

    // initialize hardware
    i8042_command(NULL, I8042_CMD_CTL_KBD_DIS);
    i8042_command(NULL, I8042_CMD_CTL_MOUSE_DIS);
    i8042_flush();

    if (i8042_command(ctr, I8042_CMD_CTL_RCTR) < 0)
        return -1;

    xprintf("i8042 controller register: 0x%02x\n", *ctr);
    bool have_mouse = !!(*ctr & I8042_CTR_AUXDIS);
    // disable IRQs and translation
    *ctr &= ~(I8042_CTR_KBDINT | I8042_CTR_AUXINT | I8042_CTR_XLATE);
    if (i8042_command(ctr, I8042_CMD_CTL_WCTR) < 0)
        return -1;

    if (i8042_selftest() < 0) {
        printf("i8042 self-test failed\n");
        return -1;
    }

    uint8_t resp = 0;
    if (i8042_command(&resp, I8042_CMD_CTL_KBD_TEST) < 0)
        return -1;
    if (resp != 0x00) {
        printf("i8042 kbd test failed: 0x%02x\n", resp);
        return -1;
    }
    if (have_mouse) {
        resp = 0;
        if (i8042_command(&resp, I8042_CMD_CTL_MOUSE_TEST) < 0)
            return -1;
        if (resp != 0x00) {
            printf("i8042 mouse test failed: 0x%02x\n", resp);
            return -1;
        }
    }
    return NO_ERROR;
}

static void i8042_identify(int (*cmd)(uint8_t* param, int command)) {
    uint8_t resp[3];
    if (cmd(resp, I8042_CMD_SCAN_DIS) < 0) return;
    resp[0] = 0;
    int ident_sz = cmd(resp, I8042_CMD_IDENTIFY);
    if (ident_sz < 0) return;
    printf("i8042 device ");
    switch (ident_sz) {
    case 1:
        printf("(unknown)");
        break;
    case 2:
        printf("0x%02x", resp[1]);
        break;
    case 3:
        printf("0x%02x 0x%02x", resp[1], resp[2]);
        break;
    default:
        printf("failed to respond to IDENTIFY");
    }
    printf("\n");
    cmd(resp, I8042_CMD_SCAN_EN);
}

static mx_status_t i8042_dev_init(i8042_device_t* dev) {
    mtx_init(&dev->instance_lock, mtx_plain);
    list_initialize(&dev->instance_list);

    // add to root device
    dev->device.protocol_id = MX_PROTOCOL_INPUT;
    mx_status_t status = device_add(&dev->device, NULL);
    if (status != NO_ERROR) {
        free(dev);
        return status;
    }

    // enable device port
    int cmd = dev->type == INPUT_PROTO_KBD ?
        I8042_CMD_CTL_KBD_DIS : I8042_CMD_CTL_MOUSE_DIS;
    i8042_command(NULL, cmd);

    i8042_identify(dev->type == INPUT_PROTO_KBD ?
            i8042_dev_command : i8042_aux_command);

    cmd = dev->type == INPUT_PROTO_KBD ?
        I8042_CMD_CTL_KBD_EN : I8042_CMD_CTL_MOUSE_EN;
    i8042_command(NULL, cmd);
    return NO_ERROR;
}

static int i8042_init_thread(void* arg) {
    mx_driver_t* driver = (mx_driver_t*)arg;
    uint8_t ctr = 0;
    mx_status_t status = i8042_setup(&ctr);
    if (status != NO_ERROR) {
        return status;
    }
    bool have_mouse = !!(ctr & I8042_CTR_AUXDIS);

    // turn on translation
    ctr |= I8042_CTR_XLATE;

    // enable devices and irqs
    ctr &= ~I8042_CTR_KBDDIS;
    ctr |= I8042_CTR_KBDINT;
    if (have_mouse) {
        ctr &= ~I8042_CTR_AUXDIS;
        ctr |= I8042_CTR_AUXINT;
    }

    if (i8042_command(&ctr, I8042_CMD_CTL_WCTR) < 0)
        return -1;

    // create keyboard device
    i8042_device_t* kbd_device = calloc(1, sizeof(i8042_device_t));
    if (!kbd_device)
        return ERR_NO_MEMORY;

    device_init(&kbd_device->device, driver, "i8042-keyboard", &i8042_device_proto);
    kbd_device->drv = driver;
    kbd_device->type = INPUT_PROTO_KBD;
    status = i8042_dev_init(kbd_device);
    if (status != NO_ERROR) {
        return status;
    }

    i8042_device_t* mouse_device = NULL;
    if (have_mouse) {
        mouse_device = calloc(1, sizeof(i8042_device_t));
        if (mouse_device) {
            device_init(&mouse_device->device, driver, "i8042-mouse", &i8042_device_proto);
            mouse_device->drv = driver;
            mouse_device->type = INPUT_PROTO_MOUSE;
            status = i8042_dev_init(mouse_device);
            if (status != NO_ERROR) {
                have_mouse = false;
            }
        }
    }

    // get interrupt wait handle
    kbd_device->irq = mx_interrupt_event_create(ISA_IRQ_KEYBOARD, MX_FLAG_REMAP_IRQ);
    if (kbd_device->irq < 0)
        goto fail;

    if (have_mouse) {
        mouse_device->irq = mx_interrupt_event_create(ISA_IRQ_MOUSE, MX_FLAG_REMAP_IRQ);
        if (mouse_device->irq < 0) {
            printf("i8042: interrupt_event_create failed for mouse\n");
            device_remove(&mouse_device->device);
            free(mouse_device);
            have_mouse = false;
        }
    }

    // create irq thread
    const char* name = "i8042-kbd-irq";
    int ret = thrd_create_with_name(&kbd_device->irq_thread, i8042_irq_thread, kbd_device, name);
    if (ret != thrd_success)
        goto fail;

    if (have_mouse) {
        name = "i8042-mouse-irq";
        ret = thrd_create_with_name(&mouse_device->irq_thread, i8042_irq_thread, mouse_device, name);
        if (ret != thrd_success) {
            printf("i8042: could not create irq thread for mouse\n");
            device_remove(&mouse_device->device);
            free(mouse_device);
        }
    }

    xprintf("initialized i8042 driver\n");

    return NO_ERROR;

fail:
    device_remove(&kbd_device->device);
    free(kbd_device);
    if (have_mouse) {
        free(mouse_device);
    }
    return status;
}

static mx_status_t i8042_init(mx_driver_t* driver) {
    thrd_t t;
    int rc = thrd_create_with_name(&t, i8042_init_thread, driver, "i8042-init");
    return rc;
}

mx_driver_t _driver_i8042 BUILTIN_DRIVER = {
    .name = "i8042",
    .ops = {
        .init = i8042_init,
    },
};
