#include "overlay.h"

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
    const auto e = ov->CreateDashboardOverlay(kKey, kName, &main, &thumb);
    if (e != vr::VROverlayError_None) {
        fprintf(stderr, "steamvr: CreateDashboardOverlay failed: %s%s\n", ov->GetOverlayErrorNameFromEnum(e),
                e == vr::VROverlayError_KeyInUse ? " (another lhctl is running?)" : "");
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

    if (model_.snapshot().settings.auto_on) {
        fprintf(stderr, "auto: waking base stations\n");
        model_.run_all(Op::On);
    }

    Canvas canvas(Ui::W, Ui::H);
    const auto t0 = Clock::now();
    auto last_draw = t0, last_refresh = t0 - std::chrono::hours(1);
    uint64_t drawn_version = 0;
    std::string drawn_hover = "-";
    float mx = -1, my = -1;
    bool visible = ov->IsOverlayVisible(main), dirty = true;

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

    End end = End::Stop;
    while (!stop) {
        vr::VREvent_t ev;
        while (ov->PollNextOverlayEvent(main, &ev, sizeof(ev))) {
            switch (ev.eventType) {
            case vr::VREvent_MouseMove:
                mx = ev.data.mouse.x;
                my = Ui::H - ev.data.mouse.y;   // OpenVR's mouse origin is bottom-left
                if (ui_.hit(mx, my) != drawn_hover)
                    dirty = true;
                break;
            case vr::VREvent_MouseButtonDown:
                if (ev.data.mouse.button == vr::VRMouseButton_Left) {
                    const std::string id = ui_.hit(mx, my);
                    if (!id.empty())
                        ui_.click(id, model_);
                    dirty = true;
                }
                break;
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
    printf("lhctl %s probe\n", LHCTL_VERSION);
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
