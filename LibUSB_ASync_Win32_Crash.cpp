extern "C" {
#include "libusb.h"
}

#define VID 0xDEAD
#define PID 0xBEEF

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>

using namespace std;

#ifdef WIN32
#include <windows.h>
#include <dbt.h>
#include <usbiodef.h>
#include <devguid.h>
#include <setupapi.h>

// Windows vs Posix function names for case-insensitive compares
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#endif

#include "Device.hpp"

libusb_context *ctx = nullptr;

bool libusb_hotplug_callback_thread_running;
bool libusb_handle_events_thread_running;

mutex libusb_hotplug_callback_mutex;
mutex libusb_handle_events_mutex;

thread libusb_hotplug_callback_thread;
thread libusb_handle_events_thread;

condition_variable libusb_hotplug_callback_cv;

thread windows_hwdet_thread;

map<libusb_device*, Device*> mapDevices;
map<int, Device*> mapSerial2Device;

typedef struct {
    struct libusb_context *ctx;
    struct libusb_device *dev;
    libusb_hotplug_event event;
} libusb_hotplug_event_t;

queue<libusb_hotplug_event_t> libusb_hotplug_event_queue;

void libusb_handle_events_thread_code(void) {
    while (libusb_handle_events_thread_running) {
        // This is where the crash might occur when there is a transfer active when unplugging the device.
        // The crash occurs in windows_winusb.c line 1890
        // winusb_get_transfer_fd(usbi_transfer * itransfer)

        libusb_handle_events_completed(ctx, nullptr);
    }
}

void libusb_hotplug_callback_thread_code(void) {
    while (libusb_hotplug_callback_thread_running) {
        unique_lock < mutex > lk(libusb_hotplug_callback_mutex);
        libusb_hotplug_callback_cv.wait(lk);
        if (!libusb_hotplug_callback_thread_running)
            return;

        while (!libusb_hotplug_event_queue.empty()) {
            auto libusb_hotplug_callback_event = libusb_hotplug_event_queue.front();
            libusb_hotplug_event_queue.pop();

            switch (libusb_hotplug_callback_event.event) {
            case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED: {

                libusb_device_handle *handle = NULL;
                int retval = libusb_open(libusb_hotplug_callback_event.dev, &handle);

                bool isSupportedDevice = true;

                if (retval) {
                    printf("Unable to open device: %s: %s\n", libusb_error_name(retval), libusb_strerror((libusb_error) retval));
                    isSupportedDevice = false;
                }

                if (!isSupportedDevice) {
                    libusb_close(handle);
                    break;
                }


                // Here we would do some checks about the device, but for the demo, just accept the device

                printf("Adding Device!\n");
                Device *dev = new Device(handle);
                mapSerial2Device[dev->getSerial()] = dev;
                mapDevices[dev->getLibUsbDevice()] = dev;

                break;
            }
            case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT: {
                Device *dev = mapDevices[libusb_hotplug_callback_event.dev];
                if (dev) {
                    mapSerial2Device.erase(dev->getSerial());
                    mapDevices.erase(libusb_hotplug_callback_event.dev);
                    delete dev;
                }

                break;
            }
            default: {
                printf("Unhandled event %d\n", libusb_hotplug_callback_event.event);
            }
            }
        }
    }
}

int LIBUSB_CALL libusb_hotplug_callback(struct libusb_context* ctx, struct libusb_device* dev, libusb_hotplug_event event,
        void* user_data) {
    unique_lock<mutex> lk(libusb_hotplug_callback_mutex);
    libusb_hotplug_event_queue.push( {ctx, dev, event});
    libusb_hotplug_callback_cv.notify_all();
    return 0;
}

#ifdef WIN32

HDEVNOTIFY hDeviceNotify = nullptr;
HWND hWnd = nullptr;

