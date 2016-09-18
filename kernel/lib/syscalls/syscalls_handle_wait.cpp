// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <err.h>
#include <inttypes.h>
#include <trace.h>

#include <kernel/auto_lock.h>

#include <lib/ktrace.h>
#include <lib/user_copy.h>
#include <lib/user_copy/user_ptr.h>

#include <magenta/magenta.h>
#include <magenta/process_dispatcher.h>
#include <magenta/user_copy.h>
#include <magenta/wait_event.h>
#include <magenta/wait_state_observer.h>

#include <mxtl/ref_ptr.h>

#include "syscalls_priv.h"

#define LOCAL_TRACE 0

constexpr uint32_t kMaxWaitHandleCount = 256u;

mx_status_t sys_handle_wait_one(mx_handle_t handle_value,
                                mx_signals_t signals,
                                mx_time_t timeout,
                                user_ptr<mx_signals_state_t> _signals_state) {
    LTRACEF("handle %u\n", handle_value);

    WaitEvent event;

    status_t result;
    WaitStateObserver wait_state_observer;

    auto up = ProcessDispatcher::GetCurrent();
    {
        AutoLock lock(up->handle_table_lock());

        Handle* handle = up->GetHandle_NoLock(handle_value);
        if (!handle)
            return up->BadHandle(handle_value, ERR_BAD_HANDLE);
        if (!magenta_rights_check(handle->rights(), MX_RIGHT_READ))
            return up->BadHandle(handle_value, ERR_ACCESS_DENIED);

        result = wait_state_observer.Begin(&event, handle, signals, 0u);
        if (result != NO_ERROR)
            return result;
    }

    lk_time_t t = mx_time_to_lk(timeout);
    if ((timeout > 0ull) && (t == 0u))
        t = 1u;

#if WITH_LIB_KTRACE
    mxtl::RefPtr<Dispatcher> dispatcher;
    uint32_t rights;
    uint32_t koid;
    if (up->GetDispatcher(handle_value, &dispatcher, &rights)) {
        koid = (uint32_t)dispatcher->get_koid();
    } else {
        koid = 0;
    }
    ktrace(TAG_WAIT_ONE, koid, signals, (uint32_t)timeout, (uint32_t)(timeout >> 32));
#endif
    result = WaitEvent::ResultToStatus(event.Wait(t, nullptr));

    // Regardless of wait outcome, we must call End().
    auto signals_state = wait_state_observer.End();

#if WITH_LIB_KTRACE
    ktrace(TAG_WAIT_ONE_DONE, koid, signals_state.satisfied, result, 0);
#endif

    if (_signals_state) {
        if (copy_to_user(_signals_state, &signals_state, sizeof(signals_state)) != NO_ERROR)
            return ERR_INVALID_ARGS;
    }

    return result;
}

mx_status_t sys_handle_wait_many(uint32_t count,
                                 const mx_handle_t* _handle_values,
                                 const mx_signals_t* _signals,
                                 mx_time_t timeout,
                                 user_ptr<uint32_t> _result_index,
                                 user_ptr<mx_signals_state_t> _signals_states) {
    LTRACEF("count %u\n", count);

    if (!count) {
        mx_status_t result = magenta_sleep(timeout);
        if (result != NO_ERROR)
            return result;
        return ERR_TIMED_OUT;
    }

    if (!_handle_values || !_signals)
        return ERR_INVALID_ARGS;
    if (count > kMaxWaitHandleCount)
        return ERR_INVALID_ARGS;

    uint32_t max_size = kMaxWaitHandleCount * sizeof(uint32_t);
    uint32_t bytes_size = static_cast<uint32_t>(sizeof(uint32_t) * count);

    void* copy;
    status_t result;

    result = magenta_copy_user_dynamic(_handle_values, &copy, bytes_size, max_size);
    if (result != NO_ERROR)
        return result;
    mxtl::unique_ptr<int32_t[]> handle_values(reinterpret_cast<mx_handle_t*>(copy));

    result = magenta_copy_user_dynamic(_signals, &copy, bytes_size, max_size);
    if (result != NO_ERROR)
        return result;
    mxtl::unique_ptr<uint32_t[]> signals(reinterpret_cast<mx_signals_t*>(copy));

    AllocChecker ac;
    mxtl::unique_ptr<WaitStateObserver[]> wait_state_observers(new (&ac) WaitStateObserver[count]);
    if (!ac.check())
        return ERR_NO_MEMORY;

    mxtl::unique_ptr<mx_signals_state_t[]> signals_states;
    if (_signals_states) {
        signals_states.reset(new (&ac) mx_signals_state_t[count]);
        if (!ac.check())
            return ERR_NO_MEMORY;
    }

    WaitEvent event;

    // We may need to unwind (which can be done outside the lock).
    result = NO_ERROR;
    size_t num_added = 0;
    {
        auto up = ProcessDispatcher::GetCurrent();
        AutoLock lock(up->handle_table_lock());

        for (; num_added != count; ++num_added) {
            Handle* handle = up->GetHandle_NoLock(handle_values[num_added]);
            if (!handle) {
                result = up->BadHandle(handle_values[num_added], ERR_BAD_HANDLE);
                break;
            }
            if (!magenta_rights_check(handle->rights(), MX_RIGHT_READ)) {
                result = ERR_ACCESS_DENIED;
                break;
            }

            result = wait_state_observers[num_added].Begin(&event, handle, signals[num_added],
                                                           static_cast<uint64_t>(num_added));
            if (result != NO_ERROR)
                break;
        }
    }
    if (result != NO_ERROR) {
        DEBUG_ASSERT(num_added < count);
        for (size_t ix = 0; ix < num_added; ++ix)
            wait_state_observers[ix].End();
        return result;
    }

    lk_time_t t = mx_time_to_lk(timeout);
    if ((timeout > 0ull) && (t == 0u))
        t = 1u;

    uint64_t context = -1;
    WaitEvent::Result wait_event_result = event.Wait(t, &context);
    result = WaitEvent::ResultToStatus(wait_event_result);

    // Regardless of wait outcome, we must call End().
    for (size_t ix = 0; ix != count; ++ix) {
        auto s = wait_state_observers[ix].End();
        if (signals_states)
            signals_states[ix] = s;
    }

    if (_result_index && WaitEvent::HaveContextForResult(wait_event_result)) {
        if (copy_to_user_u32(_result_index, static_cast<uint32_t>(context)) != NO_ERROR)
            return ERR_INVALID_ARGS;
    }

    if (_signals_states) {
        if (copy_to_user(_signals_states, signals_states.get(),
                         sizeof(mx_signals_state_t) * count) != NO_ERROR)
            return ERR_INVALID_ARGS;
    }

    return result;
}
