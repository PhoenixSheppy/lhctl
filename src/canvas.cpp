#include "canvas.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace lh {

namespace {

std::vector<uint32_t> decode_utf8(const std::string &s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); k++)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        out.push_back(cp);
        i += n;
    }
    return out;
}

stbtt_fontinfo *info_of(const std::shared_ptr<void> &p) { return static_cast<stbtt_fontinfo *>(p.get()); }

}  // namespace

bool Font::load(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    data_.assign(std::istreambuf_iterator<char>(in), {});
    auto *info = new stbtt_fontinfo;
    if (!stbtt_InitFont(info, data_.data(), stbtt_GetFontOffsetForIndex(data_.data(), 0))) {
        delete info;
        data_.clear();
        return false;
    }
    info_ = std::shared_ptr<void>(info, [](void *p) { delete static_cast<stbtt_fontinfo *>(p); });
    return true;
}

const Font::Glyph &Font::glyph(uint32_t cp, float px) const {
    const auto key = std::make_pair(cp, int(std::lround(px * 4)));
    auto it = cache_.find(key);
    if (it != cache_.end())
        return it->second;
    Glyph g;
    stbtt_fontinfo *info = info_of(info_);
    const float scale = stbtt_ScaleForPixelHeight(info, px);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(info, int(cp), &adv, &lsb);
    g.advance = adv * scale;
    unsigned char *bmp = stbtt_GetCodepointBitmap(info, scale, scale, int(cp), &g.w, &g.h, &g.xoff, &g.yoff);
    if (bmp) {
        g.bitmap.assign(bmp, bmp + size_t(g.w) * g.h);
        stbtt_FreeBitmap(bmp, nullptr);
    }
    return cache_.emplace(key, std::move(g)).first->second;
}

float Font::measure(const std::string &utf8, float px) const {
    if (!ok())
        return 0;
    float w = 0;
    const float scale = stbtt_ScaleForPixelHeight(info_of(info_), px);
    auto cps = decode_utf8(utf8);
    for (size_t i = 0; i < cps.size(); i++) {
        w += glyph(cps[i], px).advance;
        if (i + 1 < cps.size())
            w += stbtt_GetCodepointKernAdvance(info_of(info_), int(cps[i]), int(cps[i + 1])) * scale;
    }
    return w;
}

void Canvas::clear(Color c) {
    for (size_t i = 0; i < px_.size(); i += 4) {
        px_[i] = c.r;
        px_[i + 1] = c.g;
        px_[i + 2] = c.b;
        px_[i + 3] = c.a;
    }
}

void Canvas::blend(int x, int y, Color c, float coverage) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_ || coverage <= 0)
        return;
    const float a = std::min(1.f, coverage) * c.a / 255.f;
    uint8_t *p = &px_[(size_t(y) * w_ + x) * 4];
    p[0] = uint8_t(p[0] + (c.r - p[0]) * a);
    p[1] = uint8_t(p[1] + (c.g - p[1]) * a);
    p[2] = uint8_t(p[2] + (c.b - p[2]) * a);
    p[3] = uint8_t(std::min(255.f, p[3] + (255 - p[3]) * a));
}

// Coverage of pixel center (px, py) by a rounded rectangle, from its signed distance.
float Canvas::rr_coverage(float px, float py, Rect r, float radius) const {
    radius = std::min(radius, std::min(r.w, r.h) / 2);
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    const float qx = std::fabs(px - cx) - (r.w / 2 - radius);
    const float qy = std::fabs(py - cy) - (r.h / 2 - radius);
    const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - radius;
    return std::clamp(0.5f - d, 0.f, 1.f);
}

void Canvas::rect(Rect r, float radius, Color c) {
    const int x0 = std::max(0, int(std::floor(r.x))), x1 = std::min(w_, int(std::ceil(r.x + r.w)));
    const int y0 = std::max(0, int(std::floor(r.y))), y1 = std::min(h_, int(std::ceil(r.y + r.h)));
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            blend(x, y, c, rr_coverage(x + .5f, y + .5f, r, radius));
}

