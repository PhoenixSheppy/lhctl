#include "ble.h"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

namespace lh {

namespace {

constexpr const char *kBluez = "org.bluez";
constexpr const char *kPowerUuid = "00001525-1212-efde-1523-785feabcd124";
constexpr const char *kIdentUuid = "00008421-1212-efde-1523-785feabcd124";
constexpr int kCallMs = 10000;

using Clock = std::chrono::steady_clock;

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string take_error(GError *e) {
    if (!e)
        return "unknown error";
    std::string s = e->message;
    // "GDBus.Error:org.bluez.Error.Failed: le-connection-abort-by-local" -> "Failed: le-connection-abort-by-local"
    const char *prefix = "GDBus.Error:org.bluez.Error.";
    if (s.rfind(prefix, 0) == 0)
        s = s.substr(strlen(prefix));
    g_error_free(e);
    return s;
}

void set_err(std::string *err, const std::string &s) {
    if (err)
        *err = s;
}

// Calls a method on org.bluez; returns the reply (caller unrefs) or nullptr with *err set.
GVariant *call(GDBusConnection *bus, const std::string &path, const char *iface, const char *method,
               GVariant *args, const char *reply_type, int timeout_ms, std::string *err) {
    GError *e = nullptr;
    GVariant *r = g_dbus_connection_call_sync(bus, kBluez, path.c_str(), iface, method, args,
                                              reply_type ? G_VARIANT_TYPE(reply_type) : nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, timeout_ms, nullptr, &e);
    if (!r)
        set_err(err, std::string(method) + ": " + take_error(e));
    return r;
}

GVariant *managed_objects(GDBusConnection *bus, std::string *err) {
    GVariant *r = call(bus, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
                       "(a{oa{sa{sv}}})", kCallMs, err);
    if (!r)
        return nullptr;
    GVariant *inner = g_variant_get_child_value(r, 0);
    g_variant_unref(r);
    return inner;
}

// Calls fn(path, interface, props) for every interface of every object.
void for_each(GVariant *objs, const std::function<void(const char *, const char *, GVariant *)> &fn) {
    GVariantIter it;
    const char *path;
    GVariant *ifaces;
    g_variant_iter_init(&it, objs);
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariantIter it2;
        const char *iface;
        GVariant *props;
        g_variant_iter_init(&it2, ifaces);
        while (g_variant_iter_next(&it2, "{&s@a{sv}}", &iface, &props)) {
            fn(path, iface, props);
            g_variant_unref(props);
        }
        g_variant_unref(ifaces);
    }
}

std::string prop_str(GVariant *props, const char *key) {
    GVariant *v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_STRING);
    if (!v)
        v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_OBJECT_PATH);
    if (!v)
        return {};
    std::string s = g_variant_get_string(v, nullptr);
    g_variant_unref(v);
    return s;
}

std::optional<bool> prop_bool(GVariant *props, const char *key) {
    GVariant *v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_BOOLEAN);
    if (!v)
        return std::nullopt;
    bool b = g_variant_get_boolean(v);
    g_variant_unref(v);
    return b;
}

std::optional<int> prop_i16(GVariant *props, const char *key) {
    GVariant *v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_INT16);
    if (!v)
        return std::nullopt;
    int i = g_variant_get_int16(v);
    g_variant_unref(v);
    return i;
}

std::vector<std::string> prop_strv(GVariant *props, const char *key) {
    std::vector<std::string> out;
    GVariant *v = g_variant_lookup_value(props, key, G_VARIANT_TYPE_STRING_ARRAY);
    if (!v)
        return out;
    gsize n = 0;
    const gchar **a = g_variant_get_strv(v, &n);
    for (gsize i = 0; i < n; i++)
        out.emplace_back(a[i]);
    g_free(a);
    g_variant_unref(v);
    return out;
}

bool is_station_name(const std::string &n) { return n.rfind("LHB-", 0) == 0; }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

}  // namespace

const char *power_name(Power p) {
    switch (p) {
    case Power::Sleep: return "Asleep";
    case Power::Booting: return "Waking";
    case Power::Standby: return "Standby";
    case Power::On: return "On";
    default: return "Unknown";
    }
}

Power power_from_byte(uint8_t b) {
    switch (b) {
    case 0x00: return Power::Sleep;
    case 0x02: return Power::Standby;
    case 0x0b: return Power::On;
    case 0x01:
    case 0x08:
    case 0x09: return Power::Booting;
    default: return Power::Unknown;
    }
}

