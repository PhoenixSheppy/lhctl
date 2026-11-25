#include "model.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace lh {

namespace {

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

const char *op_activity(Op op) {
    switch (op) {
    case Op::On: return "Waking up";
    case Op::Standby: return "Going to standby";
    case Op::Sleep: return "Going to sleep";
    case Op::Identify: return "Blinking";
    default: return "Checking";
    }
}

uint8_t op_byte(Op op) {
    switch (op) {
    case Op::On: return cmd::On;
    case Op::Standby: return cmd::Standby;
    default: return cmd::Sleep;
    }
}

AutoOff parse_auto_off(const std::string &s) {
    if (s == "standby")
        return AutoOff::Standby;
    if (s == "sleep")
        return AutoOff::Sleep;
    return AutoOff::Nothing;
}

}  // namespace

const char *auto_off_name(AutoOff a) {
    switch (a) {
    case AutoOff::Standby: return "standby";
    case AutoOff::Sleep: return "sleep";
    default: return "nothing";
    }
}

std::string default_config_path() {
    // Inside the Flatpak this is ~/.var/app/<id>/config/lhctl/config.ini.
    gchar *p = g_build_filename(g_get_user_config_dir(), "lhctl", "config.ini", nullptr);
    std::string s = p;
    g_free(p);
    return s;
}

Model::Model(std::string config_path) : path_(std::move(config_path)) {}

Model::~Model() { wait_idle(15000); }

void Model::load() {
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path_.c_str(), G_KEY_FILE_NONE, nullptr)) {
        std::lock_guard<std::mutex> lock(m_);
        settings_.auto_on = g_key_file_get_boolean(kf, "settings", "auto_on", nullptr);
        gchar *off = g_key_file_get_string(kf, "settings", "auto_off", nullptr);
        settings_.auto_off = parse_auto_off(off ? off : "");
        g_free(off);
        gsize n = 0;
        gchar **groups = g_key_file_get_groups(kf, &n);
        for (gsize i = 0; i < n; i++) {
            std::string g = groups[i];
            if (g.rfind("station ", 0) != 0)
                continue;
            Station s;
            s.address = g.substr(8);
            gchar *name = g_key_file_get_string(kf, groups[i], "name", nullptr);
            gchar *nick = g_key_file_get_string(kf, groups[i], "nickname", nullptr);
            s.name = name ? name : s.address;
            s.nickname = nick ? nick : "";
            g_free(name);
            g_free(nick);
            stations_.push_back(s);
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    std::lock_guard<std::mutex> lock(m_);
    status_ = stations_.empty() ? "No base stations yet. Tap Scan." : "";
    bump();
}

void Model::save_locked() const {
    GKeyFile *kf = g_key_file_new();
    // Keep comments and unknown keys from the existing file (e.g. hand-edited nicknames).
    g_key_file_load_from_file(kf, path_.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_key_file_set_boolean(kf, "settings", "auto_on", settings_.auto_on);
    g_key_file_set_string(kf, "settings", "auto_off", auto_off_name(settings_.auto_off));
    for (const Station &s : stations_) {
        const std::string g = "station " + s.address;
        g_key_file_set_string(kf, g.c_str(), "name", s.name.c_str());
        if (!g_key_file_has_key(kf, g.c_str(), "nickname", nullptr))
            g_key_file_set_string(kf, g.c_str(), "nickname", s.nickname.c_str());
    }
    gchar *dir = g_path_get_dirname(path_.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GError *e = nullptr;
    if (!g_key_file_save_to_file(kf, path_.c_str(), &e)) {
        g_printerr("config: %s\n", e->message);
        g_error_free(e);
    }
    g_key_file_free(kf);
}

Snapshot Model::snapshot() const {
    std::lock_guard<std::mutex> lock(m_);
    Snapshot s;
    s.stations = stations_;
    s.settings = settings_;
    s.scanning = scanning_;
    s.ble_ok = ble_ok_;
    s.status = status_;
    s.version = version_.load();
    return s;
}

bool Model::busy() const {
    std::lock_guard<std::mutex> lock(m_);
    return active_ > 0;
}

void Model::set_status(const std::string &s) {
    std::lock_guard<std::mutex> lock(m_);
    status_ = s;
    bump();
}

bool Model::ensure_ble() {
    std::lock_guard<std::mutex> lock(ble_init_m_);
    std::string err;
    const bool ok = ble_.init(&err);
    {
        std::lock_guard<std::mutex> l(m_);
        ble_ok_ = ok;
        if (!ok)
            status_ = "Bluetooth: " + err;
        bump();
    }
    if (!ok)
        g_printerr("ble: %s\n", err.c_str());
    return ok;
}

void Model::spawn(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(m_);
        active_++;
    }
    std::thread([this, fn = std::move(fn)] {
        fn();
        std::lock_guard<std::mutex> lock(m_);
        active_--;
        bump();
        idle_cv_.notify_all();
    }).detach();
}

bool Model::wait_idle(int timeout_ms) {
    std::unique_lock<std::mutex> lock(m_);
    return idle_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return active_ == 0; });
}

