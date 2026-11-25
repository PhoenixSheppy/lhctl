// The list of base stations, their state, and the background jobs that talk to them.
#pragma once

#include "ble.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace lh {

enum class Op { Refresh, On, Standby, Sleep, Identify };
enum class AutoOff { Nothing, Standby, Sleep };

struct Station {
    std::string address, name, nickname;
    Power power = Power::Unknown;
    bool busy = false;
    std::string activity;   // shown while busy
    std::string error;      // last failure, cleared on success
    int rssi = 0;
    bool has_rssi = false;
    std::string label() const { return nickname.empty() ? name : nickname; }
};

struct Settings {
    bool auto_on = false;                 // wake all stations when SteamVR starts
    AutoOff auto_off = AutoOff::Nothing;  // what to do when SteamVR quits
};

struct Snapshot {
    std::vector<Station> stations;
    Settings settings;
    bool scanning = false;
    bool ble_ok = false;
    std::string status;
    uint64_t version = 0;
};

class Model {
public:
    explicit Model(std::string config_path);
    ~Model();

    void load();
    Snapshot snapshot() const;
    uint64_t version() const { return version_.load(); }
    bool busy() const;

    // Discovers stations, then runs `then` on every station found (Refresh reads their state).
    void scan(int seconds = 6, Op then = Op::Refresh);
    void run(const std::string &address, Op op);
    void run_all(Op op);
    void set_auto_on(bool on);
    void set_auto_off(AutoOff mode);

    // Blocks until no job is running, up to timeout_ms. Returns false on timeout.
    bool wait_idle(int timeout_ms);

    // For the preview renderer: replaces everything with canned data.
    void set_mock(std::vector<Station> stations, Settings settings, std::string status);

private:
    bool ensure_ble();
    void spawn(std::function<void()> fn);
    void station_job(const std::string &address, Op op);
    void scan_job(int seconds, Op then);
    void edit(const std::string &address, const std::function<void(Station &)> &fn);
    void set_status(const std::string &s);
    void save_locked() const;
    void bump() { version_++; }

    std::string path_;
    Ble ble_;
    std::mutex ble_init_m_;
    bool ble_ok_ = false;

    mutable std::mutex m_;
    std::condition_variable idle_cv_;
    int active_ = 0;
    bool scanning_ = false;
    std::vector<Station> stations_;
    Settings settings_;
    std::string status_;
    std::atomic<uint64_t> version_{1};
};

std::string default_config_path();
const char *auto_off_name(AutoOff a);

}  // namespace lh
