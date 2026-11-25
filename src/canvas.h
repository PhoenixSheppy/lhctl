// A tiny software rasterizer: anti-aliased rounded rects, circles and text into an RGBA buffer.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lh {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};
constexpr Color rgb(uint32_t hex, uint8_t a = 255) {
    return Color{uint8_t(hex >> 16), uint8_t(hex >> 8), uint8_t(hex), a};
}

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

class Font {
public:
    bool load(const std::string &path);
    bool ok() const { return !data_.empty(); }
    float measure(const std::string &utf8, float px) const;

private:
    friend class Canvas;
    struct Glyph {
        int w = 0, h = 0, xoff = 0, yoff = 0;
        float advance = 0;
        std::vector<uint8_t> bitmap;
    };
    const Glyph &glyph(uint32_t cp, float px) const;
    std::vector<unsigned char> data_;
    std::shared_ptr<void> info_;   // stbtt_fontinfo
    mutable std::map<std::pair<uint32_t, int>, Glyph> cache_;
};

enum class Align { Left, Center, Right };

class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h), px_(size_t(w) * h * 4, 0) {}
    int width() const { return w_; }
    int height() const { return h_; }
    const uint8_t *data() const { return px_.data(); }
    std::vector<uint8_t> &pixels() { return px_; }

    void clear(Color c);
    void rect(Rect r, float radius, Color c);
    void rect_outline(Rect r, float radius, float stroke, Color c);
    void circle(float cx, float cy, float radius, Color c);
    void ring(float cx, float cy, float radius, float stroke, Color c);
    void vgradient(Rect r, float radius, Color top, Color bottom);
    void blit(const Canvas &src, int x, int y);   // alpha-blends src at (x, y)
    // Draws text with its baseline vertically centered in [y, y+h) when h > 0, else baseline at y.
    float text(const Font &f, const std::string &utf8, float px, float x, float y, Color c,
               Align align = Align::Left, float h = 0);

private:
    void blend(int x, int y, Color c, float coverage);
    float rr_coverage(float px, float py, Rect r, float radius) const;
    int w_, h_;
    std::vector<uint8_t> px_;
};

}  // namespace lh