void deviceArrived(uint16_t vid, uint16_t pid, const uint8_t* sSerial) {
    // It seems we might get a callback before the device is fully visible to libusb
    // So adding a delay here
    std::this_thread::sleep_for(1000ms);

    printf("DEBUG: WIN32 Detect found %04X:%04X %s, finding matching libusb Device\n", vid, pid, sSerial);
    bool found = false;
    libusb_device** list;
    ssize_t cnt = libusb_get_device_list(NULL, &list);
    if (cnt > 0) {
        for (int i = 0; i < cnt; i++) {
            libusb_device* dev = list[i];
            struct libusb_device_descriptor desc;
            libusb_device_handle* hotplug_handle_e = NULL;
            int retval = libusb_open(dev, &hotplug_handle_e);
            if (retval) {
                continue;
            }
            retval = libusb_get_device_descriptor(dev, &desc);
            if (retval) {
                libusb_close(hotplug_handle_e);
                continue;
            }
            if (desc.idProduct == pid && desc.idVendor == vid) {
                // VID/PID matches
                uint8_t string_descriptor[256];
                retval = libusb_get_string_descriptor_ascii(hotplug_handle_e, desc.iSerialNumber, string_descriptor,
                        sizeof(string_descriptor));

                if (retval != 8) {
                    libusb_close(hotplug_handle_e);
                    continue; // Expected lenght is 8 for our device
                }
                // Microsoft Windows is case insensitive. It even changes case for device's serial numbers
                // so we cannot use mixed case in serial numbers on our devices

                if (strcasecmp((const char*)string_descriptor, (const char*)sSerial)) {
                    libusb_close(hotplug_handle_e);
                    continue;
                }
                // Okay, we got our newly attached device!

                libusb_close(hotplug_handle_e);
                libusb_hotplug_callback(ctx, dev, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, NULL);
                found = true;
                break;
            }
            else {
                libusb_close(hotplug_handle_e);
                continue;
            }
        }
        libusb_free_device_list(list, 0);
    }
    static int retry_counter = 0;
    if (!found) {

        retry_counter++;
        if (retry_counter < 10) {
            std::this_thread::sleep_for(1000ms);
            deviceArrived(vid, pid, sSerial);
        }
        else {
            printf("Too many attempts, bailing out!");
            return;
        }
    }
    retry_counter = 0;
}

int extractVidPidfromWindowsDeviceString(uint8_t* WindowsDeviceString, uint16_t* vid, uint16_t* pid,
        uint8_t* sSerial, int* iSerial) {

    static int SerialLen = 8;
    // Windows is case-insensitive and actually might use different cases when called during start up and hotplug events
    if (strncasecmp((const char*)WindowsDeviceString + 0x04, "USB", 3))
    return 1;// Not a USB device

    uint8_t sVid[5], sPid[5];
    memcpy(sVid, WindowsDeviceString + 0x0C, 4);// Extract Vid
    memcpy(sPid, WindowsDeviceString + 0x15, 4);// Extract Pid
    sVid[4] = sPid[4] = 0;// NULL Terminating
    *vid = 0, * pid = 0;

    sscanf_s((const char*)sVid, "%hX", vid);// Convert Vid
    sscanf_s((const char*)sPid, "%hX", pid);// Convert Pid

    // Since windows might change the case of serial numbers
    // We can only use numerical or hex serial numbers
    // For this instance we'll be using numberical

    memcpy(sSerial, WindowsDeviceString + 0x1A, SerialLen);// Extract Vid
    sSerial[SerialLen] = 0;
    sscanf_s((const char*)sSerial, "%d", iSerial);

    return 0;

}

