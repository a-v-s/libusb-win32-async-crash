#include "Device.hpp"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

using namespace std;

void Device::send(int ep, void *data, size_t size) {
    if (!size)
        size = sizeof(m_recv_buffers);
    struct libusb_transfer *xfr;
    xfr = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(xfr, this->m_handle, 0x7F & ep, // Endpoint ID
    (unsigned char*) data, size, libusb_transfer_cb, this, 5000);

    // Less frequent crash
    // io.c  Line 1417
    //       add_to_flying_list(usbi_transfer * transfer)
    libusb_submit_transfer(xfr);
}

void Device::libusb_transfer_cb(struct libusb_transfer *transfer) {
    Device *md = (Device*) (transfer->user_data);
    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:

        if (transfer->endpoint & 0x80) {
            printf("Received %d bytes on EP %02X\n", transfer->actual_length, transfer->endpoint);

            vector < uint8_t > recvData;
            recvData.resize(1 + transfer->actual_length);
            memcpy(1 + recvData.data(), transfer->buffer, transfer->actual_length);
            recvData[0] = transfer->endpoint;
            unique_lock < mutex > lk(md->m_process_recv_queue_mutex);
            md->m_recv_queue.push_back(recvData);
            md->m_process_recv_queue_cv.notify_all();

            libusb_error status = (libusb_error) libusb_submit_transfer(transfer);
            if (status) {
                printf("Re-issue receive transfer error %s %s\n", libusb_error_name(status), libusb_strerror(status));
            }
        } else {
            printf("Transmitted %d bytes on EP %02X\n", transfer->actual_length, transfer->endpoint);

            transfer->buffer[0] = 0;
            // Free transfer
            libusb_free_transfer(transfer);
        }

        break;
    case LIBUSB_TRANSFER_STALL:
    case LIBUSB_TRANSFER_OVERFLOW:
    case LIBUSB_TRANSFER_TIMED_OUT:
    case LIBUSB_TRANSFER_ERROR:

        if (transfer->endpoint & 0x80) {
            libusb_error status = (libusb_error) libusb_submit_transfer(transfer);
            if (status) {
                printf("Re-issue receive transfer error %s %s\n", libusb_error_name(status), libusb_strerror(status));
            }
        } else {
            transfer->buffer[0] = 0;

            libusb_error status = (libusb_error) libusb_submit_transfer(transfer);
            if (status) {
                printf("Transmit transfer error %s %s\n", libusb_error_name(status), libusb_strerror(status));
            }

            // Free transfer
            libusb_free_transfer(transfer);
        }

        break;

        printf("Other USB STATUS %d\n", transfer->status);
        break;
    case LIBUSB_TRANSFER_NO_DEVICE:
        printf("LIBUSB_TRANSFER_NO_DEVICE\n");
        break;
    case LIBUSB_TRANSFER_CANCELLED:
        printf("LIBUSB_TRANSFER_CANCELLED\n");
        break;
    }
}

void Device::process_recv_queue_code(Device *md) {
    while (md->m_process_recv_queue_running) {
        unique_lock < mutex > lk(md->m_process_recv_queue_mutex);
        md->m_process_recv_queue_cv.wait(lk);
        if (!md->m_process_recv_queue_running)
            return;

        while (md->m_recv_queue.size()) {
            auto data = md->m_recv_queue.front();

            // For this demo, we return the data received
            //md->parse(*data.data(), data.data() + 1, data.size()-1);
            md->send(*data.data(), data.data() + 1, data.size() - 1);

            md->m_recv_queue.pop_front();
        }
    }
}

Device::Device(libusb_device_handle *handle) {
    m_handle = handle;
    m_device = libusb_get_device(handle);
    int retval;

    retval = libusb_claim_interface(handle, 0);
    if (retval)
        fprintf(stderr, "Error claiming interface %d: %s.\n", 0, libusb_strerror((libusb_error) retval));

    struct libusb_device_descriptor device_desc;
    retval = libusb_get_device_descriptor(m_device, &device_desc);

    if (retval) {
        printf("Error. Cannot Obtain Device Descriptor:%s %s\n Trying again...\n ", libusb_error_name(retval),
                libusb_strerror((libusb_error) retval));
        retval = libusb_get_device_descriptor(m_device, &device_desc);
        if (retval) {
            printf("Error. Cannot Obtain Device Descriptor:%s %s\n Bailing out...\n ", libusb_error_name(retval),
                    libusb_strerror((libusb_error) retval));
            return;
        }
    }

    retval = libusb_get_string_descriptor_ascii(handle, device_desc.iSerialNumber, sSerial, sizeof(sSerial));
    sscanf_s((const char*) sSerial, "%d", &iSerial);

    m_transfer_in = libusb_alloc_transfer(0);

    libusb_fill_bulk_transfer(m_transfer_in, handle, 0x81, m_recv_buffers, sizeof(m_recv_buffers), libusb_transfer_cb, this, 5000);

    retval = libusb_submit_transfer(m_transfer_in);
    if (retval)
        fprintf(stderr, "Error submitting transfer 0x81: %s.\n", libusb_strerror((libusb_error) retval));

    m_process_recv_queue_running = true;
    m_process_recv_queue_thread = thread(process_recv_queue_code, this);

    send(0x01, m_recv_buffers, sizeof(m_recv_buffers));
}

Device::~Device() {
    printf("Device::~Device()\n");
    fflush (stdout);

    printf("Stopping Receive Thread\n");
    fflush(stdout);
    if (m_process_recv_queue_running) {
        m_process_recv_queue_running = false;
        m_process_recv_queue_cv.notify_all();
        m_process_recv_queue_thread.join();
    }

    printf("Cancelling receive transfer\n");
    fflush(stdout);
    libusb_cancel_transfer(m_transfer_in);
    libusb_free_transfer(m_transfer_in);

    printf("Releasing Interface\n");
    fflush(stdout);
    libusb_release_interface(m_handle, 0);
}
