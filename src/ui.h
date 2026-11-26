// The dashboard tab: layout, drawing, hit testing and click handling.
#pragma once

#include "canvas.h"
#include "model.h"

#include <memory>
#include <string>
#include <vector>

namespace lh {

class Ui {
public:
    static constexpr int W = 1024, H = 760, Thumb = 128;

    bool load_fonts(const std::string &dir);
    // The "A PhoenixNet-Labs Project" badge in the bottom-right corner.
    bool load_logo(const std::string &png);
    // Draws the whole tab. mx/my is the pointer in canvas pixels (negative = none); t is seconds, for animation.
    void render(Canvas &c, const Snapshot &s, float mx, float my, double t);
    static void render_icon(Canvas &c);

    // The control under (x, y) from the last render, or "".
    std::string hit(float x, float y) const;
    // Acts on a control id from hit().
    void click(const std::string &id, Model &model);
    void scroll(int rows, int count);
    // True while something on screen animates (a station is busy or a scan runs).
    static bool animating(const Snapshot &s);

private:
    struct Hit {
        Rect r;
        std::string id;
        bool enabled = true;
    };
    bool button(Canvas &c, Rect r, const std::string &id, const std::string &label, bool enabled, bool active,
                Color active_color, float mx, float my, float font_px = 20);
    void station_card(Canvas &c, const Station &st, Rect r, float mx, float my, double t);

    Font regular_, bold_;
    std::unique_ptr<Canvas> logo_;
    std::vector<Hit> hits_;
    int first_ = 0;   // first visible station row
    int count_ = 0;
};

}  // namespace lh
