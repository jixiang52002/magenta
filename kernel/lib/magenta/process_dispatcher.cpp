// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <magenta/process_dispatcher.h>

#include <assert.h>
#include <inttypes.h>
#include <list.h>
#include <new.h>
#include <rand.h>
#include <string.h>
#include <trace.h>

#include <kernel/auto_lock.h>
#include <kernel/thread.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_aspace.h>
#include <kernel/vm/vm_object.h>

#include <lib/crypto/global_prng.h>

#include <magenta/futex_context.h>
#include <magenta/magenta.h>
#include <magenta/thread_dispatcher.h>
#include <magenta/user_copy.h>
#include <magenta/vm_object_dispatcher.h>

#define LOCAL_TRACE 0


static constexpr mx_rights_t kDefaultProcessRights = MX_RIGHT_READ  |
                                                     MX_RIGHT_WRITE |
                                                     MX_RIGHT_DUPLICATE |
                                                     MX_RIGHT_TRANSFER;

mutex_t ProcessDispatcher::global_process_list_mutex_ =
    MUTEX_INITIAL_VALUE(global_process_list_mutex_);
mxtl::DoublyLinkedList<ProcessDispatcher*> ProcessDispatcher::global_process_list_;


mx_handle_t map_handle_to_value(const Handle* handle, mx_handle_t mixer) {
    // Ensure that the last bit of the result is not zero and that
    // we don't lose upper bits.
    DEBUG_ASSERT((mixer & 0x1) == 0);
    DEBUG_ASSERT((MapHandleToU32(handle) & 0xe0000000) == 0);

    auto handle_id = (MapHandleToU32(handle) << 2) | 0x1;
    return mixer ^ handle_id;
}

Handle* map_value_to_handle(mx_handle_t value, mx_handle_t mixer) {
    auto handle_id = (value ^ mixer) >> 2;
    return MapU32ToHandle(handle_id);
}

mx_status_t ProcessDispatcher::Create(mxtl::StringPiece name,
                                      mxtl::RefPtr<Dispatcher>* dispatcher,
                                      mx_rights_t* rights, uint32_t flags) {
    AllocChecker ac;
    auto process = new (&ac) ProcessDispatcher(name, flags);
    if (!ac.check())
        return ERR_NO_MEMORY;

    status_t result = process->Initialize();
    if (result != NO_ERROR)
        return result;

    *rights = kDefaultProcessRights;
    *dispatcher = mxtl::AdoptRef<Dispatcher>(process);
    return NO_ERROR;
}

ProcessDispatcher::ProcessDispatcher(mxtl::StringPiece name, uint32_t flags)
    : state_tracker_(true, mx_signals_state_t{0u, MX_SIGNAL_SIGNALED}) {
    LTRACE_ENTRY_OBJ;

    // Add ourself to the global process list, generating an ID at the same time.
    AddProcess(this);

    // Generate handle XOR mask with top bit and bottom two bits cleared
    uint32_t secret;
    auto prng = crypto::GlobalPRNG::GetInstance();
    prng->Draw(&secret, sizeof(secret));

    // Handle values cannot be negative values, so we mask the high bit.
    handle_rand_ = (secret << 2) & INT_MAX;

    if (name.length() > 0 && (name.length() < sizeof(name_)))
        strlcpy(name_, name.data(), sizeof(name_));
}

ProcessDispatcher::~ProcessDispatcher() {
    LTRACE_ENTRY_OBJ;

    DEBUG_ASSERT(state_ == State::INITIAL || state_ == State::DEAD);

    // assert that we have no handles, should have been cleaned up in the -> DEAD transition
    DEBUG_ASSERT(handles_.is_empty());

    // remove ourself from the global process list
    RemoveProcess(this);

    LTRACE_EXIT_OBJ;
}

status_t ProcessDispatcher::Initialize() {
    LTRACE_ENTRY_OBJ;

    AutoLock lock(state_lock_);

    DEBUG_ASSERT(state_ == State::INITIAL);

    // create an address space for this process.
    aspace_ = VmAspace::Create(0, nullptr);
    if (!aspace_) {
        TRACEF("error creating address space\n");
        return ERR_NO_MEMORY;
    }

    return NO_ERROR;
}

