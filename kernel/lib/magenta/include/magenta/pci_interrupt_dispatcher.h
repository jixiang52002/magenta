// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <dev/pcie.h>
#include <kernel/event.h>
#include <magenta/dispatcher.h>
#include <magenta/pci_device_dispatcher.h>
#include <sys/types.h>

class PciDeviceDispatcher;

class PciInterruptDispatcher final : public Dispatcher {
public:
    static status_t Create(const mxtl::RefPtr<PciDeviceDispatcher::PciDeviceWrapper>& device,
                           uint32_t irq_id,
                           bool maskable,
                           mx_rights_t* out_rights,
                           mxtl::RefPtr<Dispatcher>* out_interrupt);

    ~PciInterruptDispatcher() final;
    mx_obj_type_t get_type() const final { return MX_OBJ_TYPE_PCI_INT; }

    // TODO(cpu): this should be removed when device waiting is refactored.
    void Close();

    status_t InterruptWait();

private:
    static pcie_irq_handler_retval_t IrqThunk(struct pcie_device_state* dev,
                                              uint irq_id,
                                              void* ctx);
    PciInterruptDispatcher(uint32_t irq);

    uint32_t irq_id_;
    bool     maskable_;
    event_t  event_;
    Mutex lock_;
    Mutex wait_lock_;
    mxtl::RefPtr<PciDeviceDispatcher::PciDeviceWrapper> device_;
};
