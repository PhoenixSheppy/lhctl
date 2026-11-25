#include "ui.h"

#include <algorithm>
#include <cmath>

namespace lh {

namespace {

// Palette
constexpr Color kBgTop = rgb(0x1b1e2c), kBgBottom = rgb(0x11131c);
constexpr Color kCard = rgb(0x232739);
constexpr Color kText = rgb(0xeef0f7), kMuted = rgb(0x9aa1b9), kDim = rgb(0x646b86);
constexpr Color kButton = rgb(0x2e3349), kButtonHover = rgb(0x3a405b), kInk = rgb(0x11131c);
constexpr Color kAccent = rgb(0x6ee7d8), kError = rgb(0xf87171);
constexpr Color kOn = rgb(0x4ade80), kStandby = rgb(0xfbbf24), kSleep = rgb(0x8b93f8), kBooting = rgb(0x38bdf8);

constexpr float kMargin = 32;
constexpr float kListTop = 184, kCardH = 96, kCardGap = 10;
constexpr int kRows = 4;

Color with_alpha(Color c, float a) { return Color{c.r, c.g, c.b, uint8_t(std::clamp(a, 0.f, 1.f) * c.a)}; }

Color power_color(Power p) {
    switch (p) {
    case Power::On: return kOn;
    case Power::Standby: return kStandby;
    case Power::Sleep: return kSleep;
    case Power::Booting: return kBooting;
    default: return kDim;
    }
}

std::string dots(double t) { return std::string(size_t(std::fmod(t * 3, 4)), '.'); }

}  // namespace

bool Ui::load_fonts(const std::string &dir) {
    return regular_.load(dir + "/Inter-Regular.otf") & bold_.load(dir + "/Inter-SemiBold.otf");
}

bool Ui::animating(const Snapshot &s) {
    if (s.scanning)
        return true;
    return std::any_of(s.stations.begin(), s.stations.end(), [](const Station &st) { return st.busy; });
}

std::string Ui::hit(float x, float y) const {
    for (const Hit &h : hits_)
        if (h.enabled && h.r.contains(x, y))
            return h.id;
    return {};
}

void Ui::scroll(int rows, int count) {
    first_ = std::clamp(first_ + rows, 0, std::max(0, count - kRows));
}

bool Ui::button(Canvas &c, Rect r, const std::string &id, const std::string &label, bool enabled, bool active,
                Color active_color, float mx, float my, float font_px) {
    const bool hover = enabled && r.contains(mx, my);
    Color fill = active ? active_color : hover ? kButtonHover : kButton;
    Color ink = active ? kInk : kText;
    if (!enabled) {
        fill = with_alpha(fill, 0.45f);
        ink = with_alpha(ink, 0.4f);
    }
    c.rect(r, r.h / 2.6f, fill);
    if (hover && !active)
        c.rect_outline(r, r.h / 2.6f, 1.5f, with_alpha(kAccent, 0.55f));
    c.text(active ? bold_ : regular_, label, font_px, r.x + r.w / 2, r.y, ink, Align::Center, r.h);
    hits_.push_back({r, id, enabled});
    return hover;
}

void Ui::station_card(Canvas &c, const Station &st, Rect r, float mx, float my, double t) {
    c.rect(r, 20, kCard);

    // Status light: a soft halo when on, a pulse while busy.
    const Color pc = power_color(st.power);
    const float cx = r.x + 44, cy = r.y + r.h / 2;
    float glow = st.power == Power::On ? 0.22f : 0.f;
    if (st.busy)
        glow = 0.12f + 0.18f * float(0.5 + 0.5 * std::sin(t * 5));
    if (glow > 0)
        c.circle(cx, cy, 22, with_alpha(pc, glow));
    c.circle(cx, cy, 11, pc);
    c.circle(cx - 3, cy - 3, 3.5f, with_alpha(rgb(0xffffff), 0.35f));

    // Name and state chip.
    const float tx = r.x + 82;
    const float name_w = c.text(bold_, st.label(), 26, tx, r.y + 18, kText, Align::Left, 32);
    const std::string state = power_name(st.power);
    const float chip_w = regular_.measure(state, 16) + 24;
    const Rect chip{tx + name_w + 12, r.y + 22, chip_w, 26};
    c.rect(chip, 13, with_alpha(pc, 0.18f));
    c.text(regular_, state, 16, chip.x + chip.w / 2, chip.y, pc, Align::Center, chip.h);

    // Second line: what it's doing, what went wrong, or where it is.
    std::string line;
    Color line_c = kMuted;
    if (st.busy) {
        line = st.activity + dots(t);
        line_c = kAccent;
    } else if (!st.error.empty()) {
        line = st.error;
        line_c = kError;
    } else {
        line = (st.nickname.empty() ? "" : st.name + "  ·  ") + st.address;
        if (st.has_rssi)
            line += "  ·  " + std::to_string(st.rssi) + " dBm";
    }
    if (line.size() > 52)
        line = line.substr(0, 50) + "...";
    c.text(regular_, line, 17, tx, r.y + 54, line_c, Align::Left, 24);

    // Controls, right-aligned.
    struct B {
        const char *label, *op;
        Power p;
        Color col;
    } bs[] = {{"On", "on", Power::On, kOn},
              {"Standby", "standby", Power::Standby, kStandby},
              {"Sleep", "sleep", Power::Sleep, kSleep},
              {"Blink", "identify", Power::Unknown, kAccent}};
    const float bw = 96, bh = 52, gap = 10;
    float bx = r.x + r.w - 22 - (4 * bw + 3 * gap);
    for (const B &b : bs) {
        const bool active = b.p != Power::Unknown && st.power == b.p;
        button(c, {bx, r.y + (r.h - bh) / 2, bw, bh}, "st:" + st.address + ":" + b.op, b.label, !st.busy, active,
               b.col, mx, my, 18);
        bx += bw + gap;
    }
}

void Ui::render(Canvas &c, const Snapshot &s, float mx, float my, double t) {
    hits_.clear();
    count_ = int(s.stations.size());
    first_ = std::clamp(first_, 0, std::max(0, count_ - kRows));
    c.clear(kBgBottom);
    c.vgradient({0, 0, float(W), float(H)}, 0, kBgTop, kBgBottom);

    // Header
    {
        Canvas icon(56, 56);
        render_icon(icon);
        c.blit(icon, int(kMargin), 34);
    }
    c.text(bold_, "Base Stations", 34, kMargin + 72, 30, kText, Align::Left, 38);
    std::string sub = s.status;
    Color sub_c = kMuted;
    if (!s.ble_ok && !s.status.empty())
        sub_c = kError;
    if (sub.empty()) {
        int on = 0;
        for (const Station &st : s.stations)
            on += st.power == Power::On;
        sub = std::to_string(count_) + " station" + (count_ == 1 ? "" : "s") + "  ·  " + std::to_string(on) + " on";
    }
    if (s.scanning)
        sub = "Scanning" + dots(t);
    c.text(regular_, sub, 18, kMargin + 72, 68, sub_c, Align::Left, 24);
    button(c, {W - kMargin - 150, 38, 150, 50}, "scan", s.scanning ? "Scanning" : "Scan", !s.scanning, false, kAccent,
           mx, my, 20);

    // Quick actions
    {
        const char *labels[] = {"All on", "All standby", "All asleep", "Refresh"};
        const char *ids[] = {"all:on", "all:standby", "all:sleep", "all:refresh"};
        const Color cols[] = {kOn, kStandby, kSleep, kAccent};
        const float gap = 12, bw = (W - 2 * kMargin - 3 * gap) / 4;
        for (int i = 0; i < 4; i++) {
            const Rect r{kMargin + i * (bw + gap), 110, bw, 52};
            const bool hover = button(c, r, ids[i], labels[i], !s.scanning, false, cols[i], mx, my, 20);
            c.circle(r.x + 26, r.y + r.h / 2, 6, with_alpha(cols[i], hover ? 1.f : 0.8f));
        }
    }

    // Station list
    if (s.stations.empty()) {
        const Rect r{kMargin, kListTop, W - 2 * kMargin, kCardH * 2};
        c.rect(r, 20, kCard);
        c.rect_outline(r, 20, 1.5f, with_alpha(kDim, 0.5f));
        c.text(bold_, s.scanning ? "Looking around..." : "No base stations yet", 26, W / 2.f, r.y + 50, kText,
               Align::Center, 34);
        c.text(regular_, "Plug them in, then tap Scan. 2.0 stations only.", 18, W / 2.f, r.y + 96, kMuted,
               Align::Center, 26);
    }
    for (int i = 0; i < kRows && first_ + i < count_; i++) {
        const Rect r{kMargin, kListTop + i * (kCardH + kCardGap), W - 2 * kMargin, kCardH};
        station_card(c, s.stations[size_t(first_ + i)], r, mx, my, t);
    }

    // Footer: automation settings, plus paging when the list is long.
    const float fy = 618;
    const Rect sw{kMargin, fy + 8, 60, 34};
    const bool auto_on = s.settings.auto_on;
    c.rect(sw, 17, auto_on ? kOn : kButton);
    c.circle(auto_on ? sw.x + sw.w - 17 : sw.x + 17, sw.y + 17, 13, auto_on ? kInk : kMuted);
    hits_.push_back({{sw.x, fy, 300, 50}, "auto_on", true});
    c.text(regular_, "Wake with SteamVR", 19, sw.x + sw.w + 14, fy, kText, Align::Left, 50);

    const char *modes[] = {"Nothing", "Standby", "Sleep"};
    const char *mode_ids[] = {"off:nothing", "off:standby", "off:sleep"};
    const AutoOff mode_vals[] = {AutoOff::Nothing, AutoOff::Standby, AutoOff::Sleep};
    const Color mode_cols[] = {kMuted, kStandby, kSleep};
    const float segw = 104, segh = 44;
    float sx = W - kMargin - 3 * segw - 8 - 4;
    c.text(regular_, "When SteamVR quits", 19, sx - 14, fy, kText, Align::Right, 50);
    c.rect({sx - 4, fy + 1, 3 * segw + 16, segh + 6}, 25, kButton);
    for (int i = 0; i < 3; i++) {
        const bool active = s.settings.auto_off == mode_vals[i];
        const Rect r{sx + i * (segw + 4), fy + 4, segw, segh};
        const bool hover = !active && r.contains(mx, my);
        if (active)
            c.rect(r, 22, mode_cols[i]);
        else if (hover)
            c.rect(r, 22, kButtonHover);
        c.text(active ? bold_ : regular_, modes[i], 18, r.x + r.w / 2, r.y, active ? kInk : kText, Align::Center,
               r.h);
        hits_.push_back({r, mode_ids[i], true});
    }

    if (count_ > kRows) {
        // Paging sits in the header, left of Scan: [<] 1-4 of 6 [>]
        const float right = W - kMargin - 150 - 14;
        button(c, {right - 52, 38, 52, 50}, "down", ">", first_ + kRows < count_, false, kAccent, mx, my, 22);
        c.text(regular_,
               std::to_string(first_ + 1) + "-" + std::to_string(std::min(count_, first_ + kRows)) + " of " +
                   std::to_string(count_),
               17, right - 52 - 60, 38, kMuted, Align::Center, 50);
        button(c, {right - 52 - 120 - 52, 38, 52, 50}, "up", "<", first_ > 0, false, kAccent, mx, my, 22);
    }
}

void Ui::click(const std::string &id, Model &model) {
    auto op_of = [](const std::string &s) {
        if (s == "on") return Op::On;
        if (s == "standby") return Op::Standby;
        if (s == "sleep") return Op::Sleep;
        if (s == "identify") return Op::Identify;
        return Op::Refresh;
    };
    if (id == "scan") {
        model.scan();
    } else if (id.rfind("all:", 0) == 0) {
        model.run_all(op_of(id.substr(4)));
    } else if (id.rfind("st:", 0) == 0) {
        const size_t last = id.rfind(':');
        model.run(id.substr(3, last - 3), op_of(id.substr(last + 1)));
    } else if (id == "auto_on") {
        model.set_auto_on(!model.snapshot().settings.auto_on);
    } else if (id == "off:nothing") {
        model.set_auto_off(AutoOff::Nothing);
    } else if (id == "off:standby") {
        model.set_auto_off(AutoOff::Standby);
    } else if (id == "off:sleep") {
        model.set_auto_off(AutoOff::Sleep);
    } else if (id == "up") {
        scroll(-1, count_);
    } else if (id == "down") {
        scroll(1, count_);
    }
}

// A small, friendly app icon: a rounded tile with a base station "face" and signal arcs.
void Ui::render_icon(Canvas &c) {
    const float s = float(c.width());
    c.clear(Color{0, 0, 0, 0});
    c.vgradient({0, 0, s, s}, s * 0.24f, rgb(0x5eead4), rgb(0x6366f1));
    // Station body
    const Rect body{s * 0.20f, s * 0.24f, s * 0.42f, s * 0.52f};
    c.rect(body, s * 0.10f, rgb(0xf8fafc));
    c.rect({body.x + s * 0.05f, body.y + s * 0.07f, body.w - s * 0.10f, body.h - s * 0.14f}, s * 0.07f,
           rgb(0x1e2133));
    c.circle(body.x + body.w / 2, body.y + body.h / 2, s * 0.06f, rgb(0x6ee7d8));
    // Signal arcs: rings masked to their right half by redrawing the left side with the background.
    for (int i = 0; i < 2; i++) {
        const float r = s * (0.12f + 0.09f * i);
        Canvas arc(c.width(), c.height());
        arc.clear(Color{0, 0, 0, 0});
        arc.ring(body.x + body.w + s * 0.02f, body.y + body.h / 2, r, s * 0.035f, rgb(0xf8fafc, 230));
        auto &a = arc.pixels();
        auto &d = c.pixels();
        const int cut = int(body.x + body.w + s * 0.02f + r * 0.35f);
        for (int y = 0; y < c.height(); y++)
            for (int x = cut; x < c.width(); x++) {
                const size_t k = (size_t(y) * c.width() + x) * 4;
                const float al = a[k + 3] / 255.f;
                if (x > s * 0.93f || y < s * 0.08f || y > s * 0.92f)
                    continue;
                for (int ch = 0; ch < 3; ch++)
                    d[k + ch] = uint8_t(d[k + ch] + (a[k + ch] - d[k + ch]) * al);
            }
    }
}

}  // namespace lh
