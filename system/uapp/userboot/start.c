// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bootfs.h"
#include "userboot-elf.h"
#include "option.h"
#include "util.h"

#include "../../ulib/launchpad/stack.h"

#pragma GCC visibility push(hidden)

#include <magenta/syscalls.h>
#include <magenta/syscalls-ddk.h>
#include <runtime/message.h>
#include <runtime/processargs.h>
#include <stdalign.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/param.h>

#pragma GCC visibility pop

#define SHUTDOWN_COMMAND "poweroff"

static noreturn void do_shutdown(mx_handle_t log) {
    print(log, "Process exited.  Executing \"", SHUTDOWN_COMMAND, "\".\n",
          NULL);
    mx_debug_send_command(SHUTDOWN_COMMAND, strlen(SHUTDOWN_COMMAND));
    print(log, "still here after shutdown!\n", NULL);
    while (true)
        __builtin_trap();
}

static void load_child_process(mx_handle_t log, const struct options* o,
                               mx_handle_t bootfs_vmo, mx_handle_t vdso_vmo,
                               mx_handle_t proc, mx_handle_t to_child,
                               mx_vaddr_t* entry, mx_vaddr_t* vdso_base,
                               size_t* stack_size) {
    // Examine the bootfs image and find the requested file in it.
    struct bootfs bootfs;
    bootfs_mount(log, bootfs_vmo, &bootfs);

    // This will handle a PT_INTERP by doing a second lookup in bootfs.
    *entry = elf_load_bootfs(log, &bootfs, proc, o->value[OPTION_FILENAME],
                             to_child, stack_size);

    // All done with bootfs!
    bootfs_unmount(log, &bootfs);

    // Now load the vDSO into the child, so it has access to system calls.
    *vdso_base = elf_load_vmo(log, proc, vdso_vmo);
}

