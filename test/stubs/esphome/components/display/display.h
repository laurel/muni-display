#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include "esphome/components/font/font.h"
namespace esphome {
struct Color { uint8_t r{}, g{}, b{}; Color() = default; Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {} };
namespace display {
enum class TextAlign { TOP_LEFT, CENTER, BASELINE_LEFT, BASELINE_CENTER };
// One drawn element: its ink box in screen px, plus what's needed to re-render it.
struct Op { std::string kind, text; int x = 0, y = 0, w = 0, h = 0; int weight = 0, size = 0; int px = 0, py = 0; Color c{}; };
inline std::vector<int> utf8(const char *s) {
  std::vector<int> cps;
  for (const unsigned char *p = (const unsigned char *) s; *p;) {
    if (*p < 0x80) cps.push_back(*p++);
    else if ((*p & 0xE0) == 0xC0) { cps.push_back(((p[0] & 0x1F) << 6) | (p[1] & 0x3F)); p += 2; }
    else { cps.push_back('?'); p++; }
  }
  return cps;
}
struct Display {
  std::vector<Op> ops;
  std::vector<std::string> missing;  // glyphs not in the font's metrics
  void get_text_bounds(int, int, const char *s, font::Font *f, TextAlign, int *x1, int *y1, int *w, int *h) {
    float adv = 0;
    for (int cp : utf8(s)) { auto g = f->glyph(cp); adv += g ? g->adv : 0; }
    *x1 = 0; *y1 = 0; *w = (int) std::lround(adv); *h = f->m->ascent + f->m->descent;
  }
  void print(int x, int y, font::Font *f, Color c, TextAlign a, const char *s) {
    int w, h, x1, y1; get_text_bounds(0, 0, s, f, a, &x1, &y1, &w, &h);
    int penx = x, base = y;
    if (a == TextAlign::CENTER || a == TextAlign::BASELINE_CENTER) penx = x - w / 2;
    if (a == TextAlign::CENTER) base = y + (f->m->ascent - f->m->descent) / 2;   // vertical middle of the line box
    if (a == TextAlign::TOP_LEFT) base = y + f->m->ascent;
    float pen = penx; int ix0 = 1 << 30, iy0 = 1 << 30, ix1 = -(1 << 30), iy1 = -(1 << 30);
    for (int cp : utf8(s)) {
      auto g = f->glyph(cp);
      if (!g) { missing.push_back(s); continue; }
      if (g->x1 > g->x0) {
        ix0 = std::min(ix0, (int) pen + g->x0); ix1 = std::max(ix1, (int) pen + g->x1);
        iy0 = std::min(iy0, base + g->y0); iy1 = std::max(iy1, base + g->y1);
      }
      pen += g->adv;
    }
    Op o; o.kind = "text"; o.text = s; o.x = ix0; o.y = iy0; o.w = ix1 - ix0; o.h = iy1 - iy0; o.weight = f->weight; o.size = f->size; o.px = penx; o.py = base; o.c = c;
    ops.push_back(o);
  }
  void filled_circle(int cx, int cy, int r, Color c) { Op o; o.kind = "circle"; o.x = cx - r; o.y = cy - r; o.w = o.h = 2 * r; o.c = c; ops.push_back(o); }
  void draw_pixel_at(int x, int y, Color c) { Op o; o.kind = "pixel"; o.x = x; o.y = y; o.w = o.h = 1; o.c = c; ops.push_back(o); }
  void horizontal_line(int x, int y, int w, Color c) { Op o; o.kind = "line"; o.x = x; o.y = y; o.w = w; o.h = 1; o.c = c; ops.push_back(o); }
};
}}
