// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <threads.h>
#include <time.h>

#include <magenta/syscalls.h>
#include <unittest/unittest.h>

// This example tests transfering message pipe handles through message pipes. To do so, it:
//   Creates two message pipes, A and B, with handles A0-A1 and B0-B1, respectively
//   Sends message "1" into A0
//   Sends A1 to B0
//   Sends message "2" into A0
//   Reads H from B1 (should receive A1 again, possibly with a new value)
//   Sends "3" into A0
//   Reads from H until empty. Should read "1", "2", "3" in that order.
bool handle_transfer_test(void) {
    BEGIN_TEST;
    mx_handle_t A[2];
    mx_status_t status = mx_msgpipe_create(A, 0);
    char msg[512];
    snprintf(msg, sizeof(msg), "failed to create message pipe A: %d\n", status);
    EXPECT_EQ(status, 0, msg);

    mx_handle_t B[2];
    status = mx_msgpipe_create(B, 0);
    snprintf(msg, sizeof(msg), "failed to create message pipe B: %d\n", status);
    EXPECT_EQ(status, 0, msg);

    status = mx_msgpipe_write(A[0], "1", 1u, NULL, 0u, 0u);
    snprintf(msg, sizeof(msg), "failed to write message \"1\" into A0: %u\n", status);
    EXPECT_EQ(status, NO_ERROR, msg);

    status = mx_msgpipe_write(B[0], NULL, 0u, &A[1], 1u, 0u);
    snprintf(msg, sizeof(msg), "failed to write message with handle A[1]: %u\n", status);
    EXPECT_EQ(status, NO_ERROR, msg);

    A[1] = MX_HANDLE_INVALID;
    status = mx_msgpipe_write(A[0], "2", 1u, NULL, 0u, 0u);
    snprintf(msg, sizeof(msg), "failed to write message \"2\" into A0: %u\n", status);
    EXPECT_EQ(status, NO_ERROR, msg);

    mx_handle_t H;
    uint32_t num_bytes = 0u;
    uint32_t num_handles = 1u;
    status = mx_msgpipe_read(B[1], NULL, &num_bytes, &H, &num_handles, 0u);
    snprintf(msg, sizeof(msg), "failed to read message from B1: %u\n", status);
    EXPECT_EQ(status, NO_ERROR, msg);

    snprintf(msg, sizeof(msg), "failed to read actual handle value from B1\n");
    EXPECT_FALSE((num_handles != 1u || H == MX_HANDLE_INVALID), msg);

    status = mx_msgpipe_write(A[0], "3", 1u, NULL, 0u, 0u);
    snprintf(msg, sizeof(msg), "failed to write message \"3\" into A0: %u\n", status);
    EXPECT_EQ(status, NO_ERROR, msg);

    for (int i = 0; i < 3; ++i) {
        char buf[1];
        num_bytes = 1u;
        num_handles = 0u;
        status = mx_msgpipe_read(H, buf, &num_bytes, NULL, &num_handles, 0u);
        snprintf(msg, sizeof(msg), "failed to read message from H: %u\n", status);
        EXPECT_EQ(status, NO_ERROR, msg);
        unittest_printf("read message: %c\n", buf[0]);
    }

    mx_handle_close(A[0]);
    mx_handle_close(B[0]);
    mx_handle_close(B[1]);
    mx_handle_close(H);
    END_TEST;
}

static int thread(void* arg) {
    // sleep for 10ms
    // this is race-prone, but until there's a way to wait for a thread to be
    // blocked, there's no better way to determine that the other thread has
    // entered handle_wait_one.
    struct timespec t = (struct timespec){
        .tv_sec = 0,
        .tv_nsec = 10 * 1000 * 1000,
    };
    nanosleep(&t, NULL);

    // Send A0 through B1 to B0.
    mx_handle_t* A = (mx_handle_t*)arg;
    mx_handle_t* B = A + 2;
    mx_status_t status = mx_msgpipe_write(B[1], NULL, 0u, &A[0], 1, 0);
    if (status != NO_ERROR) {
        UNITTEST_TRACEF("failed to write message with handle A0 to B1: %d\n", status);
        goto thread_exit;
    }

    // Read from B0 into H, thus canceling any waits on A0.
    mx_handle_t H;
    uint32_t num_bytes = 0, num_handles = 1;
    status = mx_msgpipe_read(B[0], NULL, &num_bytes, &H, &num_handles, 0);
    if (status != NO_ERROR || num_handles < 1) {
        UNITTEST_TRACEF("failed to read message handle H from B0: %d\n", status);
    }

thread_exit:
    return 0;
}

// This tests canceling a wait when a handle is transferred.
//   There are two message pipes: A0-A1 and B0-B1.
//   A thread is created that sends A0 from B1 to B0.
//   main() waits on A0.
//   The thread then reads from B0, which should cancel the wait in main().
// See [MG-103].
bool handle_transfer_cancel_wait_test(void) {
    BEGIN_TEST;
    mx_handle_t A[4];
    mx_handle_t* B = &A[2];
    mx_status_t status = mx_msgpipe_create(A, 0);
    char msg[512];
    snprintf(msg, sizeof(msg), "failed to create message pipe A[0,1]: %d\n", status);
    EXPECT_EQ(status, 0, msg);
    status = mx_msgpipe_create(B, 0);
    snprintf(msg, sizeof(msg), "failed to create message pipe B[0,1]: %d\n", status);
    EXPECT_EQ(status, 0, msg);

    thrd_t thr;
    int ret = thrd_create_with_name(&thr, thread, A, "write thread");
    EXPECT_EQ(ret, thrd_success, "failed to create write thread");

    mx_signals_state_t signals_state;
    mx_signals_t signals = MX_SIGNAL_PEER_CLOSED;
    status = mx_handle_wait_one(A[0], signals, MX_SEC(1), &signals_state);
    EXPECT_NEQ(ERR_TIMED_OUT, status, "failed to complete wait when handle transferred");

    thrd_join(thr, NULL);
    mx_handle_close(B[1]);
    mx_handle_close(B[0]);
    mx_handle_close(A[1]);
    mx_handle_close(A[0]);
    END_TEST;
}

BEGIN_TEST_CASE(handle_transfer_tests)
RUN_TEST(handle_transfer_test)
RUN_TEST(handle_transfer_cancel_wait_test)
END_TEST_CASE(handle_transfer_tests)

#ifndef BUILD_COMBINED_TESTS
int main(int argc, char** argv) {
    return unittest_run_all_tests(argc, argv) ? 0 : -1;
}
#endif