LRESULT __stdcall WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CLOSE: {
            UnregisterDeviceNotification(hDeviceNotify);
            DestroyWindow(hWnd);
            break;
        }
        case WM_DEVICECHANGE: {
            // An extra check to verify the data type
            DEV_BROADCAST_HDR* test = (DEV_BROADCAST_HDR*)(lParam);
            if (test->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) return 1;

            PDEV_BROADCAST_DEVICEINTERFACE b = (PDEV_BROADCAST_DEVICEINTERFACE)lParam;

            // Note: format we're getting is "\\\\?\\USB#VID_DEAD&PID_BEEF#F6D0D4CE5854242A"

            // TODO: is this format fixed. eg, the offsets of the VID, PID and serial,
            // or is there some logic needed to parse this.

            // Total size of the structure as indicated, minus the offset where the string begins
            size_t max_size = b->dbcc_size - ((intptr_t)b->dbcc_name - (intptr_t)b);

            // Determining the offsets
            // "\\\\?\\USB#VID_DEAD&PID_BEEF#F6D0D4CE5854242A"
            //  0 1 23 456789ABCDEF0123456789ABCDEF0123456789
            //  0                  1               2

            if (max_size < 0x29)
            return 1;// Device string too small, not interesting for us

            // We are checking the device string for the string USB. To see whether it is a USB device
            // we could also be looking at the GUID value. However, as we only register for USB devices
            // that check should always pass, and this should give a little confirmation about the string.

            uint16_t vid, pid;
            uint8_t sSerial[17];
            int iSerial;
            uint8_t* DeviceString = (uint8_t*)b->dbcc_name;

            printf("DEBUG: dbcc_name: %s\n", b->dbcc_name);

            extractVidPidfromWindowsDeviceString(DeviceString, &vid, &pid, sSerial, &iSerial);

            switch (wParam) {
                case DBT_DEVICEARRIVAL:
                deviceArrived(vid, pid, sSerial);

                break;
                case DBT_DEVICEREMOVECOMPLETE: {

                    auto controller = mapSerial2Device[iSerial];
                    if (controller) {
                        libusb_device* dev = controller->getLibUsbDevice();
                        libusb_hotplug_callback(ctx, dev, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, NULL);
                    }

                }
                break;
            }
        }
        break;

        default:
        // Send all other messages on to the default windows handler.
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
        break;
    }

    return 1;
}

