// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <magenta/syscalls.h>
#include <mxio/remoteio.h>
#include <unittest/unittest.h>

/**
 * Remoteio aims to test the basic client/server interaction that remoteio
 * provides.
 *
 * This test is designed to be simple, and does not test error cases. It also
 * only tests 'full' reads and writes.
 *
 * The servers provided by this test (seen in the callback functions) are fake.
 * They observe and verify input that is expected to be passed, but don't do
 * anything with it. This means that many inputs seen here may be nonsensical,
 * but this test is designed to check plumbing, not sane file access.
 *
 * "Real" code using remoteio would provide different callbacks, which may
 * interact with a real storage system.
 *
 * To aid with the tracing of client/server interactions, follow the 'counter'
 * variable throughout the code.
 */



uintptr_t counter = 0;

const uintptr_t dir_cookie_gold = 0x1234;
const uintptr_t file_cookie_gold = 0x5678;

const char* write_data_gold = "foo contents";

const off_t seek_offset_gold = 2;
const int seek_whence_gold = 3;
const off_t seek_response_gold = 4;

const char* read_data_gold = "fizz buzz";

mx_status_t callback_file_access(mx_rio_msg_t* msg, void* cookie) {
    EXPECT_EQ(file_cookie_gold, (uintptr_t)cookie, "Invalid file callback cookie");
    switch (msg->op) {
    case MX_RIO_OPEN:
        return ERR_NOT_SUPPORTED;
    case MX_RIO_WRITE:
        EXPECT_EQ(counter++, 6u, "");
        EXPECT_EQ(strlen(write_data_gold) + 1, msg->datalen, "Unexpected datalen");
        EXPECT_EQ(strcmp((const char*)msg->data, write_data_gold), 0, "");
        // TODO(smklein): Set / test offset.
        return msg->datalen;
    case MX_RIO_SEEK:
        EXPECT_EQ(counter++, 8u, "");
        EXPECT_EQ(seek_whence_gold, msg->arg, "Unexpected arg");
        EXPECT_EQ(seek_offset_gold, msg->arg2.off, "Unexpected off");
        msg->arg2.off = seek_response_gold;
        return NO_ERROR;
    case MX_RIO_READ:
        EXPECT_EQ(counter++, 10u, "");
        // Copy the string and the null character.
        strncpy((char*)msg->data, read_data_gold, strlen(read_data_gold) + 1);
        msg->datalen = strlen(read_data_gold) + 1;
        // TODO(smklein): Set / test offset.
        return msg->datalen;
    case MX_RIO_CLOSE:
        EXPECT_EQ(counter++, 12u, "");
        return NO_ERROR;
    default:
        EXPECT_TRUE(false, "Operation not supported");
        return ERR_NOT_SUPPORTED;
    }
}

const int32_t open_flags_gold = 0x2222;
const char* open_path_gold = "foo";

mx_status_t callback_directory_access(mx_rio_msg_t* msg, void* cookie) {
    EXPECT_EQ(dir_cookie_gold, (uintptr_t)cookie, "Invalid dir callback cookie");
    switch (msg->op) {
    case MX_RIO_OPEN:
        // Verify input.
        EXPECT_EQ(counter++, 2u, "");
        EXPECT_EQ(strlen(open_path_gold), msg->datalen, "Unexpected datalen");
        EXPECT_EQ(open_flags_gold, msg->arg, "Unexpected arg");
        EXPECT_EQ(strncmp((const char*)msg->data, open_path_gold, msg->datalen), 0, "");
        // TODO(smklein): Test 'mode_gold'

        // Create another handler server, responsible for dealing with the file.
        mx_handle_t file_handle_client;
        mx_handle_t file_handle_server;
        file_handle_client = mx_msgpipe_create(&file_handle_server);
        EXPECT_GT(file_handle_client, 0, "Invalid file handle client");
        EXPECT_EQ(NO_ERROR, mxio_handler_create(file_handle_server,
                                                callback_file_access,
                                                (void*)file_cookie_gold),
                  "Could not create file handler server");
        msg->arg2.protocol = MXIO_PROTOCOL_REMOTE;
        msg->handle[0] = file_handle_client;
        msg->hcount = 1;
        return NO_ERROR;
    case MX_RIO_CLOSE:
        EXPECT_EQ(counter++, 4u, "");
        return NO_ERROR;
    default:
        EXPECT_TRUE(false, "Operation not supported");
        return ERR_NOT_SUPPORTED;
    }
}

bool remoteio_test(void) {
    BEGIN_TEST;
    // First, initialize the message pipes we'll be passing around later.
    mx_handle_t dir_handle_client;
    mx_handle_t dir_handle_server;
    dir_handle_client = mx_msgpipe_create(&dir_handle_server);
    EXPECT_GT(dir_handle_client, 0, "Invalid dir handle client");

    // Next, initialize the directory server.
    EXPECT_EQ(NO_ERROR, mxio_handler_create(dir_handle_server,
                                            callback_directory_access,
                                            (void*)dir_cookie_gold),
              "Could not create dir handler server");
    ASSERT_EQ(counter++, 0u, "");
    mxio_t* dir_client = mxio_remote_create(dir_handle_client, 0);
    EXPECT_NEQ(dir_client, NULL, "Could not create dir client from handle");
    ASSERT_EQ(counter++, 1u, "");

    // Open a file, causing a new file server to open.
    mxio_t* file_client;
    EXPECT_EQ(NO_ERROR, mx_open(dir_client, open_path_gold, open_flags_gold,
                                &file_client),
              "Error opening file client");
    EXPECT_NEQ(file_client, NULL, "Could not open file client");
    ASSERT_EQ(counter++, 3u,  "");

    // Close the directory server -- we no longer need it.
    EXPECT_EQ(NO_ERROR, mx_close(dir_client), "Unexpected close status");
    ASSERT_EQ(counter++, 5u, "");

    // Write to the 'file'. The write should 'transfer' all of write_data_gold.
    // +1 to write the null character too.
    EXPECT_EQ((ssize_t)strlen(write_data_gold) + 1,
              mx_write(file_client, write_data_gold, strlen(write_data_gold) + 1),
              "Unexpected number of bytes written");
    ASSERT_EQ(counter++, 7u, "");

    EXPECT_EQ(seek_response_gold,
              mx_seek(file_client, seek_offset_gold, seek_whence_gold),
              "Unexpected seek response");
    ASSERT_EQ(counter++, 9u, "");

    // Read from the 'file'. The read should complete in its entirety.
    // Note that we are ignoring the results of 'seek' intentionally.
    char read_buffer[100];
    EXPECT_EQ((ssize_t)strlen(read_data_gold) + 1,
              mx_read(file_client, read_buffer, sizeof(read_buffer)),
              "Unexpected number of bytes read");
    ASSERT_EQ(counter++, 11u, "");
    ASSERT_EQ(strcmp(read_buffer, read_data_gold), 0, "read_buffer and read_data_gold should be same");

    EXPECT_EQ(NO_ERROR, mx_close(file_client), "Unexpected close status");
    ASSERT_EQ(counter++, 13u, "");

    END_TEST;
}

BEGIN_TEST_CASE(remoteio_tests)
RUN_TEST(remoteio_test);
END_TEST_CASE(remoteio_tests)

int main(int argc, char** argv) {
    bool success = unittest_run_all_tests();
    return success ? 0 : -1;
}
