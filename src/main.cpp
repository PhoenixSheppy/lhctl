// lhctl: SteamVR Base Station 2.0 power control for the Steam Frame, as a SteamVR dashboard tab.
//
//   lhctl [run]                run in the background; adds a "Base Stations" tab while SteamVR runs
//   lhctl launch               what the app-menu entry runs: like `run`, but says so with a desktop
//                              notification, and just says "already running" if the service is up
//   lhctl probe                check SteamVR + Bluetooth access (run this first in the Flatpak)
//   lhctl scan                 list nearby base stations
//   lhctl on|standby|sleep|blink [ADDR...]   control all known stations, or just ADDR
//   lhctl preview FILE.png     render the tab with sample data (no headset needed)
#include "model.h"
#include "overlay.h"
#include "ui.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

constexpr const char *kAppId = "com.phoenixnet_labs.LighthouseCtl";

// One lhctl per user, whether started by systemd, the app menu, inside or outside the sandbox.
// $XDG_RUNTIME_DIR/app/<id> is the same folder on the host and inside the Flatpak.
// Returns the held lock fd, -1 if another instance holds it, or -2 if no lock could be made.
int single_instance() {
    gchar *dir = g_build_filename(g_get_user_runtime_dir(), "app", kAppId, nullptr);
    g_mkdir_with_parents(dir, 0700);
    gchar *path = g_build_filename(dir, "lhctl.lock", nullptr);
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    g_free(path);
    g_free(dir);
    if (fd < 0)
        return -2;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Best-effort desktop notification (shows in Desktop Mode; harmless anywhere else).
void notify(const char *body) {
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus)
        return;
    GVariantBuilder actions, hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    GVariant *r = g_dbus_connection_call_sync(
        bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
        "Notify",
        g_variant_new("(susssasa{sv}i)", "Lighthouse Control", 0u, kAppId, "Lighthouse Control", body, &actions,
                      &hints, 8000),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    if (r)
        g_variant_unref(r);
    g_object_unref(bus);
}

std::string data_dir() {
    if (const char *e = getenv("LHCTL_DATA_DIR"))
        return e;
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        gchar *bin = g_path_get_dirname(buf);
        gchar *share = g_build_filename(bin, "..", "share", "lhctl", nullptr);
        std::string s = share;
        const bool ok = g_file_test((s + "/fonts").c_str(), G_FILE_TEST_IS_DIR);
        g_free(bin);
        g_free(share);
        if (ok)
            return s;
    }
    return "resources";   // running from the source tree
}

void usage() {
    fprintf(stderr, "usage: lhctl [run | launch | probe | scan | on|standby|sleep|blink [ADDR...] | preview FILE.png]\n");
}

void print(const lh::Snapshot &s) {
    for (const lh::Station &st : s.stations)
        printf("%-14s %s  %-8s %s\n", st.name.c_str(), st.address.c_str(), lh::power_name(st.power),
               st.error.c_str());
    if (!s.status.empty())
        printf("%s\n", s.status.c_str());
}

int preview(const std::string &out) {
    lh::Ui ui;
    if (!ui.load_fonts(data_dir() + "/fonts"))
        fprintf(stderr, "fonts not found in %s/fonts\n", data_dir().c_str());
    ui.load_logo(data_dir() + "/branding/phoenixnet-labs.png");
    lh::Model model("/dev/null");
    auto st = [](const char *name, const char *nick, const char *addr, lh::Power p, int rssi) {
        lh::Station s;
        s.name = name;
        s.nickname = nick;
        s.address = addr;
        s.power = p;
        s.rssi = rssi;
        s.has_rssi = true;
        return s;
    };
    std::vector<lh::Station> v = {st("LHB-4A1C29F0", "Front left", "D4:6A:91:3C:22:10", lh::Power::On, -58),
                                  st("LHB-7E02B5D3", "Front right", "E8:11:4F:A0:7B:5C", lh::Power::On, -63),
                                  st("LHB-19C0DD42", "", "C2:9B:05:EE:41:07", lh::Power::Booting, -71),
                                  st("LHB-A3F6170B", "Closet", "F1:30:8C:D2:96:AE", lh::Power::Sleep, -80)};
    v[2].busy = true;
    v[2].activity = "Waking up";
    v[3].error = "";
    lh::Settings set;
    set.auto_on = true;
    set.auto_off = lh::AutoOff::Sleep;
    model.set_mock(v, set, "");
    lh::Canvas c(lh::Ui::W, lh::Ui::H);
    ui.render(c, model.snapshot(), 640, 312, 0.9);   // pointer over a button to show hover
    ui.render(c, model.snapshot(), 640, 312, 0.9);
    if (!stbi_write_png(out.c_str(), c.width(), c.height(), 4, c.data(), c.width() * 4))
        return 1;
    lh::Canvas icon(lh::Ui::Thumb, lh::Ui::Thumb);
    lh::Ui::render_icon(icon);
    const std::string icon_out = out.substr(0, out.rfind('.')) + "-icon.png";
    stbi_write_png(icon_out.c_str(), icon.width(), icon.height(), 4, icon.data(), icon.width() * 4);
    printf("wrote %s and %s\n", out.c_str(), icon_out.c_str());
    return 0;
}

int cli(const std::string &verb, const std::vector<std::string> &addrs) {
    lh::Model model(lh::default_config_path());
    model.load();
    if (verb == "scan") {
        model.scan(8);
        sleep(1);
    } else {
        const lh::Op op = verb == "on"        ? lh::Op::On
                          : verb == "standby" ? lh::Op::Standby
                          : verb == "sleep"   ? lh::Op::Sleep
                          : verb == "blink"   ? lh::Op::Identify
                                              : lh::Op::Refresh;
        if (addrs.empty())
            model.run_all(op);
        for (const std::string &a : addrs)
            model.run(a, op);
    }
    // Scans spawn follow-up jobs, so wait until the whole chain is quiet.
    while (!model.wait_idle(500) || model.snapshot().scanning)
        ;
    sleep(1);
    model.wait_idle(60000);
    print(model.snapshot());
    return 0;
}