// This is the main logic:
// 1. Read the kernel's bootstrap message.
// 2. Load up the child process from ELF file(s) on the bootfs.
// 3. Create the initial thread and allocate a stack for it.
// 4. Load up a message pipe with the mx_proc_args_t message for the child.
// 5. Start the child process running.
// 6. Optionally, wait for it to exit and then shut down.
static noreturn void bootstrap(mx_handle_t log, mx_handle_t bootstrap_pipe) {
    // Sample the bootstrap message to see how big it is.
    uint32_t nbytes;
    uint32_t nhandles;
    mx_status_t status = mxr_message_size(bootstrap_pipe, &nbytes, &nhandles);
    check(log, status, "mxr_message_size failed on bootstrap pipe!\n");

    // Read the bootstrap message from the kernel.
    MXR_PROCESSARGS_BUFFER(buffer, nbytes);
    mx_handle_t handles[nhandles];
    mx_proc_args_t* pargs;
    uint32_t* handle_info;
    status = mxr_processargs_read(bootstrap_pipe,
                                  buffer, nbytes, handles, nhandles,
                                  &pargs, &handle_info);
    check(log, status, "mxr_processargs_read failed on bootstrap message!\n");

    // All done with the message pipe from the kernel now.  Let it go.
    mx_handle_close(bootstrap_pipe);

    // Extract the environment (aka kernel command line) strings.
    char* environ[pargs->environ_num + 1];
    status = mxr_processargs_strings(buffer, nbytes, NULL, environ);
    check(log, status,
          "mxr_processargs_strings failed on bootstrap message\n");

    // Process the kernel command line, which gives us options and also
    // becomes the environment strings for our child.
    struct options o;
    parse_options(log, &o, environ);

    mx_handle_t resource_root = MX_HANDLE_INVALID;
    mx_handle_t bootfs_vmo = MX_HANDLE_INVALID;
    mx_handle_t vdso_vmo = MX_HANDLE_INVALID;
    mx_handle_t* proc_handle_loc = NULL;
    mx_handle_t* thread_handle_loc = NULL;
    mx_handle_t* stack_vmo_handle_loc = NULL;
    for (uint32_t i = 0; i < nhandles; ++i) {
        switch (MX_HND_INFO_TYPE(handle_info[i])) {
        case MX_HND_TYPE_VDSO_VMO:
            vdso_vmo = handles[i];
            break;
        case MX_HND_TYPE_BOOTFS_VMO:
            if (MX_HND_INFO_ARG(handle_info[i]) == 0)
                bootfs_vmo = handles[i];
            break;
        case MX_HND_TYPE_PROC_SELF:
            proc_handle_loc = &handles[i];
            break;
        case MX_HND_TYPE_THREAD_SELF:
            thread_handle_loc = &handles[i];
            break;
        case MX_HND_TYPE_STACK_VMO:
            stack_vmo_handle_loc = &handles[i];
            break;
        case MX_HND_TYPE_RESOURCE:
            resource_root = handles[i];
            break;
        }
    }
    if (bootfs_vmo == MX_HANDLE_INVALID)
        fail(log, ERR_INVALID_ARGS, "no bootfs handle in bootstrap message\n");
    if (vdso_vmo == MX_HANDLE_INVALID)
        fail(log, ERR_INVALID_ARGS, "no vDSO handle in bootstrap message\n");
    if (resource_root == MX_HANDLE_INVALID)
        fail(log, ERR_INVALID_ARGS, "no resource handle in bootstrap message\n");

    // Make the message pipe for the bootstrap message.
    mx_handle_t pipeh[2];
    status = mx_message_pipe_create(pipeh, 0);
    check(log, status, "mx_message_pipe_create failed\n");
    mx_handle_t to_child = pipeh[0];
    mx_handle_t child_start_handle = pipeh[1];

    const char* filename = o.value[OPTION_FILENAME];
    mx_handle_t proc = mx_process_create(filename, strlen(filename), 0);
    if (proc < 0)
        fail(log, proc, "mx_process_create failed\n");

    mx_vaddr_t entry, vdso_base;
    size_t stack_size = DEFAULT_STACK_SIZE;
    load_child_process(log, &o, bootfs_vmo, vdso_vmo, proc, to_child,
                       &entry, &vdso_base, &stack_size);

    // Allocate the stack for the child.
    stack_size = (stack_size + PAGE_SIZE - 1) & -PAGE_SIZE;
    mx_handle_t stack_vmo = mx_vm_object_create(stack_size);
    if (stack_vmo < 0)
        fail(log, stack_vmo, "mx_vm_object_create failed for child stack\n");
    mx_vaddr_t stack_base;
    status = mx_process_vm_map(proc, stack_vmo, 0, stack_size, &stack_base,
                               MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE);
    check(log, status, "mx_process_vm_map failed for child stack\n");
    uintptr_t sp = sp_from_mapping(stack_base, stack_size);
    if (stack_vmo_handle_loc != NULL) {
        // This is our own stack VMO handle, but we don't need it for anything.
        if (*stack_vmo_handle_loc != MX_HANDLE_INVALID)
            mx_handle_close(*stack_vmo_handle_loc);
        *stack_vmo_handle_loc = stack_vmo;
    } else {
        mx_handle_close(stack_vmo);
    }

    if (proc_handle_loc != NULL) {
        // This is our own proc handle, but we don't need it for anything.
        if (*proc_handle_loc != MX_HANDLE_INVALID)
            mx_handle_close(*proc_handle_loc);
        // Reuse the slot for the child's handle.
        *proc_handle_loc = mx_handle_duplicate(proc, MX_RIGHT_SAME_RIGHTS);
        if (*proc_handle_loc < 0)
            fail(log, *proc_handle_loc,
                 "mx_handle_duplicate failed on child process handle\n");
    }

    // create the initial thread in the new process
    mx_handle_t thread = mx_thread_create(proc, filename, strlen(filename), 0);
    if (thread < 0)
        fail(log, thread, "mx_thread_create failed\n");

    if (thread_handle_loc != NULL) {
        // Reuse the slot for the child's handle.
        *thread_handle_loc = mx_handle_duplicate(thread, MX_RIGHT_SAME_RIGHTS);
        if (*thread_handle_loc < 0)
            fail(log, *thread_handle_loc,
                 "mx_handle_duplicate failed on child thread handle\n");
    }

    // Now send the bootstrap message, consuming both our VMO handles.
    status = mx_message_write(to_child, buffer, nbytes, handles, nhandles, 0);
    check(log, status, "mx_message_write to child failed\n");
    status = mx_handle_close(to_child);
    check(log, status, "mx_handle_close failed on message pipe handle\n");

    // Start the process going.
    status = mx_process_start(proc, thread, entry, sp,
                              child_start_handle, vdso_base);
    check(log, status, "mx_process_start failed\n");
    status = mx_handle_close(thread);
    check(log, status, "mx_handle_close failed on thread handle\n");

    if (o.value[OPTION_SHUTDOWN] != NULL) {
        print(log, "Waiting for ", o.value[OPTION_FILENAME], " to exit...\n",
              NULL);
        status = mx_handle_wait_one(
            proc, MX_SIGNAL_SIGNALED, MX_TIME_INFINITE, NULL);
        check(log, status, "mx_handle_wait_one on process failed\n");
        do_shutdown(log);
    }

    // Now we've accomplished our purpose in life, and we can die happy.

    status = mx_handle_close(proc);
    check(log, status, "mx_handle_close failed on process handle\n");

    print(log, o.value[OPTION_FILENAME], " started.  userboot exiting.\n",
          NULL);
    mx_exit(0);
}

// This is the entry point for the whole show, the very first bit of code
// to run in user mode.
noreturn void _start(void* start_arg) {
    mx_handle_t log = mx_log_create(MX_LOG_FLAG_DEVMGR);
    if (log == MX_HANDLE_INVALID)
        print(log, "mx_log_create failed, using mx_debug_write instead\n",
              NULL);

    mx_handle_t bootstrap_pipe = (uintptr_t)start_arg;
    bootstrap(log, bootstrap_pipe);
}
