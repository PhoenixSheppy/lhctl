#include "overlay.h"

#include <glib.h>
#include <openvr.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace lh {

namespace {

constexpr const char *kKey = "com.phoenixnet_labs.lhctl";
constexpr const char *kName = "Base Stations";

using Clock = std::chrono::steady_clock;
double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Mouse events are in texture pixels (from SetOverlayMouseScale) with the origin at the bottom
// left. If a runtime ignores the mouse scale and sends 0..1 instead, scale it up ourselves.
void to_canvas(const vr::VREvent_Mouse_t &m, float *x, float *y) {
    float mx = m.x, my = m.y;
    if (mx >= 0 && mx <= 1.0f && my >= 0 && my <= 1.0f) {
        mx *= Ui::W;
        my *= Ui::H;
    }
    *x = mx;
    *y = Ui::H - my;
}

bool vr_connect(vr::EVRInitError *out) {
    vr::EVRInitError err = vr::VRInitError_None;
    // Background: never starts SteamVR itself, just fails until it is running.
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (out)
        *out = err;
    return err == vr::VRInitError_None;
}

}  // namespace

void Overlay::run(const std::atomic<bool> &stop) {
    vr::EVRInitError last = vr::VRInitError_None;
    while (!stop) {
        vr::EVRInitError err;
        if (!vr_connect(&err)) {
            if (err != last && err != vr::VRInitError_Init_NoServerForBackgroundApp)
                fprintf(stderr, "steamvr: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
            last = err;
            for (int i = 0; i < 30 && !stop; i++)
                sleep_ms(100);
            continue;
        }
        last = vr::VRInitError_None;
        fprintf(stderr, "steamvr: connected\n");
        const End end = session(stop);
        if (end == End::Quit) {
            // Don't reconnect to a SteamVR that is on its way out.
            for (int i = 0; i < 100 && !stop; i++)
                sleep_ms(100);
        } else if (end == End::Error) {
            for (int i = 0; i < 50 && !stop; i++)
                sleep_ms(100);
        }
    }
}

Overlay::End Overlay::session(const std::atomic<bool> &stop) {
    auto *ov = vr::VROverlay();
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid, thumb = vr::k_ulOverlayHandleInvalid;
    auto e = ov->CreateDashboardOverlay(kKey, kName, &main, &thumb);
    if (e == vr::VROverlayError_KeyInUse) {
        // SteamVR can keep the key from a previous connection (e.g. after a crash); take it back.
        vr::VROverlayHandle_t old = vr::k_ulOverlayHandleInvalid;
        if (ov->FindOverlay(kKey, &old) == vr::VROverlayError_None)
            ov->DestroyOverlay(old);
        e = ov->CreateDashboardOverlay(kKey, kName, &main, &thumb);
    }
    if (e != vr::VROverlayError_None) {
        fprintf(stderr, "steamvr: CreateDashboardOverlay failed: %s\n", ov->GetOverlayErrorNameFromEnum(e));
        vr::VR_Shutdown();
        return End::Error;
    }
    ov->SetOverlayWidthInMeters(main, 1.6f);
    ov->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t scale{float(Ui::W), float(Ui::H)};
    ov->SetOverlayMouseScale(main, &scale);
    ov->SetOverlayFlag(main, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    {
        Canvas icon(Ui::Thumb, Ui::Thumb);
        Ui::render_icon(icon);
        ov->SetOverlayRaw(thumb, icon.pixels().data(), Ui::Thumb, Ui::Thumb, 4);
    }

    // Find what's around as soon as SteamVR is up; wake them too if "Wake with SteamVR" is on.
    const bool wake = model_.snapshot().settings.auto_on;
    fprintf(stderr, "startup: scanning%s\n", wake ? " and waking base stations" : "");
    model_.scan(6, wake ? Op::On : Op::Refresh);

    Canvas canvas(Ui::W, Ui::H);
    const auto t0 = Clock::now();
    auto last_draw = t0, last_refresh = t0;   // the startup scan just refreshed everything
    uint64_t drawn_version = 0;
    std::string drawn_hover = "-";
    float mx = -1, my = -1;
    bool visible = ov->IsOverlayVisible(main), dirty = true;
    auto last_vis_check = t0, last_pointer = t0 - std::chrono::hours(1);
    int clicks_logged = 0;

    auto draw = [&] {
        const Snapshot s = model_.snapshot();
        ui_.render(canvas, s, mx, my, secs(t0, Clock::now()));
        const auto r = ov->SetOverlayRaw(main, canvas.pixels().data(), Ui::W, Ui::H, 4);
        if (r != vr::VROverlayError_None)
            fprintf(stderr, "steamvr: SetOverlayRaw: %s\n", ov->GetOverlayErrorNameFromEnum(r));
        drawn_version = s.version;
        drawn_hover = ui_.hit(mx, my);
        last_draw = Clock::now();
        dirty = false;
    };
    draw();   // so the tab never opens blank
    fprintf(stderr, "steamvr: \"%s\" tab ready\n", kName);

    End end = End::Stop;
    while (!stop) {
        vr::VREvent_t ev;
        while (ov->PollNextOverlayEvent(main, &ev, sizeof(ev))) {
            switch (ev.eventType) {
            case vr::VREvent_MouseMove:
                to_canvas(ev.data.mouse, &mx, &my);
                visible = true;                 // pointer events only come while the tab is shown
                last_pointer = Clock::now();
                if (ui_.hit(mx, my) != drawn_hover)
                    dirty = true;
                break;
            case vr::VREvent_MouseButtonDown: {
                // Use the click's own position: a click can arrive without a MouseMove before it.
                to_canvas(ev.data.mouse, &mx, &my);
                visible = dirty = true;
                last_pointer = Clock::now();
                const std::string id = ui_.hit(mx, my);
                if (clicks_logged < 50) {
                    fprintf(stderr, "input: button %u down raw (%.3f, %.3f) -> canvas (%.0f, %.0f) -> %s\n",
                            ev.data.mouse.button, ev.data.mouse.x, ev.data.mouse.y, mx, my,
                            id.empty() ? "(nothing)" : id.c_str());
                    clicks_logged++;
                }
                if (!id.empty() && ev.data.mouse.button != vr::VRMouseButton_Right)
                    ui_.click(id, model_);
                break;
            }
            case vr::VREvent_ScrollDiscrete:
                if (ev.data.scroll.ydelta != 0) {
                    ui_.scroll(ev.data.scroll.ydelta > 0 ? -1 : 1, int(model_.snapshot().stations.size()));
                    dirty = true;
                }
                break;
            case vr::VREvent_OverlayShown:
                visible = dirty = true;
                if (secs(last_refresh, Clock::now()) > 20 && !model_.busy()) {
                    model_.run_all(Op::Refresh);
                    last_refresh = Clock::now();
                }
                break;
            case vr::VREvent_OverlayHidden:
                visible = false;
                mx = my = -1;
                break;
            default: break;
            }
        }
        bool quit = false;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof(ev)))
            if (ev.eventType == vr::VREvent_Quit)
                quit = true;
        if (quit) {
            end = End::Quit;
            break;
        }

        // Don't rely on Shown/Hidden events alone: ask SteamVR twice a second.
        if (secs(last_vis_check, Clock::now()) > 0.5) {
            last_vis_check = Clock::now();
            const bool v = ov->IsOverlayVisible(main);
            if (v && !visible)
                dirty = true;
            visible = v || secs(last_pointer, Clock::now()) < 2;
        }

        if (visible) {
            const Snapshot s = model_.snapshot();
            const bool anim = Ui::animating(s) && secs(last_draw, Clock::now()) > 0.25;
            if (dirty || s.version != drawn_version || anim)
                draw();
        }
        sleep_ms(33);
    }

    if (end == End::Quit) {
        const AutoOff mode = model_.snapshot().settings.auto_off;
        fprintf(stderr, "steamvr: quitting (auto-off: %s)\n", auto_off_name(mode));
        if (mode != AutoOff::Nothing)
            model_.run_all(mode == AutoOff::Sleep ? Op::Sleep : Op::Standby);
        vr::VRSystem()->AcknowledgeQuit_Exiting();
    }
    ov->DestroyOverlay(main);
    vr::VR_Shutdown();
    if (end == End::Quit)
        model_.wait_idle(30000);   // the Bluetooth jobs don't need SteamVR
    return end;
}