status_t ProcessDispatcher::Start(mxtl::RefPtr<ThreadDispatcher> thread,
                                  uintptr_t pc, uintptr_t sp,
                                  uintptr_t arg1, uintptr_t arg2) {
    LTRACEF("process %p thread %p, entry %#" PRIxPTR ", sp %#" PRIxPTR
            ", arg1 %#" PRIxPTR ", arg2 %#" PRIxPTR "\n",
            this, thread, pc, sp, arg1, arg2);

    // grab and hold the state lock across this entire routine, since we're
    // effectively transitioning from INITIAL to RUNNING
    AutoLock lock(state_lock_);

    // make sure we're in the right state
    if (state_ != State::INITIAL)
        return ERR_BAD_STATE;

    // start the initial thread
    LTRACEF("starting main thread\n");
    auto status = thread->Start(pc, sp, arg1, arg2);
    if (status < 0)
        return status;

    SetState(State::RUNNING);

    return NO_ERROR;
}

void ProcessDispatcher::Exit(int retcode) {
    LTRACE_ENTRY_OBJ;

    {
        AutoLock lock(state_lock_);

        DEBUG_ASSERT(state_ == State::RUNNING);

        retcode_ = retcode;

        // enter the dying state, which should kill all threads
        SetState(State::DYING);
    }

    UserThread::GetCurrent()->Exit();
}

void ProcessDispatcher::Kill() {
    LTRACE_ENTRY_OBJ;

    AutoLock lock(state_lock_);

    // we're already dead
    if (state_ == State::DEAD)
        return;

    if (state_ != State::DYING) {
        // If there isn't an Exit already in progress, set a nonzero exit
        // status so e.g. crashing tests don't appear to have succeeded.
        DEBUG_ASSERT(retcode_ == 0);
        retcode_ = -1;
    }

    // if we have no threads, enter the dead state directly
    if (thread_list_.is_empty()) {
        SetState(State::DEAD);
    } else {
        // enter the dying state, which should trigger a thread kill.
        // the last thread exiting will transition us to DEAD
        SetState(State::DYING);
    }
}

void ProcessDispatcher::KillAllThreads() {
    LTRACE_ENTRY_OBJ;

    AutoLock lock(&thread_list_lock_);

    for (auto& thread : thread_list_) {
        LTRACEF("killing thread %p\n", &thread);
        thread.Kill();
    };

    // Unblock any futexes.
    // This is issued after all threads are marked as DYING so there
    // is no chance of a thread calling FutexWait.
    futex_context_.WakeAll();
}

status_t ProcessDispatcher::AddThread(UserThread* t) {
    LTRACE_ENTRY_OBJ;

    // cannot add thread to dying/dead state
    if (state_ == State::DYING || state_ == State::DEAD) {
        return ERR_BAD_STATE;
    }

    // add the thread to our list
    AutoLock lock(&thread_list_lock_);
    thread_list_.push_back(t);

    DEBUG_ASSERT(t->process() == this);

    return NO_ERROR;
}

void ProcessDispatcher::RemoveThread(UserThread* t) {
    LTRACE_ENTRY_OBJ;

    // we're going to check for state and possibly transition below
    AutoLock state_lock(&state_lock_);

    // remove the thread from our list
    AutoLock lock(&thread_list_lock_);
    DEBUG_ASSERT(t != nullptr);
    thread_list_.erase(*t);

    // drop the ref from the main_thread_ pointer if its being removed
    if (t == main_thread_.get()) {
        main_thread_.reset();
    }

    // if this was the last thread, transition directly to DEAD state
    if (thread_list_.is_empty()) {
        LTRACEF("last thread left the process %p, entering DEAD state\n", this);
        SetState(State::DEAD);
    }
}


void ProcessDispatcher::AllHandlesClosed() {
    LTRACE_ENTRY_OBJ;

    // check that we're not already entering a dead state
    // note this is checked outside of a mutex to avoid a reentrant case where the
    // process is already being destroyed, the handle table is being cleaned up, and
    // the last ref to itself is being dropped. In that case it recurses into this function
    // and would wedge up if Kill() is called
    if (state_ == State::DYING || state_ == State::DEAD)
        return;

    // last handle going away acts as a kill to the process object
    Kill();
}

