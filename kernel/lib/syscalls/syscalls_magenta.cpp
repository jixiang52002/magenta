// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <assert.h>
#include <err.h>
#include <arch/ops.h>
#include <kernel/auto_lock.h>
#include <kernel/thread.h>
#include <kernel/mp.h>
#include <kernel/vm/vm_region.h>
#include <kernel/vm/vm_object.h>
#include <lib/console.h>
#include <lib/crypto/global_prng.h>
#include <lib/user_copy.h>
#include <lib/ktrace.h>
#include <list.h>
#include <mxtl/user_ptr.h>

#include <magenta/data_pipe.h>
#include <magenta/data_pipe_consumer_dispatcher.h>
#include <magenta/data_pipe_producer_dispatcher.h>
#include <magenta/event_dispatcher.h>
#include <magenta/io_port_dispatcher.h>
#include <magenta/log_dispatcher.h>
#include <magenta/magenta.h>
#include <magenta/message_pipe_dispatcher.h>
#include <magenta/process_dispatcher.h>
#include <magenta/resource_dispatcher.h>
#include <magenta/socket_dispatcher.h>
#include <magenta/state_tracker.h>
#include <magenta/thread_dispatcher.h>
#include <magenta/user_copy.h>
#include <magenta/user_thread.h>
#include <magenta/vm_object_dispatcher.h>
#include <magenta/wait_event.h>
#include <magenta/wait_set_dispatcher.h>
#include <magenta/wait_state_observer.h>

#include <inttypes.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>
#include <mxtl/string_piece.h>

#include "syscalls_priv.h"

#define LOCAL_TRACE 0

constexpr uint32_t kMaxMessageSize = 65536u;
constexpr uint32_t kMaxMessageHandles = 1024u;

constexpr uint32_t kMaxWaitHandleCount = 256u;
constexpr mx_size_t kDefaultDataPipeCapacity = 32 * 1024u;

constexpr mx_size_t kMaxCPRNGDraw = MX_CPRNG_DRAW_MAX_LEN;
constexpr mx_size_t kMaxCPRNGSeed = MX_CPRNG_ADD_ENTROPY_MAX_LEN;

constexpr uint32_t kMaxWaitSetWaitResults = 1024u;

namespace {

mx_status_t get_process(ProcessDispatcher* up,
                        mx_handle_t proc_handle,
                        mxtl::RefPtr<ProcessDispatcher>* proc) {
    if (proc_handle == 0) {
        // handle 0 is magic for 'current process'
        // TODO: remove this hack and switch to requiring user to pass the current process handle
        proc->reset(up);
        return NO_ERROR;
    }

    return up->GetDispatcher(proc_handle, proc, MX_RIGHT_WRITE);
}

} // anonymous namespace

mx_status_t validate_resource_handle(mx_handle_t handle) {
    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<ResourceDispatcher> resource;
    return up->GetDispatcher(handle, &resource);
}

void sys_exit(int retcode) {
    LTRACEF("retcode %d\n", retcode);

    ProcessDispatcher::GetCurrent()->Exit(retcode);
}

mx_status_t sys_nanosleep(mx_time_t nanoseconds) {
    LTRACEF("nseconds %llu\n", nanoseconds);

    if (nanoseconds == 0ull) {
        thread_yield();
        return NO_ERROR;
    }

    return magenta_sleep(nanoseconds);
}

uint sys_num_cpus(void) {
    return arch_max_num_cpus();
}

uint64_t sys_current_time() {
    return current_time_hires() * 1000;  // microseconds to nanoseconds
}