int probe() {
    int bad = 0;
    printf("lhctl %s probe (%s)\n", LHCTL_VERSION,
           g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS) ? "inside the Flatpak sandbox" : "on the host");
    char path[4096] = {};
    uint32_t need = 0;
    const bool installed = vr::VR_IsRuntimeInstalled();
    const bool have_path = vr::VR_GetRuntimePath(path, sizeof(path), &need);
    printf("  OpenVR runtime : %s %s\n", installed ? "found" : "NOT FOUND", have_path ? path : "");
    bad += !installed;

    vr::EVRInitError err;
    if (vr_connect(&err)) {
        printf("  SteamVR        : connected\n");
        vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid, t = vr::k_ulOverlayHandleInvalid;
        const auto e = vr::VROverlay()->CreateDashboardOverlay("com.phoenixnet_labs.lhctl.probe", "probe", &h, &t);
        printf("  Dashboard tab  : %s\n", vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
        bad += e != vr::VROverlayError_None;
        if (e == vr::VROverlayError_None) {
            std::vector<uint8_t> px(64 * 64 * 4, 200);
            const auto r = vr::VROverlay()->SetOverlayRaw(h, px.data(), 64, 64, 4);
            printf("  SetOverlayRaw  : %s\n", vr::VROverlay()->GetOverlayErrorNameFromEnum(r));
            bad += r != vr::VROverlayError_None;
            vr::VROverlay()->DestroyOverlay(h);
        }
        vr::VR_Shutdown();
    } else {
        printf("  SteamVR        : %s (%d)\n", vr::VR_GetVRInitErrorAsEnglishDescription(err), int(err));
        bad++;
    }

    Ble ble;
    std::string berr;
    if (ble.init(&berr))
        printf("  Bluetooth      : ok (%s)\n", ble.adapter().c_str());
    else {
        printf("  Bluetooth      : %s\n", berr.c_str());
        bad++;
    }
    return bad ? 1 : 0;
}

}  // namespace lh
