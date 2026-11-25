// lhctl: SteamVR Base Station 2.0 power control for the Steam Frame, as a SteamVR dashboard tab.
//
//   lhctl                      run in the background; adds a "Base Stations" tab while SteamVR runs
//   lhctl probe                check SteamVR + Bluetooth access (run this first in the Flatpak)
//   lhctl scan                 list nearby base stations
//   lhctl on|standby|sleep|blink [ADDR...]   control all known stations, or just ADDR
//   lhctl preview FILE.png     render the tab with sample data (no headset needed)
#include "model.h"
#include "overlay.h"
#include "ui.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <glib.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

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
    fprintf(stderr, "usage: lhctl [run | probe | scan | on|standby|sleep|blink [ADDR...] | preview FILE.png]\n");
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

// Inside the Flatpak, XDG_CONFIG_HOME points at ~/.var/app/<id>/config, but OpenVR looks for
// openvr/openvrpaths.vrpath (where SteamVR is) under it. Point it back at the real ~/.config; the
// manifest grants read access to ~/.config/openvr and read/write to ~/.config/lhctl only.
void fix_flatpak_config_home() {
    const char *home = getenv("HOME");
    if (getenv("FLATPAK_ID") && home)
        setenv("XDG_CONFIG_HOME", (std::string(home) + "/.config").c_str(), 1);
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
    if (verb == "probe")
        return lh::probe();
    if (verb == "preview")
        return preview(rest.empty() ? "preview.png" : rest[0]);
    if (verb == "scan" || verb == "status" || verb == "on" || verb == "standby" || verb == "sleep" ||
        verb == "blink")
        return cli(verb, rest);
    if (verb != "run") {
        usage();
        return 2;
    }

    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    lh::Ui ui;
    const std::string fonts = data_dir() + "/fonts";
    if (!ui.load_fonts(fonts))
        fprintf(stderr, "warning: fonts not found in %s; the tab will have no text\n", fonts.c_str());
    lh::Model model(lh::default_config_path());
    model.load();
    fprintf(stderr, "lhctl %s: waiting for SteamVR (config %s)\n", LHCTL_VERSION, lh::default_config_path().c_str());
    lh::Overlay(model, ui).run(g_stop);
    fprintf(stderr, "lhctl: bye\n");
    return 0;
}
