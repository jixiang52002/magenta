// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <magenta/hw/usb.h>
#include <stdio.h>
#include <threads.h>

#include "xhci.h"
#include "xhci-util.h"

//#define TRACE 1
#include "xhci-debug.h"

//#define TRACE_TRBS 1
#ifdef TRACE_TRBS
static void print_trb(xhci_t* xhci, xhci_transfer_ring_t* ring, xhci_trb_t* trb) {
    int index = trb - ring->start;
    uint32_t* ptr = (uint32_t *)trb;
    uint64_t paddr = xhci_virt_to_phys(xhci, (mx_vaddr_t)trb);

    printf("trb[%03d] %p: %08X %08X %08X %08X\n", index, (void *)paddr, ptr[0], ptr[1], ptr[2], ptr[3]);
}
#else
#define print_trb(xhci, ring, trb) do {} while (0)
#endif

static mx_status_t xhci_reset_endpoint(xhci_t* xhci, uint32_t slot_id, uint32_t endpoint) {
    xprintf("xhci_reset_endpoint %d %d\n", slot_id, endpoint);

    xhci_slot_t* slot = &xhci->slots[slot_id];
    xhci_transfer_ring_t* transfer_ring = &slot->transfer_rings[endpoint];

    mtx_lock(&transfer_ring->mutex);

    // first reset the endpoint
    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    uint32_t control = (slot_id << TRB_SLOT_ID_START) |( endpoint <<TRB_ENDPOINT_ID_START);
    xhci_post_command(xhci, TRB_CMD_RESET_ENDPOINT, 0, control, &command.context);
    int cc = xhci_sync_command_wait(&command);

    if (cc != TRB_CC_SUCCESS) {
        mtx_unlock(&transfer_ring->mutex);
        return ERR_INTERNAL;
    }

    // then move transfer ring's dequeue pointer passed the failed transaction
    xhci_sync_command_init(&command);
    uint64_t ptr = xhci_virt_to_phys(xhci, (mx_vaddr_t)transfer_ring->current);
    ptr |= transfer_ring->pcs;
    control = (slot_id << TRB_SLOT_ID_START) |( endpoint <<TRB_ENDPOINT_ID_START);
    xhci_post_command(xhci, TRB_CMD_SET_TR_DEQUEUE, ptr, control, &command.context);
    cc = xhci_sync_command_wait(&command);

    transfer_ring->dequeue_ptr = transfer_ring->current;

    mtx_unlock(&transfer_ring->mutex);

    return (cc == TRB_CC_SUCCESS ? NO_ERROR : ERR_INTERNAL);
}