void Canvas::rect_outline(Rect r, float radius, float stroke, Color c) {
    const Rect in{r.x + stroke, r.y + stroke, r.w - 2 * stroke, r.h - 2 * stroke};
    const int x0 = std::max(0, int(std::floor(r.x))), x1 = std::min(w_, int(std::ceil(r.x + r.w)));
    const int y0 = std::max(0, int(std::floor(r.y))), y1 = std::min(h_, int(std::ceil(r.y + r.h)));
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const float outer = rr_coverage(x + .5f, y + .5f, r, radius);
            const float inner = rr_coverage(x + .5f, y + .5f, in, std::max(0.f, radius - stroke));
            blend(x, y, c, outer - inner);
        }
}

void Canvas::circle(float cx, float cy, float radius, Color c) {
    rect({cx - radius, cy - radius, 2 * radius, 2 * radius}, radius, c);
}

void Canvas::ring(float cx, float cy, float radius, float stroke, Color c) {
    rect_outline({cx - radius, cy - radius, 2 * radius, 2 * radius}, radius, stroke, c);
}

void Canvas::vgradient(Rect r, float radius, Color top, Color bottom) {
    const int x0 = std::max(0, int(std::floor(r.x))), x1 = std::min(w_, int(std::ceil(r.x + r.w)));
    const int y0 = std::max(0, int(std::floor(r.y))), y1 = std::min(h_, int(std::ceil(r.y + r.h)));
    for (int y = y0; y < y1; y++) {
        const float t = std::clamp((y + .5f - r.y) / r.h, 0.f, 1.f);
        const Color c{uint8_t(top.r + (bottom.r - top.r) * t), uint8_t(top.g + (bottom.g - top.g) * t),
                      uint8_t(top.b + (bottom.b - top.b) * t), uint8_t(top.a + (bottom.a - top.a) * t)};
        for (int x = x0; x < x1; x++)
            blend(x, y, c, rr_coverage(x + .5f, y + .5f, r, radius));
    }
}

void Canvas::blit(const Canvas &src, int x, int y) {
    for (int sy = 0; sy < src.h_; sy++)
        for (int sx = 0; sx < src.w_; sx++) {
            const uint8_t *p = &src.px_[(size_t(sy) * src.w_ + sx) * 4];
            blend(x + sx, y + sy, Color{p[0], p[1], p[2], 255}, p[3] / 255.f);
        }
}

float Canvas::text(const Font &f, const std::string &utf8, float px, float x, float y, Color c, Align align,
                   float h) {
    if (!f.ok())
        return 0;
    stbtt_fontinfo *info = info_of(f.info_);
    const float scale = stbtt_ScaleForPixelHeight(info, px);
    const float width = f.measure(utf8, px);
    if (align == Align::Center)
        x -= width / 2;
    else if (align == Align::Right)
        x -= width;
    if (h > 0) {
        int x0, y0, x1, y1;
        stbtt_GetCodepointBox(info, 'H', &x0, &y0, &x1, &y1);
        const float cap = y1 * scale;
        y = y + (h + cap) / 2;
    }
    auto cps = decode_utf8(utf8);
    float pen = x;
    for (size_t i = 0; i < cps.size(); i++) {
        const Font::Glyph &g = f.glyph(cps[i], px);
        const int gx = int(std::lround(pen)) + g.xoff, gy = int(std::lround(y)) + g.yoff;
        for (int yy = 0; yy < g.h; yy++)
            for (int xx = 0; xx < g.w; xx++)
                blend(gx + xx, gy + yy, c, g.bitmap[size_t(yy) * g.w + xx] / 255.f);
        pen += g.advance;
        if (i + 1 < cps.size())
            pen += stbtt_GetCodepointKernAdvance(info, int(cps[i]), int(cps[i + 1])) * scale;
    }
    return width;
}

}  // namespace lh
