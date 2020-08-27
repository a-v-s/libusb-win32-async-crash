libusb crash test

The program in this repository demonstrates a crash in libusb 1.0.23 on
MicroSoft Windows. 

The crash is triggered when a transfer is active on the moment the USB
device is unplugged.

This example: 

* Initialises libusb
* Starts an events thread:
    This calls libusb_handle_events_completed(ctx, nullptr); in a loop
* Starts a hotplug detect (custom windows fallback)
* When a new device has been detected it will start a receive transmission on endpoint IN1 and send a packet on endpoint OUT1.
* When a transmission has been received on endpoint IN1, it will send the data back on OUT1.

This example is accompanied with a USB demo firmware, that when it received data
on endpoint OUT1 it will respond on IN1. This created a loop of receiving and
sending data. 
Eg. running https://github.com/a-v-s/ucdev/tree/master/demos/usbd/stm32f103xB
on a Blue Pill. 

The crash is triggered when the USB device is unplugged.