int xhci_queue_transfer(xhci_t* xhci, int slot_id, usb_setup_t* setup, mx_paddr_t data,
                        uint16_t length, int endpoint, int direction,
                        xhci_transfer_context_t* context, list_node_t* txn_node) {
    xprintf("xhci_queue_transfer slot_id: %d setup: %p endpoint: %d length: %d\n",
            slot_id, setup, endpoint, length);

    xhci_slot_t* slot = &xhci->slots[slot_id];
    if (!slot->enabled)
        return ERR_REMOTE_CLOSED;

    xhci_transfer_ring_t* ring = &slot->transfer_rings[endpoint];
    if (!ring)
        return ERR_INVALID_ARGS;

    // reset endpoint if it is halted
    xhci_endpoint_context_t* epc = slot->epcs[endpoint];
    if (XHCI_GET_BITS32(&epc->epc0, EP_CTX_EP_STATE_START, EP_CTX_EP_STATE_BITS) == 2 /* halted */ ) {
        xhci_reset_endpoint(xhci, slot_id, endpoint);
    }

    uint32_t interruptor_target = 0;
    size_t max_transfer_size = 1 << (XFER_TRB_XFER_LENGTH_BITS - 1);
    size_t data_packets = (length ? (length + max_transfer_size - 1) / max_transfer_size : 0);
    size_t required_trbs = data_packets + 1;   // add 1 for event data TRB
    if (setup) {
        required_trbs += 2;
    }
    if (required_trbs > ring->size) {
        // no way this will ever succeed
        return ERR_INVALID_ARGS;
    }

    // FIXME handle zero length packets

    mtx_lock(&ring->mutex);

    // don't allow queueing new requests if we have deferred requests
    if (!list_is_empty(&ring->deferred_txns) || required_trbs > xhci_transfer_ring_free_trbs(ring)) {
        // add txn to deferred_txn list for later processing
        if (txn_node) {
            list_add_tail(&ring->deferred_txns, txn_node);
        }
        mtx_unlock(&ring->mutex);
        return ERR_NOT_ENOUGH_BUFFER;
    }

    context->transfer_ring = ring;
    list_add_tail(&ring->pending_requests, &context->node);

    if (setup) {
        // Setup Stage
        xhci_trb_t* trb = ring->current;
        xhci_clear_trb(trb);

        XHCI_SET_BITS32(&trb->ptr_low, SETUP_TRB_REQ_TYPE_START, SETUP_TRB_REQ_TYPE_BITS, setup->bmRequestType);
        XHCI_SET_BITS32(&trb->ptr_low, SETUP_TRB_REQUEST_START, SETUP_TRB_REQUEST_BITS, setup->bRequest);
        XHCI_SET_BITS32(&trb->ptr_low, SETUP_TRB_VALUE_START, SETUP_TRB_VALUE_BITS, setup->wValue);
        XHCI_SET_BITS32(&trb->ptr_high, SETUP_TRB_INDEX_START, SETUP_TRB_INDEX_BITS, setup->wIndex);
        XHCI_SET_BITS32(&trb->ptr_high, SETUP_TRB_LENGTH_START, SETUP_TRB_LENGTH_BITS, length);
        XHCI_SET_BITS32(&trb->status, XFER_TRB_XFER_LENGTH_START, XFER_TRB_XFER_LENGTH_BITS, 8);
        XHCI_SET_BITS32(&trb->status, XFER_TRB_INTR_TARGET_START, XFER_TRB_INTR_TARGET_BITS, interruptor_target);

        uint32_t control_bits = (length == 0 ? XFER_TRB_TRT_NONE : (direction == USB_DIR_IN ? XFER_TRB_TRT_IN : XFER_TRB_TRT_OUT));
        control_bits |= XFER_TRB_IDT; // immediate data flag
        trb_set_control(trb, TRB_TRANSFER_SETUP, control_bits);
        print_trb(xhci, ring, trb);
        xhci_increment_ring(xhci, ring);
    }

    // Data Stage
    if (length > 0) {
        size_t remaining = length;

        for (size_t i = 0; i < data_packets; i++) {
            size_t transfer_size = (remaining > max_transfer_size ? max_transfer_size : remaining);
            remaining -= transfer_size;

            xhci_trb_t* trb = ring->current;
            xhci_clear_trb(trb);
            XHCI_WRITE64(&trb->ptr, data + (i * max_transfer_size));
            XHCI_SET_BITS32(&trb->status, XFER_TRB_XFER_LENGTH_START, XFER_TRB_XFER_LENGTH_BITS, transfer_size);
            uint32_t td_size = data_packets - i - 1;
            XHCI_SET_BITS32(&trb->status, XFER_TRB_TD_SIZE_START, XFER_TRB_TD_SIZE_BITS, td_size);
            XHCI_SET_BITS32(&trb->status, XFER_TRB_INTR_TARGET_START, XFER_TRB_INTR_TARGET_BITS, interruptor_target);

            uint32_t control_bits = TRB_CHAIN;
            if (td_size == 0) {
                control_bits |= XFER_TRB_ENT;
            }
            if (setup && i == 0) {
                // use TRB_TRANSFER_DATA for first data packet on setup requests
                control_bits |= (direction == USB_DIR_IN ? XFER_TRB_DIR_IN : XFER_TRB_DIR_OUT);
                trb_set_control(trb, TRB_TRANSFER_DATA, control_bits);
            } else {
                trb_set_control(trb, TRB_TRANSFER_NORMAL, control_bits);
            }
            print_trb(xhci, ring, trb);
            xhci_increment_ring(xhci, ring);
        }

        // Follow up with event data TRB
        xhci_trb_t* trb = ring->current;
        xhci_clear_trb(trb);
        trb_set_ptr(trb, context);
        XHCI_SET_BITS32(&trb->status, XFER_TRB_INTR_TARGET_START, XFER_TRB_INTR_TARGET_BITS, interruptor_target);
        trb_set_control(trb, TRB_TRANSFER_EVENT_DATA, XFER_TRB_IOC);
        print_trb(xhci, ring, trb);
        xhci_increment_ring(xhci, ring);
    }

    if (setup) {
        // Status Stage
        xhci_trb_t* trb = ring->current;
        xhci_clear_trb(trb);
        XHCI_SET_BITS32(&trb->status, XFER_TRB_INTR_TARGET_START, XFER_TRB_INTR_TARGET_BITS, interruptor_target);
        uint32_t control_bits = (direction == USB_DIR_IN && length > 0 ? XFER_TRB_DIR_OUT : XFER_TRB_DIR_IN);
        if (length == 0) {
            control_bits |= TRB_CHAIN;
        }
        trb_set_control(trb, TRB_TRANSFER_STATUS, control_bits);
        print_trb(xhci, ring, trb);
        xhci_increment_ring(xhci, ring);

        if (length == 0) {
            // Follow up with event data TRB
            xhci_trb_t* trb = ring->current;
            xhci_clear_trb(trb);
            trb_set_ptr(trb, context);
            XHCI_SET_BITS32(&trb->status, XFER_TRB_INTR_TARGET_START, XFER_TRB_INTR_TARGET_BITS, interruptor_target);
            trb_set_control(trb, TRB_TRANSFER_EVENT_DATA, XFER_TRB_IOC);
            print_trb(xhci, ring, trb);
            xhci_increment_ring(xhci, ring);
        }
    }
    // update dequeue_ptr to TRB following this transaction
    context->dequeue_ptr = ring->current;

    XHCI_WRITE32(&xhci->doorbells[slot_id], endpoint + 1);

    mtx_unlock(&ring->mutex);

    return NO_ERROR;
}

