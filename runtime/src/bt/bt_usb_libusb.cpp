// Bluetooth passthrough over libusb (bt_transport.h): finding a USB Bluetooth adapter this runtime
// may drive, claiming it, and its I/O thread. Windows needs the adapter on the WinUSB driver
// (Zadig); Linux needs permission to its USB device, and gives btusb its interface back on exit.

#include "bt_transport.h"

#if defined(NSMBW_BT_PASSTHROUGH)

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX  // libusb.h includes windows.h
#endif
#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

// The runtime log's format (runtime_log.h), without pulling the guest memory headers in here.
#define BT_LOG(...) std::fprintf(stderr, "[bluetooth] " __VA_ARGS__)

namespace BtTransport {

namespace {

constexpr uint16_t kNintendo = 0x057E;
constexpr uint16_t kWiiModule = 0x0305;
constexpr int kInterface = 0;
// HCI commands: a class request to the device (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS |
// LIBUSB_RECIPIENT_DEVICE), bRequest 0.
constexpr uint8_t kHciCommandRequestType = 0x20;
constexpr int kInTransfersPerEndpoint = 2;
constexpr int kInBufferSize = 1024;
constexpr int kMaxOutInFlight = 64;
constexpr int kMaxConsecutiveInErrors = 50;

struct Endpoints {
    uint8_t event = 0;   // interrupt IN: HCI events
    uint8_t aclIn = 0;   // bulk IN
    uint8_t aclOut = 0;  // bulk OUT
};

bool IsBluetoothClass(uint8_t cls, uint8_t sub, uint8_t protocol) {
    return cls == LIBUSB_CLASS_WIRELESS && sub == 0x01 && protocol == 0x01;
}

// Interface 0 of a Bluetooth adapter carries HCI: one interrupt IN, one bulk IN, one bulk OUT.
bool FindEndpoints(const libusb_config_descriptor* config, Endpoints& out, bool& bluetoothInterface) {
    bluetoothInterface = false;
    if (config == nullptr || config->bNumInterfaces < 1 || config->interface[0].num_altsetting < 1) {
        return false;
    }
    const libusb_interface_descriptor& alt = config->interface[0].altsetting[0];
    bluetoothInterface = IsBluetoothClass(alt.bInterfaceClass, alt.bInterfaceSubClass, alt.bInterfaceProtocol);
    for (int i = 0; i < alt.bNumEndpoints; ++i) {
        const libusb_endpoint_descriptor& e = alt.endpoint[i];
        const int type = e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
        const bool in = (e.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;
        if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in && out.event == 0) {
            out.event = e.bEndpointAddress;
        } else if (type == LIBUSB_TRANSFER_TYPE_BULK && in && out.aclIn == 0) {
            out.aclIn = e.bEndpointAddress;
        } else if (type == LIBUSB_TRANSFER_TYPE_BULK && !in && out.aclOut == 0) {
            out.aclOut = e.bEndpointAddress;
        }
    }
    return out.event != 0 && out.aclIn != 0 && out.aclOut != 0;
}

std::string IdText(const libusb_device_descriptor& d) {
    char text[16];
    std::snprintf(text, sizeof(text), "%04x:%04x", d.idVendor, d.idProduct);
    return text;
}

class LibusbTransport final : public Transport {
public:
    LibusbTransport(libusb_context* context, libusb_device_handle* handle, Endpoints endpoints, bool detached)
        : m_context(context), m_handle(handle), m_endpoints(endpoints), m_detachedDriver(detached) {}
    ~LibusbTransport() override { Stop(); }

    void Start(BtBridge::Bridge& bridge, std::function<void()> linkKeysChanged) override {
        if (m_started) return;
        m_started = true;
        m_bridge = &bridge;
        m_linkKeysChanged = std::move(linkKeysChanged);
        libusb_context* context = m_context;
        bridge.SetWakeCallback([context] { libusb_interrupt_event_handler(context); });
        m_running.store(true);
        for (int i = 0; i < kInTransfersPerEndpoint; ++i) {
            SubmitIn(m_endpoints.event, LIBUSB_TRANSFER_TYPE_INTERRUPT);
            SubmitIn(m_endpoints.aclIn, LIBUSB_TRANSFER_TYPE_BULK);
        }
        m_thread = std::thread([this] { Loop(); });
    }