void Model::edit(const std::string &address, const std::function<void(Station &)> &fn) {
    std::lock_guard<std::mutex> lock(m_);
    for (Station &s : stations_)
        if (s.address == address)
            fn(s);
    bump();
}

void Model::scan(int seconds, Op then) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (scanning_)
            return;
        scanning_ = true;
        status_ = "Scanning for base stations...";
        bump();
    }
    spawn([this, seconds, then] { scan_job(seconds, then); });
}

void Model::scan_job(int seconds, Op then) {
    std::vector<std::string> targets;
    if (ensure_ble()) {
        std::string err;
        auto found = ble_.scan(seconds, &err);
        std::lock_guard<std::mutex> lock(m_);
        int added = 0;
        for (const Found &f : found) {
            auto it = std::find_if(stations_.begin(), stations_.end(),
                                   [&](const Station &s) { return s.address == f.address; });
            if (it == stations_.end()) {
                Station s;
                s.address = f.address;
                s.name = f.name;
                stations_.push_back(s);
                it = stations_.end() - 1;
                added++;
            }
            it->rssi = f.rssi;
            it->has_rssi = f.has_rssi;
        }
        std::sort(stations_.begin(), stations_.end(),
                  [](const Station &a, const Station &b) { return a.label() < b.label(); });
        if (!err.empty())
            status_ = "Scan failed: " + err;
        else if (found.empty())
            status_ = "No base stations found. Are they plugged in?";
        else
            status_ = "Found " + std::to_string(found.size()) + " base station" + (found.size() == 1 ? "" : "s") +
                      (added ? " (" + std::to_string(added) + " new)" : "");
        save_locked();
        for (const Found &f : found)
            targets.push_back(f.address);
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        scanning_ = false;
        bump();
    }
    for (const std::string &a : targets)
        run(a, then);
}

void Model::run(const std::string &address, Op op) {
    {
        std::lock_guard<std::mutex> lock(m_);
        auto it = std::find_if(stations_.begin(), stations_.end(),
                               [&](const Station &s) { return s.address == address; });
        if (it == stations_.end() || it->busy)
            return;
        it->busy = true;
        it->activity = "Connecting";
        it->error.clear();
        bump();
    }
    spawn([this, address, op] { station_job(address, op); });
}

void Model::run_all(Op op) {
    std::vector<std::string> all;
    {
        std::lock_guard<std::mutex> lock(m_);
        for (const Station &s : stations_)
            all.push_back(s.address);
        if (all.empty() && !scanning_) {
            // Nothing known yet: find them first, then apply the command.
            scanning_ = true;
            status_ = "Scanning for base stations...";
            bump();
            spawn([this, op] { scan_job(6, op); });
            return;
        }
    }
    for (const std::string &a : all)
        run(a, op);
}

void Model::station_job(const std::string &address, Op op) {
    auto fail = [&](const std::string &err) {
        g_printerr("%s: %s\n", address.c_str(), err.c_str());
        edit(address, [&](Station &s) {
            s.busy = false;
            s.error = err;
            s.activity.clear();
        });
    };
    auto set_power = [&](uint8_t b) { edit(address, [&](Station &s) { s.power = power_from_byte(b); }); };

    if (!ensure_ble())
        return fail("Bluetooth unavailable");

    Ble::Session session(ble_);
    std::string err;
    if (!session.open(address, &err))
        return fail(err);
    edit(address, [&](Station &s) { s.activity = op_activity(op); });

    if (op == Op::Identify) {
        if (!session.identify(&err))
            return fail("identify: " + err);
    } else if (op != Op::Refresh) {
        if (!session.write_power(op_byte(op), &err))
            return fail(err);
        sleep_ms(op == Op::On ? 1500 : 800);
    }

    auto b = session.read_power(&err);
    if (!b)
        return fail(err);
    set_power(*b);

    // Waking takes a few seconds (motor spin-up); keep watching so the UI turns green on its own.
    if (op == Op::On) {
        for (int i = 0; i < 15 && power_from_byte(*b) == Power::Booting; i++) {
            sleep_ms(2000);
            auto again = session.read_power(&err);
            if (!again)
                break;
            b = again;
            set_power(*b);
        }
    }
    session.close();
    edit(address, [&](Station &s) {
        s.busy = false;
        s.activity.clear();
        s.error.clear();
    });
}

void Model::set_auto_on(bool on) {
    std::lock_guard<std::mutex> lock(m_);
    settings_.auto_on = on;
    save_locked();
    bump();
}

void Model::set_auto_off(AutoOff mode) {
    std::lock_guard<std::mutex> lock(m_);
    settings_.auto_off = mode;
    save_locked();
    bump();
}

void Model::set_mock(std::vector<Station> stations, Settings settings, std::string status) {
    std::lock_guard<std::mutex> lock(m_);
    stations_ = std::move(stations);
    settings_ = settings;
    status_ = std::move(status);
    ble_ok_ = true;
    bump();
}

}  // namespace lh