int xhci_control_request(xhci_t* xhci, int slot_id, uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, mx_paddr_t data, uint16_t length) {
    xprintf("xhci_control_request slot_id: %d type: 0x%02X req: %d value: %d index: %d length: %d\n",
            slot_id, request_type, request, value, index, length);

    usb_setup_t setup;
    setup.bmRequestType = request_type;
    setup.bRequest = request;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = length;

    xhci_sync_transfer_t xfer;
    xhci_sync_transfer_init(&xfer);

    mx_status_t result = xhci_queue_transfer(xhci, slot_id, &setup, data, length, 0,
                                             request_type & USB_DIR_MASK, &xfer.context, NULL);
    if (result != NO_ERROR)
        return result;

    result = xhci_sync_transfer_wait(&xfer);
    xprintf("xhci_control_request returning %d\n", result);
    return result;
}

mx_status_t xhci_get_descriptor(xhci_t* xhci, int slot_id, uint8_t type, uint16_t value,
                                uint16_t index, void* data, uint16_t length) {
    mx_paddr_t phys_addr = xhci_virt_to_phys(xhci, (mx_vaddr_t)data);
    return xhci_control_request(xhci, slot_id, USB_DIR_IN | type | USB_RECIP_DEVICE,
                                USB_REQ_GET_DESCRIPTOR, value, index, phys_addr, length);
}

void xhci_handle_transfer_event(xhci_t* xhci, xhci_trb_t* trb) {
    xprintf("xhci_handle_transfer_event: %08X %08X %08X %08X\n",
            ((uint32_t*)trb)[0], ((uint32_t*)trb)[1], ((uint32_t*)trb)[2], ((uint32_t*)trb)[3]);

    uint32_t control = XHCI_READ32(&trb->control);
    uint32_t status = XHCI_READ32(&trb->status);
    uint32_t cc = (status & XHCI_MASK(EVT_TRB_CC_START, EVT_TRB_CC_BITS)) >> EVT_TRB_CC_START;
    uint32_t length = (status & XHCI_MASK(EVT_TRB_XFER_LENGTH_START, EVT_TRB_XFER_LENGTH_BITS)) >> EVT_TRB_XFER_LENGTH_START;
    xhci_transfer_context_t* context = NULL;

    if (control & EVT_TRB_ED) {
        context = (xhci_transfer_context_t*)trb_get_ptr(trb);
    } else {
        trb = xhci_read_trb_ptr(xhci, trb);
        for (int i = 0; i < 5 && trb; i++) {
            if (trb_get_type(trb) == TRB_TRANSFER_EVENT_DATA) {
                context = (xhci_transfer_context_t*)trb_get_ptr(trb);
                break;
            }
            trb = xhci_get_next_trb(xhci, trb);
        }
    }

    mx_status_t result;
    switch (cc) {
        case TRB_CC_SUCCESS:
        case TRB_CC_SHORT_PACKET:
            result = length;
            break;
        case TRB_CC_STALL_ERROR:
            // FIXME - better error for stall case?
            result = ERR_BAD_STATE;
            break;
        case TRB_CC_STOPPED:
        case TRB_CC_STOPPED_LENGTH_INVALID:
        case TRB_CC_STOPPED_SHORT_PACKET:
            // for these errors the transfer ring may no longer exist,
            // so it is not safe to attempt to retrieve our transfer context
            xprintf("ignoring transfer event with cc: %d\n", cc);
            return;
        default:
            // FIXME - how do we report stalls, etc?
            result = ERR_REMOTE_CLOSED;
            break;
    }

    if (!context) {
        printf("unable to find transfer context in xhci_handle_transfer_event\n");
        return;
    }

    xhci_transfer_ring_t* ring = context->transfer_ring;

    mtx_lock(&ring->mutex);

    // update dequeue_ptr to TRB following this transaction
    ring->dequeue_ptr = context->dequeue_ptr;

    // remove context from pending_requests
    list_delete(&context->node);

    bool process_deferred_txns = !list_is_empty(&ring->deferred_txns);
    mtx_unlock(&ring->mutex);

    context->callback(result, context->data);

    if (process_deferred_txns) {
        xhci_process_deferred_txns(xhci, ring, false);
    }
}