std::string device_path(const std::string &adapter, const std::string &address) {
    std::string a = address;
    std::replace(a.begin(), a.end(), ':', '_');
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return adapter + "/dev_" + a;
}

Ble::Ble() = default;

Ble::~Ble() {
    if (bus_)
        g_object_unref(bus_);
}

bool Ble::init(std::string *err) {
    if (!bus_) {
        GError *e = nullptr;
        bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e);
        if (!bus_) {
            set_err(err, "system bus: " + take_error(e));
            return false;
        }
    }
    GVariant *objs = managed_objects(bus_, err);
    if (!objs) {
        if (err)
            *err = "BlueZ not reachable (" + *err + ")";
        return false;
    }
    std::string first, powered;
    for_each(objs, [&](const char *path, const char *iface, GVariant *props) {
        if (strcmp(iface, "org.bluez.Adapter1") != 0)
            return;
        if (first.empty())
            first = path;
        if (powered.empty() && prop_bool(props, "Powered").value_or(false))
            powered = path;
    });
    g_variant_unref(objs);
    if (first.empty()) {
        set_err(err, "no Bluetooth adapter");
        return false;
    }
    adapter_ = powered.empty() ? first : powered;
    if (powered.empty()) {
        // Try to switch the radio on; this fails harmlessly if rfkill blocks it.
        GVariant *r = call(bus_, adapter_, "org.freedesktop.DBus.Properties", "Set",
                           g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE)),
                           nullptr, kCallMs, err);
        if (!r) {
            if (err)
                *err = "Bluetooth is off (" + *err + ")";
            return false;
        }
        g_variant_unref(r);
    }
    return true;
}

void Ble::discovery_acquire() {
    std::lock_guard<std::mutex> lock(disc_mutex_);
    if (disc_refs_++ > 0)
        return;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "Transport", g_variant_new_string("le"));
    if (GVariant *r = call(bus_, adapter_, "org.bluez.Adapter1", "SetDiscoveryFilter", g_variant_new("(a{sv})", &b),
                           nullptr, kCallMs, nullptr))
        g_variant_unref(r);
    std::string err;
    if (GVariant *r = call(bus_, adapter_, "org.bluez.Adapter1", "StartDiscovery", nullptr, nullptr, kCallMs, &err))
        g_variant_unref(r);
    else
        g_printerr("ble: %s\n", err.c_str());   // InProgress (someone else scanning) is fine.
}

void Ble::discovery_release() {
    std::lock_guard<std::mutex> lock(disc_mutex_);
    if (--disc_refs_ > 0)
        return;
    if (GVariant *r = call(bus_, adapter_, "org.bluez.Adapter1", "StopDiscovery", nullptr, nullptr, kCallMs, nullptr))
        g_variant_unref(r);
}

std::vector<Found> Ble::scan(int seconds, std::string *err) {
    std::vector<Found> out;
    if (!bus_ || adapter_.empty()) {
        set_err(err, "Bluetooth not ready");
        return out;
    }
    discovery_acquire();
    sleep_ms(seconds * 1000);
    GVariant *objs = managed_objects(bus_, err);
    discovery_release();
    if (!objs)
        return out;
    const std::string prefix = adapter_ + "/";
    for_each(objs, [&](const char *path, const char *iface, GVariant *props) {
        if (strcmp(iface, "org.bluez.Device1") != 0 || std::string(path).rfind(prefix, 0) != 0)
            return;
        std::string name = prop_str(props, "Name");
        if (name.empty())
            name = prop_str(props, "Alias");
        if (!is_station_name(name))
            return;
        Found f;
        f.address = prop_str(props, "Address");
        f.name = name;
        if (auto r = prop_i16(props, "RSSI")) {
            f.rssi = *r;
            f.has_rssi = true;
        }
        out.push_back(f);
    });
    g_variant_unref(objs);
    std::sort(out.begin(), out.end(), [](const Found &a, const Found &b) { return a.name < b.name; });
    return out;
}

bool Ble::device_known(const std::string &dev_path) {
    GVariant *r = call(bus_, dev_path, "org.freedesktop.DBus.Properties", "Get",
                       g_variant_new("(ss)", "org.bluez.Device1", "Address"), "(v)", 3000, nullptr);
    if (!r)
        return false;
    g_variant_unref(r);
    return true;
}