    void Stop() override {
        if (m_stopped) return;
        m_stopped = true;
        if (m_started) {
            m_running.store(false);
            libusb_interrupt_event_handler(m_context);
            if (m_thread.joinable()) m_thread.join();
            if (m_bridge != nullptr) m_bridge->SetWakeCallback(nullptr);
            for (libusb_transfer* transfer : m_inTransfers) libusb_cancel_transfer(transfer);
            for (int i = 0; i < 100 && (!m_inTransfers.empty() || m_outInFlight.load() > 0); ++i) {
                timeval tv{0, 10000};
                libusb_handle_events_timeout_completed(m_context, &tv, nullptr);
            }
        }
        const bool idle = m_inTransfers.empty() && m_outInFlight.load() == 0;
        if (!m_gone.load()) {
            // An HCI Reset drops the remotes' connections, so none is left hanging on to the adapter.
            uint8_t reset[3] = {0x03, 0x0C, 0x00};
            libusb_control_transfer(m_handle, kHciCommandRequestType,
                                    0, 0, 0, reset, sizeof(reset), 500);
        }
        libusb_release_interface(m_handle, kInterface);  // with auto-detach, Linux's driver comes back here
        if (m_detachedDriver) libusb_attach_kernel_driver(m_handle, kInterface);
        if (idle) {
            libusb_close(m_handle);
            libusb_exit(m_context);
        }
        BT_LOG("released the adapter\n");
    }

private:
    static void LIBUSB_CALL InCallback(libusb_transfer* transfer) {
        static_cast<LibusbTransport*>(transfer->user_data)->OnIn(transfer);
    }
    static void LIBUSB_CALL OutCallback(libusb_transfer* transfer) {
        static_cast<LibusbTransport*>(transfer->user_data)->OnOut(transfer);
    }

    void SubmitIn(uint8_t endpoint, uint8_t type) {
        libusb_transfer* transfer = libusb_alloc_transfer(0);
        auto* buffer = static_cast<unsigned char*>(std::malloc(kInBufferSize));
        if (transfer == nullptr || buffer == nullptr) {
            std::free(buffer);
            if (transfer != nullptr) libusb_free_transfer(transfer);
            return;
        }
        if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
            libusb_fill_interrupt_transfer(transfer, m_handle, endpoint, buffer, kInBufferSize, InCallback, this, 0);
        } else {
            libusb_fill_bulk_transfer(transfer, m_handle, endpoint, buffer, kInBufferSize, InCallback, this, 0);
        }
        transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER;
        const int rc = libusb_submit_transfer(transfer);
        if (rc != 0) {
            BT_LOG("couldn't read from the adapter's endpoint 0x%02X: %s\n", endpoint, libusb_error_name(rc));
            libusb_free_transfer(transfer);
            return;
        }
        m_inTransfers.insert(transfer);
    }

