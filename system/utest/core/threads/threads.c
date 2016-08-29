// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <magenta/syscalls.h>
#include <unittest/unittest.h>
#include <test-utils/test-utils.h>
#include <runtime/thread.h>

static intptr_t thread_1(void* arg) {
    unittest_printf("thread 1 sleeping for .1 seconds\n");
    struct timespec t = (struct timespec){
        .tv_sec = 0,
        .tv_nsec = 100 * 1000 * 1000,
    };
    nanosleep(&t, NULL);

    unittest_printf("thread 1 calling mx_thread_exit()\n");
    mx_thread_exit();
    return 0;
}

bool threads_test(void) {
    BEGIN_TEST;
    mx_handle_t handle;
    unittest_printf("Welcome to thread test!\n");

    for (int i = 0; i != 4; ++i) {
        handle = tu_thread_create(thread_1, NULL, "thread 1");
        ASSERT_GT(handle, 0, "Error while creating thread");
        unittest_printf("thread:%d created handle %d\n", i, handle);

        mx_handle_wait_one(handle, MX_SIGNAL_SIGNALED, MX_TIME_INFINITE, NULL);
        unittest_printf("thread:%d joined\n", i);

        mx_handle_close(handle);
    }

    unittest_printf("Attempting to create thread with a super long name. This should fail\n");
    mxr_thread_t *t = NULL;
    mx_status_t status = mxr_thread_create(
                                    "01234567890123456789012345678901234567890123456789012345678901234567890123456789", &t);
    ASSERT_LT(status, 0, "Thread creation should have failed");
    unittest_printf("mxr_thread_create returned %d\n", status);

    END_TEST;
}

BEGIN_TEST_CASE(threads_tests)
RUN_TEST(threads_test)
END_TEST_CASE(threads_tests)

#ifndef BUILD_COMBINED_TESTS
int main(int argc, char** argv) {
    return unittest_run_all_tests(argc, argv) ? 0 : -1;
}
#endif