void ProcessDispatcher::SetState(State s) {
    LTRACEF("process %p: state %u (%s)\n", this, static_cast<unsigned int>(s), StateToString(s));

    DEBUG_ASSERT(state_lock_.IsHeld());

    // look for some invalid state transitions
    if (state_ == State::DEAD && s != State::DEAD) {
        panic("ProcessDispatcher::SetState invalid state transition from DEAD to !DEAD\n");
        return;
    }

    // transitions to your own state are okay
    if (s == state_)
        return;

    state_ = s;

    if (s == State::DYING) {
        // send kill to all of our threads
        KillAllThreads();
    } else if (s == State::DEAD) {
        // clean up the handle table
        LTRACEF_LEVEL(2, "cleaning up handle table on proc %p\n", this);
        {
            AutoLock lock(&handle_table_lock_);
            Handle* handle;
            while ((handle = handles_.pop_front()) != nullptr) {
                DeleteHandle(handle);
            };
        }
        LTRACEF_LEVEL(2, "done cleaning up handle table on proc %p\n", this);

        // tear down the address space
        aspace_->Destroy();

        // signal waiter
        LTRACEF_LEVEL(2, "signalling waiters\n");
        state_tracker_.UpdateSatisfied(0u, MX_SIGNAL_SIGNALED);

        {
            AutoLock lock(&exception_lock_);
            if (exception_port_)
                exception_port_->OnProcessExit(this);
        }
    }
}

// process handle manipulation routines
mx_handle_t ProcessDispatcher::MapHandleToValue(const Handle* handle) const {
    return map_handle_to_value(handle, handle_rand_);
}

Handle* ProcessDispatcher::GetHandle_NoLock(mx_handle_t handle_value) {
    auto handle = map_value_to_handle(handle_value, handle_rand_);
    if (!handle)
        return nullptr;
    return (handle->process_id() == get_koid()) ? handle : nullptr;
}

void ProcessDispatcher::AddHandle(HandleUniquePtr handle) {
    AutoLock lock(&handle_table_lock_);
    AddHandle_NoLock(mxtl::move(handle));
}

void ProcessDispatcher::AddHandle_NoLock(HandleUniquePtr handle) {
    handle->set_process_id(get_koid());
    handles_.push_front(handle.release());
}

HandleUniquePtr ProcessDispatcher::RemoveHandle(mx_handle_t handle_value) {
    AutoLock lock(&handle_table_lock_);
    return RemoveHandle_NoLock(handle_value);
}

HandleUniquePtr ProcessDispatcher::RemoveHandle_NoLock(mx_handle_t handle_value) {
    auto handle = GetHandle_NoLock(handle_value);
    if (!handle)
        return nullptr;
    handles_.erase(*handle);
    handle->set_process_id(0u);

    return HandleUniquePtr(handle);
}

void ProcessDispatcher::UndoRemoveHandle_NoLock(mx_handle_t handle_value) {
    auto handle = map_value_to_handle(handle_value, handle_rand_);
    AddHandle_NoLock(HandleUniquePtr(handle));
}

bool ProcessDispatcher::GetDispatcher(mx_handle_t handle_value,
                                      mxtl::RefPtr<Dispatcher>* dispatcher,
                                      uint32_t* rights) {
    AutoLock lock(&handle_table_lock_);
    Handle* handle = GetHandle_NoLock(handle_value);
    if (!handle)
        return false;

    *rights = handle->rights();
    *dispatcher = handle->dispatcher();
    return true;
}

status_t ProcessDispatcher::GetInfo(mx_record_process_t* info) {
    info->return_code = retcode_;

    return NO_ERROR;
}

status_t ProcessDispatcher::CreateUserThread(mxtl::StringPiece name, uint32_t flags, mxtl::RefPtr<UserThread>* user_thread) {
    AllocChecker ac;
    auto ut = mxtl::AdoptRef(new (&ac) UserThread(GenerateKernelObjectId(),
                                                   mxtl::WrapRefPtr(this),
                                                   flags));
    if (!ac.check())
        return ERR_NO_MEMORY;

    status_t result = ut->Initialize(name);
    if (result != NO_ERROR)
        return result;

    *user_thread = mxtl::move(ut);
    return NO_ERROR;
}