    void OnIn(libusb_transfer* transfer) {
        if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
            m_inErrors = 0;
            if (transfer->actual_length > 0) {
                if (transfer->endpoint == m_endpoints.event) {
                    m_bridge->OnAdapterEventBytes(transfer->buffer, static_cast<size_t>(transfer->actual_length));
                } else {
                    m_bridge->OnAdapterAclBytes(transfer->buffer, static_cast<size_t>(transfer->actual_length));
                }
            }
        } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
            if (!m_gone.exchange(true)) {
                BT_LOG("the adapter was unplugged: the Wii Remotes stay disconnected until the game restarts\n");
            }
        } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
            if (++m_inErrors == 1 || m_inErrors == kMaxConsecutiveInErrors) {
                BT_LOG("reading endpoint 0x%02X failed (status %d)%s\n", transfer->endpoint,
                        static_cast<int>(transfer->status),
                        m_inErrors == kMaxConsecutiveInErrors ? ", giving up on it" : "");
            }
        }
        const bool again = m_running.load() && !m_gone.load() && transfer->status != LIBUSB_TRANSFER_CANCELLED &&
                           m_inErrors < kMaxConsecutiveInErrors;
        if (again && libusb_submit_transfer(transfer) == 0) return;
        m_inTransfers.erase(transfer);
        libusb_free_transfer(transfer);
    }

    void OnOut(libusb_transfer* transfer) {
        m_outInFlight.fetch_sub(1);
        if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
            m_gone.store(true);
        } else if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_CANCELLED &&
                   m_outErrors++ < 10) {
            BT_LOG("sending to the adapter failed (endpoint 0x%02X, status %d)\n", transfer->endpoint,
                    static_cast<int>(transfer->status));
        }
        // LIBUSB_TRANSFER_FREE_TRANSFER: libusb frees it after this returns.
    }

    void SubmitOut(libusb_transfer* transfer) {
        transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;
        m_outInFlight.fetch_add(1);
        const int rc = libusb_submit_transfer(transfer);
        if (rc != 0) {
            m_outInFlight.fetch_sub(1);
            if (rc == LIBUSB_ERROR_NO_DEVICE) m_gone.store(true);
            if (m_outErrors++ < 10) BT_LOG("couldn't send to the adapter: %s\n", libusb_error_name(rc));
            libusb_free_transfer(transfer);  // frees the buffer too
        }
    }

    void SendCommand(const std::vector<uint8_t>& command) {
        libusb_transfer* transfer = libusb_alloc_transfer(0);
        auto* buffer = static_cast<unsigned char*>(std::malloc(LIBUSB_CONTROL_SETUP_SIZE + command.size()));
        if (transfer == nullptr || buffer == nullptr) {
            std::free(buffer);
            if (transfer != nullptr) libusb_free_transfer(transfer);
            return;
        }
        libusb_fill_control_setup(buffer, kHciCommandRequestType, 0, 0,
                                  kInterface, static_cast<uint16_t>(command.size()));
        std::memcpy(buffer + LIBUSB_CONTROL_SETUP_SIZE, command.data(), command.size());
        libusb_fill_control_transfer(transfer, m_handle, buffer, OutCallback, this, 1000);
        SubmitOut(transfer);
    }

    void SendAcl(const std::vector<uint8_t>& packet) {
        libusb_transfer* transfer = libusb_alloc_transfer(0);
        auto* buffer = static_cast<unsigned char*>(std::malloc(std::max<size_t>(packet.size(), 1)));
        if (transfer == nullptr || buffer == nullptr) {
            std::free(buffer);
            if (transfer != nullptr) libusb_free_transfer(transfer);
            return;
        }
        std::memcpy(buffer, packet.data(), packet.size());
        libusb_fill_bulk_transfer(transfer, m_handle, m_endpoints.aclOut, buffer, static_cast<int>(packet.size()),
                                  OutCallback, this, 0);
        SubmitOut(transfer);
    }

    void Loop() {
        std::vector<uint8_t> packet;
        while (m_running.load()) {
            const auto now = BtBridge::Clock::now();
            while (m_bridge->TakeCommand(packet, now)) {
                if (!m_gone.load()) SendCommand(packet);
            }
            while (m_outInFlight.load() < kMaxOutInFlight && m_bridge->TakeAcl(packet)) {
                if (!m_gone.load()) SendAcl(packet);
            }
            if (m_bridge->TakeLinkKeysChanged() && m_linkKeysChanged) m_linkKeysChanged();
            // Woken early by the bridge (libusb_interrupt_event_handler) when the game sends something.
            timeval tv{0, 10000};
            libusb_handle_events_timeout_completed(m_context, &tv, nullptr);
        }
    }

    libusb_context* m_context;
    libusb_device_handle* m_handle;
    Endpoints m_endpoints;
    bool m_detachedDriver;
    BtBridge::Bridge* m_bridge = nullptr;
    std::function<void()> m_linkKeysChanged;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_gone{false};
    std::atomic<int> m_outInFlight{0};
    std::set<libusb_transfer*> m_inTransfers;  // the I/O thread's, and Start/Stop's around it
    int m_inErrors = 0;
    int m_outErrors = 0;
    bool m_started = false;
    bool m_stopped = false;
};

}  // namespace