void windows_hwdet_thread_code() {

    GUID UsbDeviceGuid = {0xa5dcbf10, 0x6530, 0x11d2, 0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}; // GUID_DEVINTERFACE_USB_DEVICE
    printf("DEBUG: Starting WIN32 Hotplug support...\n");

    {
        // https://www.codeproject.com/Articles/119168/Hardware-Change-Detection
        // We need the initial hardware list on startup too

        HDEVINFO hDevInfo = ::SetupDiGetClassDevsW(&UsbDeviceGuid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
        SP_DEVICE_INTERFACE_DATA devInterfaceData;
        ::ZeroMemory(&devInterfaceData, sizeof(SP_DEVICE_INTERFACE_DATA));
        devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        for (DWORD dwCount = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &UsbDeviceGuid, dwCount, &devInterfaceData);
                ++dwCount) {
            SP_DEVINFO_DATA DevInfoData;
            DevInfoData.cbSize = sizeof(DevInfoData);

            uint8_t buffer[256];
            memset(buffer, 0, sizeof(256));
            DWORD size;
            PSP_DEVICE_INTERFACE_DETAIL_DATA DeviceInterfaceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)(buffer);

            // This initial value is required
            // https://msdn.microsoft.com/en-us/library/windows/hardware/ff551120(v=vs.85).aspx
            DeviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

            int result = SetupDiGetDeviceInterfaceDetail(hDevInfo, &devInterfaceData, DeviceInterfaceDetailData, sizeof(buffer),
                    nullptr, nullptr);
            // The results for this function are inverted, zero means failure.
            if (!result)
            result = GetLastError();
            else
            result = 0;

            printf("DeviceInterfaceDetailData.DevicePath : %s\n", DeviceInterfaceDetailData->DevicePath);

            uint16_t vid, pid;
            uint8_t sSerial[17];
            int iSerial;
            uint8_t* DeviceString = (uint8_t*)DeviceInterfaceDetailData->DevicePath;
            extractVidPidfromWindowsDeviceString(DeviceString, &vid, &pid, sSerial, &iSerial);

            if (vid == VID && pid == PID) {
                deviceArrived(vid, pid, sSerial);
            }

        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    static const char* class_name = "DUMMY_CLASS";
    WNDCLASSEXA wx = {};
    wx.cbSize = sizeof(WNDCLASSEX);
    wx.lpfnWndProc = WindowProc; // function which will handle messages
    wx.hInstance = GetModuleHandle(NULL);
    wx.lpszClassName = class_name;
    if (RegisterClassExA(&wx)) {
        hWnd = CreateWindowExA(0, class_name, "dummy_name", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    }
    ShowWindow(hWnd, SW_SHOWNORMAL);
    UpdateWindow(hWnd);

    // https://stackoverflow.com/questions/4081334/using-createwindowex-to-make-a-message-only-window
    // https://stackoverflow.com/questions/6267596/windows-message-only-window-appears-when-i-call-back-from-native-to-managed-cod
    // https://msdn.microsoft.com/en-us/library/aa363432(VS.85).aspx
    // https://docs.microsoft.com/en-us/windows-hardware/drivers/install/system-defined-device-setup-classes-available-to-vendors

    // https://msdn.microsoft.com/en-us/library/windows/desktop/aa363432(v=vs.85).aspx

    // See also
    // https://docs.microsoft.com/en-us/windows/desktop/devio/registering-for-device-notification
    // for closing

    DEV_BROADCAST_DEVICEINTERFACE NotificationFilter;

    ZeroMemory(&NotificationFilter, sizeof(NotificationFilter));
    NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    NotificationFilter.dbcc_classguid = UsbDeviceGuid;

    hDeviceNotify = RegisterDeviceNotification(hWnd,// events recipient
            &NotificationFilter,// type of device
            DEVICE_NOTIFY_WINDOW_HANDLE// type of recipient handle
    );

    MSG msg;
    int retVal = 1;

    // A Message loop, this must be run in a loop to receive hotplug notifications
    while (retVal) {
        retVal = GetMessage(&msg, hWnd, 0, 0);
        if (retVal <= 0) {
            //ErrorHandler(TEXT("GetMessage"));
            break;
        }
        else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}
#endif

int main() {

    auto version = libusb_get_version();
    printf("Using libusb version %d.%d.%d.%d\n", version->major, version->minor, version->micro, version->nano);

    printf("Initialising libusb...\n");

    int res = libusb_init(&ctx);

    if (res) {
        fprintf(stderr, "Error initialising libusb.\n");
        return res;
    }

    printf("Starting hotplug callback thread...\n");
    libusb_hotplug_callback_thread_running = true;
    libusb_hotplug_callback_thread = thread(libusb_hotplug_callback_thread_code);

    printf("Starting events thread...\n");
    libusb_handle_events_thread_running = true;
    libusb_handle_events_thread = thread(libusb_handle_events_thread_code);

    printf("Registering hotplug callback...\n");

    res = libusb_hotplug_register_callback(ctx,
            libusb_hotplug_event(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT), LIBUSB_HOTPLUG_ENUMERATE,
            VID, PID, LIBUSB_HOTPLUG_MATCH_ANY, libusb_hotplug_callback, nullptr, nullptr);

#ifdef WIN32
	// Current Windows version of libusb has no hotplug support
	// Therefore we need to run some custom hotplug detection code
	// https://github.com/libusb/libusb/issues/86

	if (res == LIBUSB_ERROR_NOT_SUPPORTED) {
		// Start the windows specific hotplug thread code
		printf("Hotplug not supported on this version.\nStarting our own detection thread...\n");
		windows_hwdet_thread = thread(windows_hwdet_thread_code);
	}

#endif

    while (1)
        this_thread::sleep_for(100s);
}

