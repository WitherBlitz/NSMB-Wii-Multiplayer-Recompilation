#pragma once

// Bluetooth passthrough: what carries the bridge's HCI traffic to an adapter. The libusb one
// (bt_usb_libusb.cpp) drives a real USB adapter; the fake one (FakeTransport in bt_passthrough.cpp,
// NSMBW_BT_FAKE_ADAPTER=1) answers every command itself, to run the game's Bluetooth stack
// without hardware.

#include "bt_hci_bridge.h"

#include <functional>
#include <memory>
#include <string>

namespace BtTransport {

class Transport {
public:
    virtual ~Transport() = default;
    // Starts the adapter thread: it moves the bridge's commands and ACL data to the adapter and
    // the adapter's events and data to the bridge, and calls `linkKeysChanged` (on that thread)
    // after the game paired or forgot a remote.
    virtual void Start(BtBridge::Bridge& bridge, std::function<void()> linkKeysChanged) = 0;
    // Stops it and releases the adapter. Safe to call more than once, and without Start.
    virtual void Stop() = 0;
};

struct ProbeResult {
    std::unique_ptr<Transport> transport;  // null when no adapter can be used
    bool wiiModule = false;                // the Wii's own module (057e:0305)
    std::string adapter;                   // its name, for the log
    std::string detail;                    // why none can be used
};

// Finds and claims a USB Bluetooth adapter. `explicitlyOn`: the setting is "on", not "auto"
// (on Linux, "auto" leaves adapters alone when running as root).
ProbeResult ProbeLibusb(bool explicitlyOn);

}  // namespace BtTransport
