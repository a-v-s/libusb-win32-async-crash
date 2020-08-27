#pragma once
extern "C" {
#include "libusb.h"
}

#include <vector>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>

using namespace std;

class Device {
public:
    Device(libusb_device_handle *handle);
    ~Device();static void LIBUSB_CALL libusb_transfer_cb(struct libusb_transfer* transfer);

    int getSerial() {
        return iSerial;
    }
    libusb_device* getLibUsbDevice() {
        return m_device;
    }
private:
    libusb_device_handle *m_handle = nullptr;
    libusb_device *m_device = nullptr;

    struct libusb_transfer *m_transfer_in = nullptr;

    uint8_t m_recv_buffers[60];
    uint8_t sSerial[20];
    int iSerial;
    mutex m_process_recv_queue_mutex;
    condition_variable m_process_recv_queue_cv;
    deque<vector<uint8_t>> m_recv_queue;

    bool m_process_recv_queue_running;
    thread m_process_recv_queue_thread;

    void send(int ep, void *data, size_t size);

    static void process_recv_queue_code(Device *mc);
};

