// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <magenta/syscalls.h>
#include <unittest/unittest.h>

bool vmo_create_test(void) {
    BEGIN_TEST;

    mx_status_t status;
    mx_handle_t vmo[16];

    // allocate a bunch of vmos then free them
    for (size_t i = 0; i < countof(vmo); i++) {
        vmo[i] = mx_vmo_create(i * PAGE_SIZE);
        EXPECT_LT(0, vmo[i], "vm_object_create");
    }

    for (size_t i = 0; i < countof(vmo); i++) {
        status = mx_handle_close(vmo[i]);
        EXPECT_EQ(NO_ERROR, status, "handle_close");
    }

    END_TEST;
}

bool vmo_read_write_test(void) {
    BEGIN_TEST;

    mx_status_t status;
    mx_ssize_t sstatus;
    mx_handle_t vmo;

    // allocate an object and read/write from it
    const size_t len = PAGE_SIZE * 4;
    vmo = mx_vmo_create(len);
    EXPECT_LT(0, vmo, "vm_object_create");

    char buf[PAGE_SIZE];
    sstatus = mx_vmo_read(vmo, buf, 0, sizeof(buf));
    EXPECT_EQ((mx_ssize_t)sizeof(buf), sstatus, "vm_object_read");

    memset(buf, 0x99, sizeof(buf));
    sstatus = mx_vmo_write(vmo, buf, 0, sizeof(buf));
    EXPECT_EQ((mx_ssize_t)sizeof(buf), sstatus, "vm_object_write");

    // map it
    uintptr_t ptr;
    status = mx_process_map_vm(0, vmo, 0, len, &ptr,
                                     MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE);
    EXPECT_EQ(NO_ERROR, status, "vm_map");
    EXPECT_NEQ(0u, ptr, "vm_map");

    // check that it matches what we last wrote into it
    EXPECT_BYTES_EQ((void*)buf, (void*)ptr, sizeof(buf), "mapped buffer");

    status = mx_process_unmap_vm(0, ptr, 0);
    EXPECT_EQ(NO_ERROR, status, "vm_unmap");

    // close the handle
    status = mx_handle_close(vmo);
    EXPECT_EQ(NO_ERROR, status, "handle_close");

    END_TEST;
}

bool vmo_read_only_map_test(void) {
    BEGIN_TEST;

    mx_status_t status;
    mx_handle_t vmo;

    // allocate an object and read/write from it
    const size_t len = PAGE_SIZE;
    vmo = mx_vmo_create(len);
    EXPECT_LT(0, vmo, "vm_object_create");

    // map it
    uintptr_t ptr;
    status = mx_process_map_vm(0, vmo, 0, len, &ptr,
                                     MX_VM_FLAG_PERM_READ);
    EXPECT_EQ(NO_ERROR, status, "vm_map");
    EXPECT_NEQ(0u, ptr, "vm_map");

    status = mx_cprng_draw((void*)ptr, 1);
    EXPECT_LT(status, 0, "write");

    status = mx_process_unmap_vm(0, ptr, 0);
    EXPECT_EQ(NO_ERROR, status, "vm_unmap");

    // close the handle
    status = mx_handle_close(vmo);
    EXPECT_EQ(NO_ERROR, status, "handle_close");

    END_TEST;
}

bool vmo_resize_test(void) {
    BEGIN_TEST;

    mx_status_t status;
    mx_handle_t vmo;

    // allocate an object
    size_t len = PAGE_SIZE * 4;
    vmo = mx_vmo_create(len);
    EXPECT_LT(0, vmo, "vm_object_create");

    // get the size that we set it to
    uint64_t size = 0x99999999;
    status = mx_vmo_get_size(vmo, &size);
    EXPECT_EQ(NO_ERROR, status, "vm_object_get_size");
    EXPECT_EQ(len, size, "vm_object_get_size");

// set_size is not implemented right now, so test for the failure mode
#if 0
    // try to resize it
    len += PAGE_SIZE;
    status = mx_vmo_set_size(vmo, len);
    EXPECT_EQ(NO_ERROR, status, "vm_object_set_size");

    // get the size again
    size = 0x99999999;
    status = mx_vmo_get_size(vmo, &size);
    EXPECT_EQ(NO_ERROR, status, "vm_object_get_size");
    EXPECT_EQ(len, size, "vm_object_get_size");

    // try to resize it to a ludicrous size
    status = mx_vmo_set_size(vmo, UINT64_MAX);
    EXPECT_EQ(ERR_NO_MEMORY, status, "vm_object_set_size");
#else
    status = mx_vmo_set_size(vmo, len + PAGE_SIZE);
    EXPECT_EQ(ERR_NOT_SUPPORTED, status, "vm_object_set_size");
#endif

    // close the handle
    status = mx_handle_close(vmo);
    EXPECT_EQ(NO_ERROR, status, "handle_close");

    END_TEST;
}

BEGIN_TEST_CASE(vmo_tests)
RUN_TEST(vmo_create_test);
RUN_TEST(vmo_read_write_test);
RUN_TEST(vmo_read_only_map_test);
RUN_TEST(vmo_resize_test);
END_TEST_CASE(vmo_tests)

int main(int argc, char** argv) {
    bool success = unittest_run_all_tests(argc, argv);
    return success ? 0 : -1;
}
