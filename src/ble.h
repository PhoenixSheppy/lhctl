// BlueZ (D-Bus) access to SteamVR Base Station 2.0 power control.
//
// Base Station 2.0 GATT layout (same as lighthouse_pm / SteamVR):
//   power     00001525-1212-efde-1523-785feabcd124  write 0x01 on, 0x02 standby, 0x00 sleep; read = state byte
//   identify  00008421-1212-efde-1523-785feabcd124  write 0x00 to blink the front LED
// Stations advertise as "LHB-XXXXXXXX".
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

typedef struct _GDBusConnection GDBusConnection;

namespace lh {

enum class Power { Unknown, Sleep, Booting, Standby, On };
const char *power_name(Power p);
Power power_from_byte(uint8_t b);

namespace cmd {
constexpr uint8_t Sleep = 0x00, On = 0x01, Standby = 0x02;
}

struct Found {
    std::string address, name;
    int rssi = 0;
    bool has_rssi = false;
};

class Ble {
public:
    Ble();
    ~Ble();
    Ble(const Ble &) = delete;
    Ble &operator=(const Ble &) = delete;

    // Connects to the system bus and finds a powered adapter. Safe to call again after a failure.
    bool init(std::string *err);
    std::string adapter() const { return adapter_; }

    // Runs discovery for `seconds` and returns every LHB-* device BlueZ knows about.
    std::vector<Found> scan(int seconds, std::string *err);

    // One GATT connection to a station; disconnects when destroyed.
    class Session {
    public:
        Session(Ble &ble) : ble_(ble) {}
        ~Session() { close(); }
        bool open(const std::string &address, std::string *err);
        std::optional<uint8_t> read_power(std::string *err);
        bool write_power(uint8_t value, std::string *err);
        bool identify(std::string *err);
        void close();

    private:
        bool write(const std::string &char_path, bool with_response, uint8_t value, std::string *err);
        Ble &ble_;
        std::string dev_, power_char_, ident_char_;
        bool power_wr_ = true, ident_wr_ = true;
    };

private:
    friend class Session;
    void discovery_acquire();
    void discovery_release();
    bool device_known(const std::string &dev_path);

    GDBusConnection *bus_ = nullptr;
    std::string adapter_;
    std::mutex disc_mutex_;     // ref-counted StartDiscovery/StopDiscovery
    std::mutex connect_mutex_;  // many controllers only create one LE connection at a time
    int disc_refs_ = 0;
};

std::string device_path(const std::string &adapter, const std::string &address);

}  // namespace lh