mx_status_t sys_handle_wait_one(mx_handle_t handle_value,
                                mx_signals_t signals,
                                mx_time_t timeout,
                                mxtl::user_ptr<mx_signals_state_t> _signals_state) {
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
                                 mxtl::user_ptr<uint32_t> _result_index,
                                 mxtl::user_ptr<mx_signals_state_t> _signals_states) {
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

mx_status_t sys_handle_close(mx_handle_t handle_value) {
    LTRACEF("handle %u\n", handle_value);
    auto up = ProcessDispatcher::GetCurrent();
    HandleUniquePtr handle(up->RemoveHandle(handle_value));
    if (!handle)
        return up->BadHandle(handle_value, ERR_BAD_HANDLE);
    return NO_ERROR;
}

mx_handle_t sys_handle_duplicate(mx_handle_t handle_value, mx_rights_t rights) {
    LTRACEF("handle %u\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    mx_handle_t dup_hv;

    {
        AutoLock lock(up->handle_table_lock());
        Handle* source = up->GetHandle_NoLock(handle_value);
        if (!source)
            return up->BadHandle(handle_value, ERR_BAD_HANDLE);

        if (!magenta_rights_check(source->rights(), MX_RIGHT_DUPLICATE))
            return up->BadHandle(handle_value, ERR_ACCESS_DENIED);

        HandleUniquePtr dest;
        if (rights == MX_RIGHT_SAME_RIGHTS) {
            dest.reset(DupHandle(source, source->rights()));
        } else {
            if ((source->rights() & rights) != rights)
                return ERR_INVALID_ARGS;
            dest.reset(DupHandle(source, rights));
        }
        if (!dest)
            return ERR_NO_MEMORY;

        dup_hv = up->MapHandleToValue(dest.get());
        up->AddHandle_NoLock(mxtl::move(dest));
    }

    return dup_hv;
}

mx_handle_t sys_handle_replace(mx_handle_t handle_value, mx_rights_t rights) {
    LTRACEF("handle %u\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    HandleUniquePtr source;
    mx_handle_t replacement_hv;

    {
        AutoLock lock(up->handle_table_lock());
        source = up->RemoveHandle_NoLock(handle_value);
        if (!source)
            return up->BadHandle(handle_value, ERR_BAD_HANDLE);

        HandleUniquePtr dest;
        // Used only if |dest| doesn't (successfully) get set below.
        mx_status_t error = ERR_NO_MEMORY;
        if (rights == MX_RIGHT_SAME_RIGHTS) {
            dest.reset(DupHandle(source.get(), source->rights()));
        } else {
            if ((source->rights() & rights) != rights)
                error = ERR_INVALID_ARGS;
            else
                dest.reset(DupHandle(source.get(), rights));
        }

        if (!dest) {
            // Unwind: put |source| back!
            up->AddHandle_NoLock(mxtl::move(source));
            return error;
        }

        replacement_hv = up->MapHandleToValue(dest.get());
        up->AddHandle_NoLock(mxtl::move(dest));
    }

    return replacement_hv;
}

mx_ssize_t sys_object_get_info(mx_handle_t handle, uint32_t topic, uint16_t topic_size,
                            mxtl::user_ptr<void> _buffer, mx_size_t buffer_size) {
    auto up = ProcessDispatcher::GetCurrent();

    LTRACEF("handle %u topic %u topic_size %u buffer %p buffer_size %lu\n",
            handle, topic, topic_size, _buffer.get(), buffer_size);

    switch (topic) {
        case MX_INFO_HANDLE_VALID: {
            mxtl::RefPtr<Dispatcher> dispatcher;
            uint32_t rights;

            // test that the handle is valid at all, return error if it's not
            if (!up->GetDispatcher(handle, &dispatcher, &rights))
                return ERR_BAD_HANDLE;
            return NO_ERROR;
        }
        case MX_INFO_HANDLE_BASIC: {
            mxtl::RefPtr<Dispatcher> dispatcher;
            uint32_t rights;

            if (!up->GetDispatcher(handle, &dispatcher, &rights))
                return up->BadHandle(handle, ERR_BAD_HANDLE);

            // test that they've asking for an appropriate version
            if (topic_size != 0 && topic_size != sizeof(mx_record_handle_basic_t))
                return ERR_INVALID_ARGS;

            // make sure they passed us a buffer
            if (!_buffer)
                return ERR_INVALID_ARGS;

            // test that we have at least enough target buffer to support the header and one record
            if (buffer_size < sizeof(mx_info_header_t) + topic_size)
                return ERR_NOT_ENOUGH_BUFFER;

            // build the info structure
            mx_info_handle_basic_t info = {};

            // fill in the header
            info.hdr.topic = topic;
            info.hdr.avail_topic_size = sizeof(info.rec);
            info.hdr.topic_size = topic_size;
            info.hdr.avail_count = 1;
            info.hdr.count = 1;

            mx_size_t tocopy;
            if (topic_size == 0) {
                // just copy the header
                tocopy = sizeof(info.hdr);
            } else {
                bool waitable = dispatcher->get_state_tracker() &&
                            dispatcher->get_state_tracker()->is_waitable();

                // copy the header and the record
                info.rec.koid = dispatcher->get_koid();
                info.rec.rights = rights;
                info.rec.type = dispatcher->get_type();
                info.rec.props = waitable ? MX_OBJ_PROP_WAITABLE : MX_OBJ_PROP_NONE;

                tocopy = sizeof(info);
            }

            if (copy_to_user(_buffer.reinterpret<uint8_t>(), &info, tocopy) != NO_ERROR)
                return ERR_INVALID_ARGS;

            return tocopy;
        }
        case MX_INFO_PROCESS: {
            // grab a reference to the dispatcher
            mxtl::RefPtr<ProcessDispatcher> process;
            auto error = up->GetDispatcher<ProcessDispatcher>(handle, &process, MX_RIGHT_READ);
            if (error < 0)
                return error;

            // test that they've asking for an appropriate version
            if (topic_size != 0 && topic_size != sizeof(mx_record_process_t))
                return ERR_INVALID_ARGS;

            // make sure they passed us a buffer
            if (!_buffer)
                return ERR_INVALID_ARGS;

            // test that we have at least enough target buffer to support the header and one record
            if (buffer_size < sizeof(mx_info_header_t) + topic_size)
                return ERR_NOT_ENOUGH_BUFFER;

            // build the info structure
            mx_info_process_t info = {};

            // fill in the header
            info.hdr.topic = topic;
            info.hdr.avail_topic_size = sizeof(info.rec);
            info.hdr.topic_size = topic_size;
            info.hdr.avail_count = 1;
            info.hdr.count = 1;

            mx_size_t tocopy;
            if (topic_size == 0) {
                // just copy the header
                tocopy = sizeof(info.hdr);
            } else {
                auto err = process->GetInfo(&info.rec);
                if (err != NO_ERROR)
                    return err;

                tocopy = sizeof(info);
            }

            if (copy_to_user(_buffer.reinterpret<uint8_t>(), &info, tocopy) != NO_ERROR)
                return ERR_INVALID_ARGS;

            return tocopy;
        }
        default:
            return ERR_NOT_FOUND;
    }
}

mx_status_t sys_object_get_property(mx_handle_t handle_value, uint32_t property,
                                    mxtl::user_ptr<void> _value, mx_size_t size) {
    if (!_value)
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<Dispatcher> dispatcher;
    uint32_t rights;

    if (!up->GetDispatcher(handle_value, &dispatcher, &rights))
        return up->BadHandle(handle_value, ERR_BAD_HANDLE);

    // TODO:cpu use 'get-info' rights when avaliable.
    if (!magenta_rights_check(rights, MX_RIGHT_READ))
        return ERR_ACCESS_DENIED;

    switch (property) {
        case MX_PROP_BAD_HANDLE_POLICY: {
            if (size != sizeof(uint32_t))
                return ERR_NOT_ENOUGH_BUFFER;
            auto process = dispatcher->get_specific<ProcessDispatcher>();
            if (!process)
                return ERR_WRONG_TYPE;
            uint32_t value = process->get_bad_handle_policy();
            if (copy_to_user_u32(_value.reinterpret<uint32_t>(), value) != NO_ERROR)
                return ERR_INVALID_ARGS;
            return NO_ERROR;
        }
        default:
            return ERR_INVALID_ARGS;
    }

    __UNREACHABLE;
}

mx_status_t sys_object_set_property(mx_handle_t handle_value, uint32_t property,
                                    mxtl::user_ptr<const void> _value, mx_size_t size) {
    if (!_value)
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<Dispatcher> dispatcher;
    uint32_t rights;

    if (!up->GetDispatcher(handle_value, &dispatcher, &rights))
        return up->BadHandle(handle_value, ERR_BAD_HANDLE);

    // TODO:cpu use 'set-info' rights when avaliable.
    if (!magenta_rights_check(rights, MX_RIGHT_READ))
        return up->BadHandle(handle_value, ERR_ACCESS_DENIED);

    mx_status_t status = ERR_INVALID_ARGS;

    switch (property) {
        case MX_PROP_BAD_HANDLE_POLICY: {
            if (size < sizeof(uint32_t))
                return ERR_NOT_ENOUGH_BUFFER;
            auto process = dispatcher->get_specific<ProcessDispatcher>();
            if (!process)
                return up->BadHandle(handle_value, ERR_WRONG_TYPE);
            uint32_t value = 0;
            if (copy_from_user_u32(&value, _value.reinterpret<const uint32_t>()) != NO_ERROR)
                return ERR_INVALID_ARGS;
            status = process->set_bad_handle_policy(value);
            break;
        }
    }

    return status;
}

mx_status_t sys_msgpipe_read(mx_handle_t handle_value, mxtl::user_ptr<void> _bytes,
                             mxtl::user_ptr<uint32_t> _num_bytes, mxtl::user_ptr<mx_handle_t> _handles,
                             mxtl::user_ptr<uint32_t> _num_handles, uint32_t flags) {
    LTRACEF("handle %d bytes %p num_bytes %p handles %p num_handles %p flags 0x%x\n",
            handle_value, _bytes.get(), _num_bytes.get(), _handles.get(), _num_handles.get(), flags);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<MessagePipeDispatcher> msg_pipe;
    mx_status_t status = up->GetDispatcher(handle_value, &msg_pipe,
                                           MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    uint32_t num_bytes = 0;
    uint32_t num_handles = 0;

    if (_num_bytes) {
        if (copy_from_user_u32(&num_bytes, _num_bytes) != NO_ERROR)
            return ERR_INVALID_ARGS;
    }

    if (_num_handles) {
        if (copy_from_user_u32(&num_handles, _num_handles) != NO_ERROR)
            return ERR_INVALID_ARGS;
    }

    if (_bytes != 0u && !_num_bytes)
        return ERR_INVALID_ARGS;
    if (_handles != 0u && !_num_handles)
        return ERR_INVALID_ARGS;

    mxtl::unique_ptr<uint32_t[]> handles;

    AllocChecker ac;
    if (num_handles) {
        handles.reset(new (&ac) uint32_t[num_handles]());
        if (!ac.check())
            return ERR_NO_MEMORY;
    }

    uint32_t next_message_size = 0u;
    uint32_t next_message_num_handles = 0u;
    status_t result = msg_pipe->BeginRead(&next_message_size, &next_message_num_handles);
    if (result != NO_ERROR)
        return result;

    // Always set the actual size and handle count so the caller can provide larger buffers.
    if (_num_bytes) {
        if (copy_to_user_u32(_num_bytes, next_message_size) != NO_ERROR)
            return ERR_INVALID_ARGS;
    }
    if (_num_handles) {
        if (copy_to_user_u32(_num_handles, next_message_num_handles) != NO_ERROR)
            return ERR_INVALID_ARGS;
    }

    // If the caller provided buffers are too small, abort the read so the caller can try again.
    if (num_bytes < next_message_size || num_handles < next_message_num_handles)
        return ERR_NOT_ENOUGH_BUFFER;

    // OK, now we can accept the message.
    mxtl::Array<uint8_t> bytes;
    mxtl::Array<Handle*> handle_list;

    result = msg_pipe->AcceptRead(&bytes, &handle_list);

    if (_bytes) {
        if (copy_to_user(_bytes.reinterpret<uint8_t>(), bytes.get(), num_bytes) != NO_ERROR) {
            // $$$ free handles.
            return ERR_INVALID_ARGS;
        }
    }

    if (next_message_num_handles != 0u) {
        for (size_t ix = 0u; ix < next_message_num_handles; ++ix) {
            auto hv = up->MapHandleToValue(handle_list[ix]);
            if (copy_to_user_32_unsafe(&_handles.get()[ix], hv) != NO_ERROR) {
                // $$$ free handles.
                return ERR_INVALID_ARGS;
            }
        }
    }

    for (size_t idx = 0u; idx < next_message_num_handles; ++idx) {
        if (handle_list[idx]->dispatcher()->get_state_tracker())
            handle_list[idx]->dispatcher()->get_state_tracker()->Cancel(handle_list[idx]);
        HandleUniquePtr handle(handle_list[idx]);
        up->AddHandle(mxtl::move(handle));
    }

    ktrace(TAG_MSGPIPE_READ, (uint32_t)msg_pipe->get_koid(),
           next_message_size, next_message_num_handles, 0);
    return result;
}

mx_status_t sys_msgpipe_write(mx_handle_t handle_value, mxtl::user_ptr<const void> _bytes, uint32_t num_bytes,
                              mxtl::user_ptr<const mx_handle_t> _handles, uint32_t num_handles, uint32_t flags) {
    LTRACEF("handle %d bytes %p num_bytes %u handles %p num_handles %u flags 0x%x\n",
            handle_value, _bytes.get(), num_bytes, _handles.get(), num_handles, flags);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<MessagePipeDispatcher> msg_pipe;
    mx_status_t status = up->GetDispatcher(handle_value, &msg_pipe,
                                           MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    bool is_reply_pipe = msg_pipe->is_reply_pipe();

    if (num_bytes != 0u && !_bytes)
        return ERR_INVALID_ARGS;
    if (num_handles != 0u && !_handles)
        return ERR_INVALID_ARGS;

    if (num_bytes > kMaxMessageSize)
        return ERR_TOO_BIG;
    if (num_handles > kMaxMessageHandles)
        return ERR_TOO_BIG;

    status_t result;
    mxtl::Array<uint8_t> bytes;

    if (num_bytes) {
        void* copy;
        result = magenta_copy_user_dynamic(_bytes.get(), &copy, num_bytes, kMaxMessageSize);
        if (result != NO_ERROR)
            return result;
        bytes.reset(reinterpret_cast<uint8_t*>(copy), num_bytes);
    }

    mxtl::unique_ptr<mx_handle_t[], mxtl::free_delete> handles;
    if (num_handles) {
        void* c_handles;
        status_t status = magenta_copy_user_dynamic(
            _handles.reinterpret<const void>().get(),
            &c_handles,
            num_handles * sizeof(_handles.get()[0]),
            kMaxMessageHandles);
        // |status| can be ERR_NO_MEMORY or ERR_INVALID_ARGS.
        if (status != NO_ERROR)
            return status;

        handles.reset(static_cast<mx_handle_t*>(c_handles));
    }

    AllocChecker ac;
    mxtl::Array<Handle*> handle_list(new (&ac) Handle*[num_handles], num_handles);
    if (!ac.check())
        return ERR_NO_MEMORY;

    {
        // Loop twice, first we collect and validate handles, the second pass
        // we remove them from this process.
        AutoLock lock(up->handle_table_lock());

        size_t reply_pipe_found = -1;

        for (size_t ix = 0; ix != num_handles; ++ix) {
            auto handle = up->GetHandle_NoLock(handles[ix]);
            if (!handle)
                return up->BadHandle(handles[ix], ERR_BAD_HANDLE);

            if (handle->dispatcher().get() == static_cast<Dispatcher*>(msg_pipe.get())) {
                // Found itself, which is only allowed for MX_FLAG_REPLY_PIPE (aka Reply) pipes.
                if (!is_reply_pipe) {
                    return ERR_NOT_SUPPORTED;
                } else {
                    reply_pipe_found = ix;
                }
            }

            if (!magenta_rights_check(handle->rights(), MX_RIGHT_TRANSFER))
                return up->BadHandle(handles[ix], ERR_ACCESS_DENIED);

            handle_list[ix] = handle;
        }

        if (is_reply_pipe) {
            // For reply pipes, itself must be in the handle array and be the last handle.
            if ((num_handles == 0) || (reply_pipe_found != (num_handles - 1)))
                return ERR_BAD_STATE;
        }

        for (size_t ix = 0; ix != num_handles; ++ix) {
            auto handle = up->RemoveHandle_NoLock(handles[ix]).release();
            // Passing duplicate handles is not allowed.
            // If we've already seen this handle flag an error.
            if (!handle) {
                // Put back the handles we've already removed.
                for (size_t idx = 0; idx < ix; ++idx) {
                    up->UndoRemoveHandle_NoLock(handles[idx]);
                }
                // TODO: more specific error?
                return ERR_INVALID_ARGS;
            }
        }
    }

    result = msg_pipe->Write(mxtl::move(bytes), mxtl::move(handle_list));

    if (result != NO_ERROR) {
        // Write failed, put back the handles into this process.
        AutoLock lock(up->handle_table_lock());
        for (size_t ix = 0; ix != num_handles; ++ix) {
            up->UndoRemoveHandle_NoLock(handles[ix]);
        }
    }

    ktrace(TAG_MSGPIPE_WRITE, (uint32_t)msg_pipe->get_koid(), num_bytes, num_handles, 0);
    return result;
}

mx_status_t sys_msgpipe_create(mxtl::user_ptr<mx_handle_t> out_handle /* array of size 2 */,
                               uint32_t flags) {
    LTRACEF("entry out_handle[] %p\n", out_handle.get());

    if (!out_handle)
        return ERR_INVALID_ARGS;

    if ((flags != 0u) && (flags != MX_FLAG_REPLY_PIPE))
        return ERR_INVALID_ARGS;

    mxtl::RefPtr<Dispatcher> mpd0, mpd1;
    mx_rights_t rights;
    status_t result = MessagePipeDispatcher::Create(flags, &mpd0, &mpd1, &rights);
    if (result != NO_ERROR)
        return result;

    uint64_t id0 = mpd0->get_koid();
    uint64_t id1 = mpd1->get_koid();

    HandleUniquePtr h0(MakeHandle(mxtl::move(mpd0), rights));
    if (!h0)
        return ERR_NO_MEMORY;

    HandleUniquePtr h1(MakeHandle(mxtl::move(mpd1), rights));
    if (!h1)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();
    mx_handle_t hv[2] = {up->MapHandleToValue(h0.get()), up->MapHandleToValue(h1.get())};

    if (copy_to_user(out_handle, hv, sizeof(mx_handle_t) * 2) != NO_ERROR)
        return ERR_INVALID_ARGS;

    up->AddHandle(mxtl::move(h0));
    up->AddHandle(mxtl::move(h1));

    ktrace(TAG_MSGPIPE_CREATE, (uint32_t)id0, (uint32_t)id1, flags, 0);

    LTRACE_EXIT;
    return NO_ERROR;
}

mx_handle_t sys_thread_create(mx_handle_t process_handle, mxtl::user_ptr<const char> name, uint32_t name_len, uint32_t flags) {
    LTRACEF("flags 0x%x\n", flags);

    // copy the name to a local buffer
    char buf[MX_MAX_NAME_LEN];
    mxtl::StringPiece sp;
    status_t result = magenta_copy_user_string(name.get(), name_len, buf, sizeof(buf), &sp);
    if (result != NO_ERROR)
        return result;

    // currently, the only valid flag value is 0
    if (flags != 0)
        return ERR_INVALID_ARGS;

    // convert process handle to process dispatcher
    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<ProcessDispatcher> process;
    result = get_process(up, process_handle, &process);
    if (result != NO_ERROR)
        return result;

    // create the thread object
    mxtl::RefPtr<UserThread> user_thread;
    result = process->CreateUserThread(sp.data(), flags, &user_thread);
    if (result != NO_ERROR)
        return result;

    // create the thread dispatcher
    mxtl::RefPtr<Dispatcher> thread_dispatcher;
    mx_rights_t thread_rights;
    result = ThreadDispatcher::Create(mxtl::move(user_thread), &thread_dispatcher, &thread_rights);
    if (result != NO_ERROR)
        return result;

    uint32_t koid = (uint32_t)thread_dispatcher->get_koid();
    ktrace(TAG_THREAD_CREATE, koid, (uint32_t)process->get_koid(), 0, 0);
    ktrace_name(TAG_THREAD_NAME, koid, buf);

    HandleUniquePtr handle(MakeHandle(mxtl::move(thread_dispatcher), thread_rights));
    if (!handle)
        return ERR_NO_MEMORY;

    mx_handle_t hv = up->MapHandleToValue(handle.get());
    up->AddHandle(mxtl::move(handle));

    return hv;
}

mx_status_t sys_thread_start(mx_handle_t thread_handle, uintptr_t entry,
                             uintptr_t stack, uintptr_t arg1, uintptr_t arg2) {
    LTRACEF("handle %#x, entry %#" PRIxPTR ", sp %#" PRIxPTR
            ", arg1 %#" PRIxPTR ", arg2 %#" PRIxPTR "\n",
            thread_handle, entry, stack, arg1, arg2);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<ThreadDispatcher> thread;
    mx_status_t status = up->GetDispatcher(thread_handle, &thread,
                                           MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    ktrace(TAG_THREAD_START, (uint32_t)thread->get_koid(), 0, 0, 0);
    return thread->Start(entry, stack, arg1, arg2);
}

void sys_thread_exit() {
    LTRACE_ENTRY;
    UserThread::GetCurrent()->Exit();
}

extern "C" {
uint64_t get_tsc_ticks_per_ms(void);
};

mx_status_t sys_thread_arch_prctl(mx_handle_t handle_value, uint32_t op,
                                  mxtl::user_ptr<uintptr_t> value_ptr) {
    LTRACEF("handle %u operation %u value_ptr %p", handle_value, op, value_ptr.get());

    // TODO(cpu) what to do with |handle_value|?

    uintptr_t value;

    switch (op) {
#ifdef ARCH_X86_64
    case ARCH_SET_FS:
        if (copy_from_user_uptr(&value, value_ptr) != NO_ERROR)
            return ERR_INVALID_ARGS;
        if (!x86_is_vaddr_canonical(value))
            return ERR_INVALID_ARGS;
        write_msr(X86_MSR_IA32_FS_BASE, value);
        break;
    case ARCH_GET_FS:
        value = read_msr(X86_MSR_IA32_FS_BASE);
        if (copy_to_user_uptr(value_ptr, value) != NO_ERROR)
            return ERR_INVALID_ARGS;
        break;
    case ARCH_SET_GS:
        if (copy_from_user_uptr(&value, value_ptr) != NO_ERROR)
            return ERR_INVALID_ARGS;
        if (!x86_is_vaddr_canonical(value))
            return ERR_INVALID_ARGS;
        write_msr(X86_MSR_IA32_KERNEL_GS_BASE, value);
        break;
    case ARCH_GET_GS:
        value = read_msr(X86_MSR_IA32_KERNEL_GS_BASE);
        if (copy_to_user_uptr(value_ptr, value) != NO_ERROR)
            return ERR_INVALID_ARGS;
        break;
    case ARCH_GET_TSC_TICKS_PER_MS:
        value = get_tsc_ticks_per_ms();
        if (copy_to_user_uptr(value_ptr, value) != NO_ERROR)
            return ERR_INVALID_ARGS;
        break;
#elif ARCH_ARM64
    case ARCH_SET_TPIDRRO_EL0:
        if (copy_from_user_uptr(&value, value_ptr) != NO_ERROR)
            return ERR_INVALID_ARGS;
        ARM64_WRITE_SYSREG(tpidrro_el0, value);
        break;
#elif ARCH_ARM
    case ARCH_SET_CP15_READONLY:
        if (copy_from_user_uptr(&value, value_ptr) != NO_ERROR)
            return ERR_INVALID_ARGS;
        __asm__ volatile("mcr p15, 0, %0, c13, c0, 3" : : "r" (value));
        ISB;
        break;
#endif
    default:
        return ERR_INVALID_ARGS;
    }

    return NO_ERROR;
}

mx_handle_t sys_process_create(mxtl::user_ptr<const char> name, uint32_t name_len, uint32_t flags) {
    LTRACEF("name %p, flags 0x%x\n", name.get(), flags);

    // copy out the name
    char buf[MX_MAX_NAME_LEN];
    mxtl::StringPiece sp;
    status_t result = magenta_copy_user_string(name.get(), name_len, buf, sizeof(buf), &sp);
    if (result != NO_ERROR)
        return result;
    LTRACEF("name %s\n", buf);

    // currently, the only valid flag value is 0
    if (flags != 0)
        return ERR_INVALID_ARGS;

    // create a new process dispatcher
    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    status_t res = ProcessDispatcher::Create(sp, &dispatcher, &rights, flags);
    if (res != NO_ERROR)
        return res;

    uint32_t koid = (uint32_t)dispatcher->get_koid();
    ktrace(TAG_PROC_CREATE, koid, 0, 0, 0);
    ktrace_name(TAG_PROC_NAME, koid, buf);

    HandleUniquePtr handle(MakeHandle(mxtl::move(dispatcher), rights));
    if (!handle)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();
    mx_handle_t hv = up->MapHandleToValue(handle.get());
    up->AddHandle(mxtl::move(handle));

    return hv;
}

mx_status_t sys_process_start(mx_handle_t process_handle, mx_handle_t thread_handle, uintptr_t pc, uintptr_t sp, mx_handle_t arg_handle_value, uintptr_t arg2) {
    LTRACEF("phandle %#x, thandle %#x, pc %#" PRIxPTR ", sp %#" PRIxPTR
            ", arg_handle %#x, arg2 %#" PRIxPTR "\n",
            process_handle, thread_handle, pc, sp, arg_handle_value, arg2);

    auto up = ProcessDispatcher::GetCurrent();

    // get process dispatcher
    mxtl::RefPtr<ProcessDispatcher> process;
    mx_status_t status = get_process(up, process_handle, &process);
    if (status != NO_ERROR)
        return status;

    // get thread_dispatcher
    mxtl::RefPtr<ThreadDispatcher> thread;
    status = up->GetDispatcher(thread_handle, &thread, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    // test that the thread belongs to the starting process
    if (thread->thread()->process() != process.get())
        return ERR_ACCESS_DENIED;

    // XXX test that handle has TRANSFER rights before we remove it from the source process

    HandleUniquePtr arg_handle = up->RemoveHandle(arg_handle_value);
    if (!arg_handle)
        return ERR_INVALID_ARGS;

    auto arg_nhv = process->MapHandleToValue(arg_handle.get());
    process->AddHandle(mxtl::move(arg_handle));

    // TODO(cpu) if Start() fails we want to undo RemoveHandle().

    ktrace(TAG_PROC_START, (uint32_t)thread->get_koid(),
           (uint32_t)process->get_koid(), 0, 0);

    return process->Start(mxtl::move(thread), pc, sp, arg_nhv, arg2);
}

// helper routine for sys_task_kill
template <typename T>
static mx_status_t kill_task(mxtl::RefPtr<Dispatcher> dispatcher, uint32_t rights) {
    auto task = dispatcher->get_specific<T>();
    if (!task)
        return ERR_WRONG_TYPE;

    if (!magenta_rights_check(rights, MX_RIGHT_WRITE))
        return ERR_ACCESS_DENIED;

    task->Kill();
    return NO_ERROR;
}

mx_status_t sys_task_kill(mx_handle_t task_handle) {
    LTRACEF("handle %#x\n", task_handle);

    auto up = ProcessDispatcher::GetCurrent();

    // get dispatcher to the handle passed in
    // use the bool version of GetDispatcher to just get a raw dispatcher
    mxtl::RefPtr<Dispatcher> dispatcher;
    uint32_t rights;
    if (!up->GetDispatcher(task_handle, &dispatcher, &rights))
        return up->BadHandle(task_handle, ERR_BAD_HANDLE);

    // see if it's a process or thread and dispatch accordingly
    switch (dispatcher->get_type()) {
        case MX_OBJ_TYPE_PROCESS:
            return kill_task<ProcessDispatcher>(mxtl::move(dispatcher), rights);
        case MX_OBJ_TYPE_THREAD:
            return kill_task<ThreadDispatcher>(mxtl::move(dispatcher), rights);
        default:
            return ERR_WRONG_TYPE;
    }
}

mx_handle_t sys_event_create(uint32_t options) {
    LTRACEF("options 0x%x\n", options);

    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;

    status_t result = EventDispatcher::Create(options, &dispatcher, &rights);
    if (result != NO_ERROR)
        return result;

    HandleUniquePtr handle(MakeHandle(mxtl::move(dispatcher), rights));
    if (!handle)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    mx_handle_t hv = up->MapHandleToValue(handle.get());
    up->AddHandle(mxtl::move(handle));
    return hv;
}

mx_status_t sys_object_signal(mx_handle_t handle_value, uint32_t clear_mask, uint32_t set_mask) {
    LTRACEF("handle %u\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<Dispatcher> dispatcher;
    uint32_t rights;

    if (!up->GetDispatcher(handle_value, &dispatcher, &rights))
        return up->BadHandle(handle_value, ERR_BAD_HANDLE);
    if (!magenta_rights_check(rights, MX_RIGHT_WRITE))
        return up->BadHandle(handle_value, ERR_ACCESS_DENIED);

    return dispatcher->UserSignal(clear_mask, set_mask);
}

mx_status_t sys_futex_wait(int* value_ptr, int current_value, mx_time_t timeout) {
    return ProcessDispatcher::GetCurrent()->futex_context()->FutexWait(value_ptr, current_value, timeout);
}

mx_status_t sys_futex_wake(int* value_ptr, uint32_t count) {
    return ProcessDispatcher::GetCurrent()->futex_context()->FutexWake(value_ptr, count);
}

mx_status_t sys_futex_requeue(int* wake_ptr, uint32_t wake_count, int current_value,
                              int* requeue_ptr, uint32_t requeue_count) {
    return ProcessDispatcher::GetCurrent()->futex_context()->FutexRequeue(
        wake_ptr, wake_count, current_value, requeue_ptr, requeue_count);
}

mx_handle_t sys_vmo_create(uint64_t size) {
    LTRACEF("size 0x%llx\n", size);

    // create a vm object
    mxtl::RefPtr<VmObject> vmo = VmObject::Create(0, size);
    if (!vmo)
        return ERR_NO_MEMORY;

    // create a Vm Object dispatcher
    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    mx_status_t result = VmObjectDispatcher::Create(mxtl::move(vmo), &dispatcher, &rights);
    if (result != NO_ERROR)
        return result;

    // create a handle and attach the dispatcher to it
    HandleUniquePtr handle(MakeHandle(mxtl::move(dispatcher), rights));
    if (!handle)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    mx_handle_t hv = up->MapHandleToValue(handle.get());
    up->AddHandle(mxtl::move(handle));

    return hv;
}

mx_ssize_t sys_vmo_read(mx_handle_t handle, void* data, uint64_t offset, mx_size_t len) {
    LTRACEF("handle %d, data %p, offset 0x%llx, len 0x%lx\n", handle, data, offset, len);

    auto up = ProcessDispatcher::GetCurrent();

    // lookup the dispatcher from handle
    mxtl::RefPtr<VmObjectDispatcher> vmo;
    mx_status_t status = up->GetDispatcher(handle, &vmo, MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    // do the read operation
    return vmo->Read(data, len, offset);
}

mx_ssize_t sys_vmo_write(mx_handle_t handle, const void* data, uint64_t offset, mx_size_t len) {
    LTRACEF("handle %d, data %p, offset 0x%llx, len 0x%lx\n", handle, data, offset, len);

    auto up = ProcessDispatcher::GetCurrent();

    // lookup the dispatcher from handle
    mxtl::RefPtr<VmObjectDispatcher> vmo;
    mx_status_t status = up->GetDispatcher(handle, &vmo, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    // do the write operation
    return vmo->Write(data, len, offset);
}

mx_status_t sys_vmo_get_size(mx_handle_t handle, mxtl::user_ptr<uint64_t> _size) {
    LTRACEF("handle %d, sizep %p\n", handle, _size.get());

    auto up = ProcessDispatcher::GetCurrent();

    // lookup the dispatcher from handle
    mxtl::RefPtr<VmObjectDispatcher> vmo;
    mx_status_t status = up->GetDispatcher(handle, &vmo);
    if (status != NO_ERROR)
        return status;

    // no rights check, anyone should be able to get the size

    // do the operation
    uint64_t size = 0;
    status = vmo->GetSize(&size);

    // copy the size back, even if it failed
    if (copy_to_user(_size.reinterpret<uint8_t>(), &size, sizeof(size)) != NO_ERROR)
        return ERR_INVALID_ARGS;

    return status;
}

mx_status_t sys_vmo_set_size(mx_handle_t handle, uint64_t size) {
    LTRACEF("handle %d, size 0x%llx\n", handle, size);

    auto up = ProcessDispatcher::GetCurrent();

    // lookup the dispatcher from handle
    mxtl::RefPtr<VmObjectDispatcher> vmo;
    mx_status_t status = up->GetDispatcher(handle, &vmo, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    // do the operation
    return vmo->SetSize(size);
}

mx_status_t sys_process_map_vm(mx_handle_t proc_handle, mx_handle_t vmo_handle,
                               uint64_t offset, mx_size_t len, mxtl::user_ptr<uintptr_t> user_ptr,
                               uint32_t flags) {

    LTRACEF("proc handle %d, vmo handle %d, offset 0x%llx, len 0x%lx, user_ptr %p, flags 0x%x\n",
            proc_handle, vmo_handle, offset, len, user_ptr.get(), flags);

    // current process
    auto up = ProcessDispatcher::GetCurrent();

    // get the vmo dispatcher
    mxtl::RefPtr<VmObjectDispatcher> vmo;
    uint32_t vmo_rights;
    mx_status_t status = up->GetDispatcher(vmo_handle, &vmo, &vmo_rights);
    if (status != NO_ERROR)
        return status;

    // get process dispatcher
    mxtl::RefPtr<ProcessDispatcher> process;
    status = get_process(up, proc_handle, &process);
    if (status != NO_ERROR)
        return status;

    // copy the user pointer in
    uintptr_t ptr;
    if (copy_from_user_uptr(&ptr, user_ptr) != NO_ERROR)
        return ERR_INVALID_ARGS;

    // do the map call
    status = process->Map(mxtl::move(vmo), vmo_rights,
                          offset, len, &ptr, flags);
    if (status != NO_ERROR)
        return status;

    // copy the user pointer back
    if (copy_to_user_uptr(user_ptr, ptr) != NO_ERROR)
        return ERR_INVALID_ARGS;

    return NO_ERROR;
}

mx_status_t sys_process_unmap_vm(mx_handle_t proc_handle, uintptr_t address, mx_size_t len) {
    LTRACEF("proc handle %d, address 0x%lx, len 0x%lx\n", proc_handle, address, len);

    // current process
    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<ProcessDispatcher> proc;
    auto status = get_process(up, proc_handle, &proc);
    if (status != NO_ERROR)
        return status;

    return proc->Unmap(address, len);
}

mx_status_t sys_process_protect_vm(mx_handle_t proc_handle, uintptr_t address, mx_size_t len,
                                   uint32_t prot) {
    LTRACEF("proc handle %d, address 0x%lx, len 0x%lx, prot 0x%x\n", proc_handle, address, len, prot);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<ProcessDispatcher> process;
    mx_status_t status = get_process(up, proc_handle, &process);
    if (status != NO_ERROR)
        return status;

    // get a reffed pointer to the address space in the target process
    mxtl::RefPtr<VmAspace> aspace = process->aspace();
    if (!aspace)
        return ERR_INVALID_ARGS;

    // TODO: support range protect
    // at the moment only support protecting what is at a given address, signalled with len = 0
    if (len != 0)
        return ERR_INVALID_ARGS;

    auto r = aspace->FindRegion(address);
    if (!r)
        return ERR_INVALID_ARGS;

    uint arch_mmu_flags = ARCH_MMU_FLAG_PERM_USER;
    switch (prot & (MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE)) {
    case MX_VM_FLAG_PERM_READ:
        arch_mmu_flags |= ARCH_MMU_FLAG_PERM_READ;
        break;
    case MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE:
        arch_mmu_flags |= ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE;
        break;
    case 0: // no way to express no permissions
    case MX_VM_FLAG_PERM_WRITE:
        // no way to express write only
        return ERR_INVALID_ARGS;
    }

    if (prot & MX_VM_FLAG_PERM_EXECUTE) {
        arch_mmu_flags |= ARCH_MMU_FLAG_PERM_EXECUTE;
    }

    return r->Protect(arch_mmu_flags);
}

int sys_log_create(uint32_t flags) {
    LTRACEF("flags 0x%x\n", flags);

    // kernel flag is forbidden to userspace
    flags &= (~DLOG_FLAG_KERNEL);

    // create a Log dispatcher
    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    mx_status_t result = LogDispatcher::Create(flags, &dispatcher, &rights);
    if (result != NO_ERROR)
        return result;

    // by default log objects are write-only
    // as readable logs are more expensive
    if (flags & MX_LOG_FLAG_READABLE) {
        rights |= MX_RIGHT_READ;
    }

    // create a handle and attach the dispatcher to it
    HandleUniquePtr handle(MakeHandle(mxtl::move(dispatcher), rights));
    if (!handle)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    mx_handle_t hv = up->MapHandleToValue(handle.get());
    up->AddHandle(mxtl::move(handle));

    return hv;
}

int sys_log_write(mx_handle_t log_handle, uint32_t len, mxtl::user_ptr<const void> ptr, uint32_t flags) {
    LTRACEF("log handle %d, len 0x%x, ptr 0x%p\n", log_handle, len, ptr.get());

    if (len > DLOG_MAX_ENTRY)
        return ERR_TOO_BIG;

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<LogDispatcher> log;
    mx_status_t status = up->GetDispatcher(log_handle, &log, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    char buf[DLOG_MAX_ENTRY];
    if (magenta_copy_from_user(ptr.get(), buf, len) != NO_ERROR)
        return ERR_INVALID_ARGS;

    return log->Write(buf, len, flags);
}

int sys_log_read(mx_handle_t log_handle, uint32_t len, mxtl::user_ptr<void> ptr, uint32_t flags) {
    LTRACEF("log handle %d, len 0x%x, ptr 0x%p\n", log_handle, len, ptr.get());

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<LogDispatcher> log;
    mx_status_t status = up->GetDispatcher(log_handle, &log, MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    return log->ReadFromUser(ptr.get(), len, flags);
}

mx_handle_t sys_port_create(uint32_t options) {
    LTRACEF("options %u\n", options);

    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    mx_status_t result = IOPortDispatcher::Create(options, &dispatcher, &rights);
    if (result != NO_ERROR)
        return result;

    uint32_t koid = (uint32_t)dispatcher->get_koid();

    HandleUniquePtr handle(MakeHandle(mxtl::move(dispatcher), rights));
    if (!handle)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    mx_handle_t hv = up->MapHandleToValue(handle.get());
    up->AddHandle(mxtl::move(handle));

    ktrace(TAG_PORT_CREATE, koid, 0, 0, 0);
    return hv;
}

mx_status_t sys_port_queue(mx_handle_t handle, mxtl::user_ptr<const void> packet, mx_size_t size) {
    LTRACEF("handle %d\n", handle);

    if (size > MX_PORT_MAX_PKT_SIZE)
        return ERR_NOT_ENOUGH_BUFFER;

    if (size < sizeof(mx_packet_header_t))
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<IOPortDispatcher> ioport;
    mx_status_t status = up->GetDispatcher(handle, &ioport, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    auto iopk = IOP_Packet::MakeFromUser(packet.get(), size);
    if (!iopk)
        return ERR_NO_MEMORY;

    ktrace(TAG_PORT_QUEUE, (uint32_t)ioport->get_koid(), (uint32_t)size, 0, 0);

    return ioport->Queue(iopk);
}

mx_status_t sys_port_wait(mx_handle_t handle, mxtl::user_ptr<void> packet, mx_size_t size) {
    LTRACEF("handle %d\n", handle);

    if (!packet)
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<IOPortDispatcher> ioport;
    mx_status_t status = up->GetDispatcher(handle, &ioport, MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    ktrace(TAG_PORT_WAIT, (uint32_t)ioport->get_koid(), 0, 0, 0);

    IOP_Packet* iopk = nullptr;
    status = ioport->Wait(&iopk);
    ktrace(TAG_PORT_WAIT_DONE, (uint32_t)ioport->get_koid(), status, 0, 0);
    if (status < 0)
        return status;

    if (!iopk->CopyToUser(packet.get(), &size))
        return ERR_INVALID_ARGS;

    IOP_Packet::Delete(iopk);
    return NO_ERROR;
}

mx_status_t sys_port_bind(mx_handle_t handle, uint64_t key, mx_handle_t source, mx_signals_t signals) {
    LTRACEF("handle %d source %d\n", handle, source);

    if (!signals)
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<IOPortDispatcher> ioport;
    mx_status_t status = up->GetDispatcher(handle, &ioport, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    mxtl::RefPtr<MessagePipeDispatcher> msg_pipe;
    status = up->GetDispatcher(source, &msg_pipe, MX_RIGHT_READ);
    if (status != NO_ERROR) {
        if (status == ERR_WRONG_TYPE)
            status = ERR_NOT_SUPPORTED;
        return status;
    }

    return msg_pipe->SetIOPort(mxtl::move(ioport), key, signals);
 }

// TODO(vtl): _consumer_handle should presumably be an mxtl::user_ptr instead. Also, do we want to
// provide the producer handle as an out parameter (possibly in the same way as in msgpipe_create,
// instead of overloading the return value)?
mx_handle_t sys_datapipe_create(uint32_t options, mx_size_t element_size, mx_size_t capacity,
                                mx_handle_t* _consumer_handle) {
    LTRACEF("options %u\n", options);

    if (!_consumer_handle)
        return ERR_INVALID_ARGS;

    if (element_size == 0u)
        return ERR_INVALID_ARGS;

    if (capacity % element_size != 0u)
        return ERR_INVALID_ARGS;

    if (!capacity) {
        capacity = kDefaultDataPipeCapacity - (kDefaultDataPipeCapacity % element_size);
        if (!capacity)
            capacity = element_size;
    }

    mxtl::RefPtr<Dispatcher> producer_dispatcher;
    mx_rights_t producer_rights;

    mxtl::RefPtr<Dispatcher> consumer_dispatcher;
    mx_rights_t consumer_rights;

    mx_status_t result = DataPipe::Create(element_size, capacity, &producer_dispatcher,
                                          &consumer_dispatcher, &producer_rights, &consumer_rights);
    if (result != NO_ERROR)
        return result;

    HandleUniquePtr producer_handle(MakeHandle(mxtl::move(producer_dispatcher), producer_rights));
    if (!producer_handle)
        return ERR_NO_MEMORY;

    HandleUniquePtr consumer_handle(MakeHandle(mxtl::move(consumer_dispatcher), consumer_rights));
    if (!consumer_handle)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();
    mx_handle_t hv_producer = up->MapHandleToValue(producer_handle.get());
    mx_handle_t hv_consumer = up->MapHandleToValue(consumer_handle.get());

    if (copy_to_user_32_unsafe(_consumer_handle, hv_consumer) != NO_ERROR)
        return ERR_INVALID_ARGS;

    up->AddHandle(mxtl::move(producer_handle));
    up->AddHandle(mxtl::move(consumer_handle));

    return hv_producer;
}

mx_ssize_t sys_datapipe_write(mx_handle_t producer_handle, uint32_t flags, mx_size_t requested,
                              const void* _buffer) {
    LTRACEF("handle %d\n", producer_handle);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<DataPipeProducerDispatcher> producer;
    mx_status_t status = up->GetDispatcher(producer_handle, &producer, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    // TODO(vtl): Handle write flags.

    mx_size_t written = requested;
    status = producer->Write(_buffer, &written);
    if (status < 0)
        return status;

    return written;
}

mx_ssize_t sys_datapipe_read(mx_handle_t consumer_handle, uint32_t flags, mx_size_t requested,
                             void* _buffer) {
    LTRACEF("handle %d\n", consumer_handle);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<DataPipeConsumerDispatcher> consumer;
    mx_status_t status = up->GetDispatcher(consumer_handle, &consumer, MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    if (flags & ~MX_DATAPIPE_READ_FLAG_MASK)
        return ERR_NOT_SUPPORTED;

    bool all_or_none = flags & MX_DATAPIPE_READ_FLAG_ALL_OR_NONE;
    bool discard = flags & MX_DATAPIPE_READ_FLAG_DISCARD;
    bool query = flags & MX_DATAPIPE_READ_FLAG_QUERY;
    bool peek = flags & MX_DATAPIPE_READ_FLAG_PEEK;
    if (query) {
        if (discard || peek)
            return ERR_INVALID_ARGS;
        // Note: We ignore "all or none".
        return consumer->Query();
    }
    if (discard && peek)
        return ERR_INVALID_ARGS;

    mx_size_t read = requested;
    status = consumer->Read(_buffer, &read, all_or_none, discard, peek);
    if (status < 0)
        return status;

    return read;
}

mx_ssize_t sys_datapipe_begin_write(mx_handle_t producer_handle, uint32_t flags,
                                    uintptr_t* buffer) {
    LTRACEF("handle %d\n", producer_handle);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<DataPipeProducerDispatcher> producer;
    mx_status_t status = up->GetDispatcher(producer_handle, &producer, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    // TODO(vtl): Handle (disallow) write flags.

    uintptr_t user_addr = 0u;

    mx_ssize_t result = producer->BeginWrite(up->aspace(), reinterpret_cast<void**>(&user_addr));
    if (result < 0)
        return result;
    DEBUG_ASSERT(result > 0);

    if (copy_to_user_uptr_unsafe(buffer, user_addr) != NO_ERROR) {
        producer->EndWrite(0u);
        return ERR_INVALID_ARGS;
    }

    return result;
}

mx_status_t sys_datapipe_end_write(mx_handle_t producer_handle, mx_size_t written) {
    LTRACEF("handle %d\n", producer_handle);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<DataPipeProducerDispatcher> producer;
    mx_status_t status = up->GetDispatcher(producer_handle, &producer, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    return producer->EndWrite(written);
}

mx_ssize_t sys_datapipe_begin_read(mx_handle_t consumer_handle, uint32_t flags, uintptr_t* buffer) {
    LTRACEF("handle %d\n", consumer_handle);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<DataPipeConsumerDispatcher> consumer;
    mx_status_t status = up->GetDispatcher(consumer_handle, &consumer, MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    // Currently, no flags are supported with two-phase read.
    if (flags)
        return (flags & ~MX_DATAPIPE_READ_FLAG_MASK) ? ERR_NOT_SUPPORTED : ERR_INVALID_ARGS;

    uintptr_t user_addr = 0u;

    mx_ssize_t result = consumer->BeginRead(up->aspace(), reinterpret_cast<void**>(&user_addr));
    if (result < 0)
        return result;
    DEBUG_ASSERT(result > 0);

    if (copy_to_user_uptr_unsafe(buffer, user_addr) != NO_ERROR) {
        consumer->EndRead(0u);
        return ERR_INVALID_ARGS;
    }

    return result;
}

mx_status_t sys_datapipe_end_read(mx_handle_t consumer_handle, mx_size_t read) {
    LTRACEF("handle %d\n", consumer_handle);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<DataPipeConsumerDispatcher> consumer;
    mx_status_t status = up->GetDispatcher(consumer_handle, &consumer, MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    return consumer->EndRead(read);
}

mx_ssize_t sys_cprng_draw(mxtl::user_ptr<void> buffer, mx_size_t len) {
    if (len > kMaxCPRNGDraw)
        return ERR_INVALID_ARGS;

    uint8_t kernel_buf[kMaxCPRNGDraw];

    auto prng = crypto::GlobalPRNG::GetInstance();
    prng->Draw(kernel_buf, static_cast<int>(len));

    if (copy_to_user(buffer, kernel_buf, len) != NO_ERROR)
        return ERR_INVALID_ARGS;

    // Get rid of the stack copy of the random data
    memset(kernel_buf, 0, sizeof(kernel_buf));

    return len;
}

mx_status_t sys_cprng_add_entropy(mxtl::user_ptr<void> buffer, mx_size_t len) {
    if (len > kMaxCPRNGSeed)
        return ERR_INVALID_ARGS;

    uint8_t kernel_buf[kMaxCPRNGSeed];
    if (copy_from_user(kernel_buf, buffer, len) != NO_ERROR)
        return ERR_INVALID_ARGS;

    auto prng = crypto::GlobalPRNG::GetInstance();
    prng->AddEntropy(kernel_buf, static_cast<int>(len));

    // Get rid of the stack copy of the random data
    memset(kernel_buf, 0, sizeof(kernel_buf));

    return NO_ERROR;
}

mx_handle_t sys_waitset_create(void) {
    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    mx_status_t result = WaitSetDispatcher::Create(&dispatcher, &rights);
    if (result != NO_ERROR)
        return result;

    HandleUniquePtr handle(MakeHandle(mxtl::move(dispatcher), rights));
    if (!handle)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();
    mx_handle_t hv = up->MapHandleToValue(handle.get());
    up->AddHandle(mxtl::move(handle));

    return hv;
}

mx_status_t sys_waitset_add(mx_handle_t ws_handle_value,
                            mx_handle_t handle_value,
                            mx_signals_t signals,
                            uint64_t cookie) {
    LTRACEF("wait set handle %d, handle %d\n", ws_handle_value, handle_value);

    mxtl::unique_ptr<WaitSetDispatcher::Entry> entry;
    mx_status_t result = WaitSetDispatcher::Entry::Create(signals, cookie, &entry);
    if (result != NO_ERROR)
        return result;

    // TODO(vtl): Obviously, we need to get two handles under the handle table lock. We also call
    // WaitSetDispatcher::AddEntry() under it, which is quite terrible. However, it'd be quite
    // tricky to do it correctly otherwise.
    auto up = ProcessDispatcher::GetCurrent();
    AutoLock lock(up->handle_table_lock());

    Handle* ws_handle = up->GetHandle_NoLock(ws_handle_value);
    if (!ws_handle)
        return up->BadHandle(ws_handle_value, ERR_BAD_HANDLE);
    // No need to take a ref to the dispatcher, since we're under the handle table lock. :-/
    auto ws_dispatcher = ws_handle->dispatcher()->get_specific<WaitSetDispatcher>();
    if (!ws_dispatcher)
        return up->BadHandle(ws_handle_value, ERR_WRONG_TYPE);
    if (!magenta_rights_check(ws_handle->rights(), MX_RIGHT_WRITE))
        return up->BadHandle(ws_handle_value, ERR_ACCESS_DENIED);

    Handle* handle = up->GetHandle_NoLock(handle_value);
    if (!handle)
        return up->BadHandle(handle_value, ERR_BAD_HANDLE);
    if (!magenta_rights_check(handle->rights(), MX_RIGHT_READ))
        return up->BadHandle(handle_value, ERR_ACCESS_DENIED);

    return ws_dispatcher->AddEntry(mxtl::move(entry), handle);
}

mx_status_t sys_waitset_remove(mx_handle_t ws_handle, uint64_t cookie) {
    LTRACEF("wait set handle %d\n", ws_handle);

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<WaitSetDispatcher> ws_dispatcher;
    mx_status_t status =
        up->GetDispatcher(ws_handle, &ws_dispatcher, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    return ws_dispatcher->RemoveEntry(cookie);
}

mx_status_t sys_waitset_wait(mx_handle_t ws_handle,
                             mx_time_t timeout,
                             mxtl::user_ptr<uint32_t> _num_results,
                             mxtl::user_ptr<mx_waitset_result_t> _results,
                             mxtl::user_ptr<uint32_t> _max_results) {
    LTRACEF("wait set handle %d\n", ws_handle);

    uint32_t num_results;
    if (copy_from_user_u32(&num_results, _num_results) != NO_ERROR)
        return ERR_INVALID_ARGS;

    mxtl::unique_ptr<mx_waitset_result_t[]> results;
    if (num_results > 0u) {
        if (num_results > kMaxWaitSetWaitResults)
            return ERR_TOO_BIG;

        // TODO(vtl): It kind of sucks that we always have to allocate the indicated maximum size
        // here (namely, |num_results|).
        AllocChecker ac;
        results.reset(new (&ac) mx_waitset_result_t[num_results]);
        if (!ac.check())
            return ERR_NO_MEMORY;
    }

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<WaitSetDispatcher> ws_dispatcher;
    mx_status_t status =
        up->GetDispatcher(ws_handle, &ws_dispatcher, MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    uint32_t max_results = 0u;
    mx_status_t result = ws_dispatcher->Wait(timeout, &num_results, results.get(), &max_results);
    if (result == NO_ERROR) {
        if (copy_to_user_u32(_num_results, num_results) != NO_ERROR)
            return ERR_INVALID_ARGS;
        if (num_results > 0u) {
            if (copy_to_user(_results, results.get(), num_results * sizeof(mx_waitset_result_t)) !=
                    NO_ERROR)
            return ERR_INVALID_ARGS;
        }
        if (_max_results) {
            if (copy_to_user_u32(_max_results, max_results) != NO_ERROR)
                return ERR_INVALID_ARGS;
        }
    }

    return result;
}

mx_status_t sys_socket_create(mx_handle_t out_handle[2], uint32_t flags) {
    LTRACEF("entry out_handle[] %p\n", out_handle);

    if (!out_handle)
        return ERR_INVALID_ARGS;

    if (flags != 0u)
        return ERR_INVALID_ARGS;

    mxtl::RefPtr<Dispatcher> socket0, socket1;
    mx_rights_t rights;
    status_t result = SocketDispatcher::Create(flags, &socket0, &socket1, &rights);
    if (result != NO_ERROR)
        return result;

    HandleUniquePtr h0(MakeHandle(mxtl::move(socket0), rights));
    if (!h0)
        return ERR_NO_MEMORY;

    HandleUniquePtr h1(MakeHandle(mxtl::move(socket1), rights));
    if (!h1)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();
    mx_handle_t hv[2] = {up->MapHandleToValue(h0.get()), up->MapHandleToValue(h1.get())};

    if (copy_to_user(mxtl::user_ptr<mx_handle_t>(out_handle), hv, sizeof(mx_handle_t) * 2) != NO_ERROR)
        return ERR_INVALID_ARGS;

    up->AddHandle(mxtl::move(h0));
    up->AddHandle(mxtl::move(h1));

    return NO_ERROR;
}

mx_ssize_t sys_socket_write(mx_handle_t handle, uint32_t flags,
                            mx_size_t size, void const* _buffer) {
    LTRACEF("handle %d\n", handle);

    if (!_buffer)
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<SocketDispatcher> socket;
    mx_status_t status = up->GetDispatcher(handle, &socket, MX_RIGHT_WRITE);
    if (status != NO_ERROR)
        return status;

    return flags == MX_SOCKET_CONTROL?
        socket->OOB_Write(_buffer, size, true) :
        socket->Write(_buffer, size, true);
}

mx_ssize_t sys_socket_read(mx_handle_t handle, uint32_t flags,
                           mx_size_t size, void* _buffer) {
    LTRACEF("handle %d\n", handle);

    if (!_buffer)
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<SocketDispatcher> socket;
    mx_status_t status = up->GetDispatcher(handle, &socket, MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    return flags == MX_SOCKET_CONTROL?
        socket->OOB_Read(_buffer, size, true) :
        socket->Read(_buffer, size, true);
}