status_t ProcessDispatcher::SetExceptionPort(mxtl::RefPtr<ExceptionPort> eport) {
    // Lock both |state_lock_| and |exception_lock_| to ensure the process
    // doesn't transition to dead while we're setting the exception handler.
    AutoLock state_lock(&state_lock_);
    AutoLock excp_lock(&exception_lock_);
    if (state_ == State::DEAD)
        return ERR_NOT_FOUND; // TODO(dje): ?
    if (exception_port_)
        return ERR_BAD_STATE; // TODO(dje): ?
    exception_port_ = eport;
    return NO_ERROR;
}

void ProcessDispatcher::ResetExceptionPort() {
    AutoLock lock(&exception_lock_);
    exception_port_.reset();
}

mxtl::RefPtr<ExceptionPort> ProcessDispatcher::exception_port() {
    AutoLock lock(&exception_lock_);
    return exception_port_;
}

void ProcessDispatcher::AddProcess(ProcessDispatcher* process) {
    // Don't call any method of |process|, it is not yet fully constructed.
    AutoLock lock(&global_process_list_mutex_);

    global_process_list_.push_back(process);

    LTRACEF("Adding process %p : koid = %llu\n", process, process->get_koid());
}

void ProcessDispatcher::RemoveProcess(ProcessDispatcher* process) {
    AutoLock lock(&global_process_list_mutex_);

    DEBUG_ASSERT(process != nullptr);
    global_process_list_.erase(*process);
    LTRACEF("Removing process %p : koid = %llu\n", process, process->get_koid());
}

// static
mxtl::RefPtr<ProcessDispatcher> ProcessDispatcher::LookupProcessById(mx_koid_t koid) {
    LTRACE_ENTRY;
    AutoLock lock(&global_process_list_mutex_);
    auto iter = global_process_list_.find_if([koid](const ProcessDispatcher& p) {
                                                return p.get_koid() == koid;
                                             });
    return mxtl::WrapRefPtr(iter.CopyPointer());
}

mxtl::RefPtr<UserThread> ProcessDispatcher::LookupThreadById(mx_koid_t koid) {
    LTRACE_ENTRY_OBJ;
    AutoLock lock(&thread_list_lock_);

    auto iter = thread_list_.find_if([koid](const UserThread& t) { return t.get_koid() == koid; });
    return mxtl::WrapRefPtr(iter.CopyPointer());
}

mx_status_t ProcessDispatcher::set_bad_handle_policy(uint32_t new_policy) {
    if (new_policy > MX_POLICY_BAD_HANDLE_EXIT)
        return ERR_NOT_SUPPORTED;
    bad_handle_policy_ = new_policy;
    return NO_ERROR;
}

const char* StateToString(ProcessDispatcher::State state) {
    switch (state) {
    case ProcessDispatcher::State::INITIAL:
        return "initial";
    case ProcessDispatcher::State::RUNNING:
        return "running";
    case ProcessDispatcher::State::DYING:
        return "dying";
    case ProcessDispatcher::State::DEAD:
        return "dead";
    }
    return "unknown";
}

mx_status_t ProcessDispatcher::Map(
    mxtl::RefPtr<VmObjectDispatcher> vmo, uint32_t vmo_rights,
    uint64_t offset, mx_size_t len, uintptr_t* address, uint32_t flags) {
    mx_status_t status;

    status = vmo->Map(aspace(), vmo_rights, offset, len, address, flags);

    return status;
}

mx_status_t ProcessDispatcher::Unmap(uintptr_t address, mx_size_t len) {
    mx_status_t status;

    // TODO: support range unmapping
    // at the moment only support unmapping what is at a given address, signalled with len = 0
    if (len != 0)
        return ERR_INVALID_ARGS;

    status = aspace_->FreeRegion(address);

    return status;
}

mx_status_t ProcessDispatcher::BadHandle(mx_handle_t handle_value,
                                         mx_status_t error) {
    // TODO(mcgrathr): Maybe treat other errors the same?
    // This also gets ERR_WRONG_TYPE and ERR_ACCESS_DENIED (for rights checks).
    if (error != ERR_BAD_HANDLE)
        return error;

    // TODO(cpu): Generate an exception when exception handling lands.
    if (get_bad_handle_policy() == MX_POLICY_BAD_HANDLE_EXIT) {
        printf("\n[fatal: %s used a bad handle]\n", name().data());
        Exit(error);
    }
    return error;
}