ProbeResult ProbeLibusb(bool explicitlyOn) {
    ProbeResult result;
    libusb_context* context = nullptr;
    int rc = libusb_init(&context);
    if (rc != 0) {
        result.detail = std::string("libusb could not start (") + libusb_error_name(rc) + ")";
        return result;
    }
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) {
        result.detail = std::string("couldn't list the USB devices (") + libusb_error_name(static_cast<int>(count)) + ")";
        libusb_exit(context);
        return result;
    }
    // The Wii's own module first, then any other adapter in the order the system lists them.
    std::vector<libusb_device*> order(list, list + count);
    std::stable_sort(order.begin(), order.end(), [](libusb_device* a, libusb_device* b) {
        libusb_device_descriptor da{}, db{};
        libusb_get_device_descriptor(a, &da);
        libusb_get_device_descriptor(b, &db);
        const bool wa = da.idVendor == kNintendo && da.idProduct == kWiiModule;
        const bool wb = db.idVendor == kNintendo && db.idProduct == kWiiModule;
        return wa && !wb;
    });
    std::vector<std::string> problems;
    for (libusb_device* device : order) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(device, &d) != 0) continue;
        const bool wii = d.idVendor == kNintendo && d.idProduct == kWiiModule;
        libusb_config_descriptor* config = nullptr;
        if (libusb_get_active_config_descriptor(device, &config) != 0) {
            config = nullptr;
            libusb_get_config_descriptor(device, 0, &config);
        }
        Endpoints endpoints;
        bool bluetoothInterface = false;
        const bool haveEndpoints = FindEndpoints(config, endpoints, bluetoothInterface);
        if (config != nullptr) libusb_free_config_descriptor(config);
        const bool bluetooth =
            wii || bluetoothInterface || IsBluetoothClass(d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol);
        if (!bluetooth) continue;
        std::string name = "Bluetooth adapter " + IdText(d);
        libusb_device_handle* handle = nullptr;
        rc = libusb_open(device, &handle);
        if (rc != 0) {
#if defined(_WIN32)
            if (rc == LIBUSB_ERROR_NOT_SUPPORTED) {
                problems.push_back(name + " uses Windows' own Bluetooth driver: passthrough needs the WinUSB driver (Zadig)");
                continue;
            }
#else
            if (rc == LIBUSB_ERROR_ACCESS) {
                problems.push_back(name + ": no permission to open it (see the udev rule in the passthrough notes)");
                continue;
            }
#endif
            problems.push_back(name + ": couldn't open it (" + libusb_error_name(rc) + ")");
            continue;
        }
        unsigned char product[128] = {};
        if (d.iProduct != 0 &&
            libusb_get_string_descriptor_ascii(handle, d.iProduct, product, sizeof(product) - 1) > 0) {
            name = std::string(reinterpret_cast<const char*>(product)) + " (" + IdText(d) + ")";
        }
        if (!haveEndpoints) {
            problems.push_back(name + ": its interface 0 has no HCI endpoints");
            libusb_close(handle);
            continue;
        }
#if defined(__linux__)
        if (!explicitlyOn && geteuid() == 0) {
            problems.push_back(name + ": running as root, so \"auto\" leaves it to the system (set bluetooth_passthrough = \"on\" to use it)");
            libusb_close(handle);
            continue;
        }
#else
        (void)explicitlyOn;
#endif
        // Linux: take the interface from btusb (and give it back when released). Elsewhere a no-op.
        bool detached = false;
        if (libusb_set_auto_detach_kernel_driver(handle, 1) != LIBUSB_SUCCESS &&
            libusb_kernel_driver_active(handle, kInterface) == 1) {
            detached = libusb_detach_kernel_driver(handle, kInterface) == LIBUSB_SUCCESS;
        }
        rc = libusb_claim_interface(handle, kInterface);
        if (rc != 0) {
            problems.push_back(name + (rc == LIBUSB_ERROR_BUSY ? ": in use by another program"
                                                               : std::string(": couldn't claim it (") + libusb_error_name(rc) + ")"));
            if (detached) libusb_attach_kernel_driver(handle, kInterface);
            libusb_close(handle);
            continue;
        }
        result.transport = std::make_unique<LibusbTransport>(context, handle, endpoints, detached);
        result.wiiModule = wii;
        result.adapter = name;
        break;
    }
    libusb_free_device_list(list, 1);
    if (result.transport) return result;
    libusb_exit(context);
    if (problems.empty()) {
        result.detail = "no USB Bluetooth adapter found";
    } else {
        for (size_t i = 0; i < problems.size(); ++i) {
            result.detail += (i == 0 ? "" : "; ") + problems[i];
        }
    }
    return result;
}

}  // namespace BtTransport

#endif  // NSMBW_BT_PASSTHROUGH