bool Ble::Session::open(const std::string &address, std::string *err) {
    if (!ble_.bus_ || ble_.adapter_.empty()) {
        set_err(err, "Bluetooth not ready");
        return false;
    }
    dev_ = device_path(ble_.adapter_, address);

    // BlueZ forgets unpaired LE devices after a while; scan until it shows up again.
    if (!ble_.device_known(dev_)) {
        ble_.discovery_acquire();
        auto until = Clock::now() + std::chrono::seconds(12);
        bool seen = false;
        while (!seen && Clock::now() < until) {
            sleep_ms(500);
            seen = ble_.device_known(dev_);
        }
        ble_.discovery_release();
        if (!seen) {
            set_err(err, "not in range");
            dev_.clear();
            return false;
        }
    }

    // Connect; the first attempt after a scan often aborts, so try twice.
    std::string cerr;
    bool connected = false;
    for (int attempt = 0; attempt < 2 && !connected; attempt++) {
        std::unique_lock<std::mutex> lock(ble_.connect_mutex_);
        if (GVariant *r = call(ble_.bus_, dev_, "org.bluez.Device1", "Connect", nullptr, nullptr, 20000, &cerr)) {
            g_variant_unref(r);
            connected = true;
        } else {
            lock.unlock();
            sleep_ms(700);
        }
    }
    if (!connected) {
        set_err(err, cerr);
        return false;
    }

    // Wait for GATT discovery, then find our two characteristics.
    auto until = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < until) {
        GVariant *objs = managed_objects(ble_.bus_, nullptr);
        bool resolved = false;
        if (objs) {
            const std::string prefix = dev_ + "/";
            for_each(objs, [&](const char *path, const char *iface, GVariant *props) {
                std::string p = path;
                if (p == dev_ && strcmp(iface, "org.bluez.Device1") == 0)
                    resolved = prop_bool(props, "ServicesResolved").value_or(false);
                if (p.rfind(prefix, 0) != 0 || strcmp(iface, "org.bluez.GattCharacteristic1") != 0)
                    return;
                const std::string uuid = lower(prop_str(props, "UUID"));
                const auto flags = prop_strv(props, "Flags");
                const bool wr = std::find(flags.begin(), flags.end(), "write") != flags.end() || flags.empty();
                if (uuid == kPowerUuid) {
                    power_char_ = p;
                    power_wr_ = wr;
                } else if (uuid == kIdentUuid) {
                    ident_char_ = p;
                    ident_wr_ = wr;
                }
            });
            g_variant_unref(objs);
        }
        if (resolved && !power_char_.empty())
            return true;
        sleep_ms(200);
    }
    set_err(err, power_char_.empty() ? "power characteristic not found (is this a 2.0 station?)"
                                     : "GATT services did not resolve");
    return false;
}

std::optional<uint8_t> Ble::Session::read_power(std::string *err) {
    if (power_char_.empty()) {
        set_err(err, "not connected");
        return std::nullopt;
    }
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    GVariant *r = call(ble_.bus_, power_char_, "org.bluez.GattCharacteristic1", "ReadValue",
                       g_variant_new("(a{sv})", &opts), "(ay)", kCallMs, err);
    if (!r)
        return std::nullopt;
    GVariant *bytes = g_variant_get_child_value(r, 0);
    gsize n = 0;
    const uint8_t *d = static_cast<const uint8_t *>(g_variant_get_fixed_array(bytes, &n, 1));
    std::optional<uint8_t> out;
    if (n > 0)
        out = d[0];
    else
        set_err(err, "empty read");
    g_variant_unref(bytes);
    g_variant_unref(r);
    return out;
}

bool Ble::Session::write(const std::string &char_path, bool with_response, uint8_t value, std::string *err) {
    if (char_path.empty()) {
        set_err(err, "characteristic not found");
        return false;
    }
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "type", g_variant_new_string(with_response ? "request" : "command"));
    GVariant *data = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, &value, 1, 1);
    GVariant *r = call(ble_.bus_, char_path, "org.bluez.GattCharacteristic1", "WriteValue",
                       g_variant_new("(@aya{sv})", data, &opts), nullptr, kCallMs, err);
    if (!r)
        return false;
    g_variant_unref(r);
    return true;
}

bool Ble::Session::write_power(uint8_t value, std::string *err) { return write(power_char_, power_wr_, value, err); }

bool Ble::Session::identify(std::string *err) { return write(ident_char_, ident_wr_, 0x00, err); }

void Ble::Session::close() {
    if (dev_.empty())
        return;
    if (GVariant *r = call(ble_.bus_, dev_, "org.bluez.Device1", "Disconnect", nullptr, nullptr, 5000, nullptr))
        g_variant_unref(r);
    dev_.clear();
    power_char_.clear();
    ident_char_.clear();
}

}  // namespace lh
