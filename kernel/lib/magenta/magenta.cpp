// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <magenta/magenta.h>

#include <trace.h>

#include <kernel/auto_lock.h>
#include <kernel/mutex.h>

#include <lk/init.h>

#include <lib/console.h>

#include <magenta/dispatcher.h>
#include <magenta/excp_port.h>
#include <magenta/handle.h>
#include <magenta/process_dispatcher.h>
#include <magenta/resource_dispatcher.h>
#include <magenta/state_tracker.h>

// The next two includes should be removed. See DeleteHandle().
#include <magenta/pci_interrupt_dispatcher.h>
#include <magenta/io_mapping_dispatcher.h>

#include <mxtl/arena.h>
#include <mxtl/intrusive_double_list.h>
#include <mxtl/type_support.h>

#define LOCAL_TRACE 0

// This handle limit relects the fact that VMOs have an internal array
// with entry per memory page. When we switch to a tree of ranges we can
// up this limit.
constexpr size_t kMaxHandleCount = 32 * 1024;

// The handle arena and its mutex.
mutex_t handle_mutex = MUTEX_INITIAL_VALUE(handle_mutex);
mxtl::TypedArena<Handle> handle_arena;

// The system exception port.
static mxtl::RefPtr<ExceptionPort> system_exception_port;
static mutex_t system_exception_mutex = MUTEX_INITIAL_VALUE(system_exception_mutex);

void magenta_init(uint level) {
    handle_arena.Init("handles", kMaxHandleCount);
}

Handle* MakeHandle(mxtl::RefPtr<Dispatcher> dispatcher, mx_rights_t rights) {
    AutoLock lock(&handle_mutex);
    return handle_arena.New(mxtl::move(dispatcher), rights);
}

Handle* DupHandle(Handle* source, mx_rights_t rights) {
    AutoLock lock(&handle_mutex);
    return handle_arena.New(source, rights);
}

void DeleteHandle(Handle* handle) {
    StateTracker* state_tracker = handle->dispatcher()->get_state_tracker();
    if (state_tracker) {
        state_tracker->Cancel(handle);
    } else {
        auto disp = handle->dispatcher();
        // This code is sad but necessary because certain dispatchers
        // have complicated Close() logic which cannot be untangled at
        // this time.
        switch (disp->get_type()) {
            case MX_OBJ_TYPE_IOMAP: disp->get_specific<IoMappingDispatcher>()->Close();
                break;
            default:  break;
                // This is fine. See for example the LogDispatcher.
        };
    }
    // Calling the handle dtor can cause many things to happen, so it is important
    // to call it outside the lock.
    handle->~Handle();
    // Setting the memory to zero is critical for the safe operation of the handle
    // table lookup.
    memset(handle, 0, sizeof(Handle));

    AutoLock lock(&handle_mutex);
    handle_arena.RawFree(handle);
}

bool HandleInRange(void* addr) {
    AutoLock lock(&handle_mutex);
    return handle_arena.in_range(addr);
}

uint32_t MapHandleToU32(const Handle* handle) {
    auto va = handle - reinterpret_cast<Handle*>(handle_arena.start());
    return static_cast<uint32_t>(va);
}

Handle* MapU32ToHandle(uint32_t value) {
    auto va = &reinterpret_cast<Handle*>(handle_arena.start())[value];
    if (!HandleInRange(va))
        return nullptr;
    return reinterpret_cast<Handle*>(va);
}

mx_status_t SetSystemExceptionPort(mxtl::RefPtr<ExceptionPort> eport) {
    AutoLock lock(&system_exception_mutex);
    if (system_exception_port)
        return ERR_BAD_STATE; // TODO(dje): ?
    system_exception_port = eport;
    return NO_ERROR;
}

void ResetSystemExceptionPort() {
    AutoLock lock(&system_exception_mutex);
    system_exception_port.reset();
}

mxtl::RefPtr<ExceptionPort> GetSystemExceptionPort() {
    AutoLock lock(&system_exception_mutex);
    return system_exception_port;
}

bool magenta_rights_check(mx_rights_t actual, mx_rights_t desired) {
    if ((actual & desired) == desired)
        return true;
    LTRACEF("rights check fail!! has 0x%x, needs 0x%x\n", actual, desired);
    return false;
}

mx_status_t magenta_sleep(mx_time_t nanoseconds) {
    lk_time_t t = mx_time_to_lk(nanoseconds);
    if ((nanoseconds > 0ull) && (t == 0u))
        t = 1u;

    /* sleep with interruptable flag set */
    return thread_sleep_etc(t, true);
}

mx_status_t validate_resource_handle(mx_handle_t handle) {
    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<ResourceDispatcher> resource;
    return up->GetDispatcher(handle, &resource);
}

LK_INIT_HOOK(magenta, magenta_init, LK_INIT_LEVEL_THREADING);