// Inside the Flatpak, XDG_CONFIG_HOME points at ~/.var/app/<id>/config. Point it back at the real
// ~/.config so sandboxed CLI commands and the host-side service share ~/.config/lhctl/config.ini.
void fix_flatpak_config_home() {
    const char *home = getenv("HOME");
    if (getenv("FLATPAK_ID") && home)
        setenv("XDG_CONFIG_HOME", (std::string(home) + "/.config").c_str(), 1);
}

// SteamVR refuses OpenVR clients from inside the Flatpak sandbox (vrserver internal error 124:
// its IPC doesn't cross the sandbox's namespaces). So for anything that talks to SteamVR, the
// sandboxed copy re-runs this same binary on the host through the Flatpak portal, the way BFR
// runs unsandboxed. Bluetooth-only commands stay in the sandbox. Set LHCTL_NO_HOST=1 to disable.
void run_on_host_if_sandboxed(int argc, char **argv) {
    if (getenv("LHCTL_NO_HOST"))
        return;
    GKeyFile *kf = g_key_file_new();
    const char *info = getenv("LHCTL_FLATPAK_INFO") ? getenv("LHCTL_FLATPAK_INFO") : "/.flatpak-info";   // for tests
    if (!g_key_file_load_from_file(kf, info, G_KEY_FILE_NONE, nullptr)) {
        g_key_file_free(kf);
        return;   // not sandboxed
    }
    gchar *app_path = g_key_file_get_string(kf, "Instance", "app-path", nullptr);
    g_key_file_free(kf);
    if (!app_path) {
        fprintf(stderr, "lhctl: can't find the app's host path in /.flatpak-info; staying sandboxed\n");
        return;
    }
    const std::string host_bin = std::string(app_path) + "/bin/lhctl";
    g_free(app_path);
    std::vector<std::string> args = {"flatpak-spawn", "--host", "--watch-bus", host_bin};
    for (int i = 1; i < argc; i++)
        args.emplace_back(argv[i]);
    std::vector<char *> cargs;
    for (auto &a : args)
        cargs.push_back(a.data());
    cargs.push_back(nullptr);
    execvp(cargs[0], cargs.data());
    perror("lhctl: flatpak-spawn");   // fall through and try in the sandbox
}

}  // namespace

int main(int argc, char **argv) {
    fix_flatpak_config_home();   // before anything calls g_get_user_config_dir(), which caches
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string verb = argc > 1 ? argv[1] : "run";
    std::vector<std::string> rest(argv + std::min(argc, 2), argv + argc);

    if (verb == "-h" || verb == "--help" || verb == "help") {
        usage();
        return 0;
    }
    if (verb == "--version") {
        printf("lhctl %s\n", LHCTL_VERSION);
        return 0;
    }
    if (verb == "run" || verb == "launch" || verb == "probe")
        run_on_host_if_sandboxed(argc, argv);   // only returns if not sandboxed (or it failed)
    if (verb == "probe")
        return lh::probe();
    if (verb == "icon" && rest.size() == 2) {   // lhctl icon SIZE FILE.png (for packaging)
        const int n = atoi(rest[0].c_str());
        lh::Canvas c(n, n);
        lh::Ui::render_icon(c);
        return stbi_write_png(rest[1].c_str(), n, n, 4, c.data(), n * 4) ? 0 : 1;
    }
    if (verb == "preview")
        return preview(rest.empty() ? "preview.png" : rest[0]);
    if (verb == "scan" || verb == "status" || verb == "on" || verb == "standby" || verb == "sleep" ||
        verb == "blink")
        return cli(verb, rest);
    if (verb != "run" && verb != "launch") {
        usage();
        return 2;
    }
    const bool interactive = verb == "launch";
    const int lock = single_instance();
    if (lock == -1) {
        fprintf(stderr, "lhctl is already running\n");
        if (interactive)
            notify("Already running. Open the SteamVR dashboard and look for the Base Stations tab.");
        return 0;
    }
    // Started from the app menu there's no terminal or journal: log to a file that has the same
    // path inside and outside the sandbox: $XDG_RUNTIME_DIR/app/<id>/lhctl.log
    if (!isatty(STDERR_FILENO) && !getenv("JOURNAL_STREAM")) {
        gchar *log = g_build_filename(g_get_user_runtime_dir(), "app", kAppId, "lhctl.log", nullptr);
        if (FILE *f = freopen(log, "w", stderr))
            setvbuf(f, nullptr, _IOLBF, 0);
        g_free(log);
    }
    if (interactive)
        notify("Running in the background. Open the SteamVR dashboard and look for the Base Stations tab.");

    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    lh::Ui ui;
    const std::string fonts = data_dir() + "/fonts";
    if (!ui.load_fonts(fonts))
        fprintf(stderr, "warning: fonts not found in %s; the tab will have no text\n", fonts.c_str());
    if (!ui.load_logo(data_dir() + "/branding/phoenixnet-labs.png"))
        fprintf(stderr, "warning: logo not found in %s/branding\n", data_dir().c_str());
    lh::Model model(lh::default_config_path());
    model.load();
    fprintf(stderr, "lhctl %s: waiting for SteamVR (config %s)\n", LHCTL_VERSION, lh::default_config_path().c_str());
    lh::Overlay(model, ui).run(g_stop);
    fprintf(stderr, "lhctl: bye\n");
    if (lock >= 0)
        close(lock);
    return 0;
}
