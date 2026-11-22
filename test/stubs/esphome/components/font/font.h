#pragma once
#include "../../../font_metrics.h"
namespace esphome { namespace font {
// Desktop stand-in: real Roboto metrics for one (weight, size).
struct Font {
  int weight, size;
  const FontM *m = nullptr;
  Font(int w, int s) : weight(w), size(s) {
    for (const auto &f : ALL_FONTS) if (f.weight == w && f.size == s) m = &f;
  }
  const GlyphM *glyph(int cp) const {
    for (int i = 0; i < m->n; i++) if (m->g[i].cp == cp) return &m->g[i];
    return nullptr;
  }
};
}}
