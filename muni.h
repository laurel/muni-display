// Muni arrivals display: data handling and drawing for the round 720x720 screen.
// Included from muni_display.yaml (esphome: includes:). The pure helpers at the top
// compile on a desktop too, for the tests in test/; the rest needs ESPHome.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace muni {

struct Arrival {
  int64_t when = 0;       // unix seconds, UTC (expected time if live, else timetable)
  bool live = false;      // true when 511 gave an ExpectedArrivalTime
  bool at_stop = false;   // VehicleAtStop
  std::string occupancy;  // seatsAvailable | standingAvailable | full | ""
};

struct State {
  std::vector<Arrival> arrivals;  // sorted soonest first
  std::string headsign;           // e.g. "Ocean Beach"
  int64_t updated = 0;            // unix seconds of the last good response
  uint32_t updated_ms = 0;        // millis() of the last good response
  bool ever_ok = false;
  std::string error;              // empty when the last fetch worked
};

inline State g_state;

// ---------- pure helpers (host-testable) ----------

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
inline int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t) era * 146097 + doe - 719468;
}

// Parses "2026-11-08T23:05:01Z", with optional fractional seconds or a +hh:mm offset.
inline bool parse_iso8601(const char *s, int64_t *out) {
  if (s == nullptr || strlen(s) < 19)
    return false;
  int Y, M, D, h, mi, sec;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &mi, &sec) != 6)
    return false;
  const char *p = s + 19;
  if (*p == '.')
    for (++p; *p >= '0' && *p <= '9'; ++p) {
    }
  int64_t offset = 0;
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    if (sscanf(p + 1, "%2d:%2d", &oh, &om) < 1)
      return false;
    offset = (oh * 60 + om) * 60;
    if (*p == '-')
      offset = -offset;
  } else if (*p != 'Z' && *p != '\0') {
    return false;
  }
  *out = days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + sec - offset;
  return true;
}

// Whole minutes until `when`, rounded down; 0 once it's due.
inline int minutes_until(int64_t when, int64_t now) {
  int64_t d = when - now;
  return d <= 0 ? 0 : (int) (d / 60);
}

inline bool is_now(const Arrival &a, int64_t now) { return a.at_stop || minutes_until(a.when, now) == 0; }

// "3:05 PM"
inline std::string clock_text(int hour24, int minute) {
  char b[12];
  int h = hour24 % 12;
  snprintf(b, sizeof b, "%d:%02d %s", h == 0 ? 12 : h, minute, hour24 < 12 ? "AM" : "PM");
  return b;
}

inline const char *occupancy_text(const std::string &o) {
  if (o == "seatsAvailable")
    return "seats available";
  if (o == "standingAvailable")
    return "standing room";
  if (o == "full")
    return "full";
  return "";
}

// Arrivals still worth showing: anything not more than a minute past due, soonest first.
inline std::vector<Arrival> upcoming(const std::vector<Arrival> &all, int64_t now, size_t max_n) {
  std::vector<Arrival> out;
  for (const auto &a : all)
    if (a.at_stop || a.when >= now - 60)
      out.push_back(a);
  std::sort(out.begin(), out.end(), [](const Arrival &x, const Arrival &y) { return x.when < y.when; });
  if (out.size() > max_n)
    out.resize(max_n);
  return out;
}

// ---------- gzip / DEFLATE decoder ----------
// 511 sends gzip-compressed responses. This is a small inflate written after the
// structure of Mark Adler's public "puff" decoder (zlib licence): correctness over speed,
// which is fine for a few kilobytes every 90 seconds.
namespace inflate_ {

struct Bits {
  const uint8_t *in;
  size_t len, pos = 0;
  uint32_t buf = 0;
  int cnt = 0;
  bool err = false;
  int get(int need) {
    uint32_t val = buf;
    while (cnt < need) {
      if (pos >= len) {
        err = true;
        return 0;
      }
      val |= (uint32_t) in[pos++] << cnt;
      cnt += 8;
    }
    buf = need >= 32 ? 0 : val >> need;
    cnt -= need;
    return (int) (val & ((1u << need) - 1));
  }
};

struct Huff {
  short count[16];
  short symbol[288];
};

inline int decode(Bits &s, const Huff &h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= 15; len++) {
    code |= s.get(1);
    if (s.err)
      return -1;
    int count = h.count[len];
    if (code - count < first)
      return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -10;  // ran out of codes
}

// Returns 0 for a complete code, >0 incomplete, <0 over-subscribed.
inline int construct(Huff &h, const short *length, int n) {
  for (int len = 0; len <= 15; len++)
    h.count[len] = 0;
  for (int sym = 0; sym < n; sym++)
    h.count[length[sym]]++;
  if (h.count[0] == n)
    return 0;
  int left = 1;
  for (int len = 1; len <= 15; len++) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0)
      return left;
  }
  short offs[16];
  offs[1] = 0;
  for (int len = 1; len < 15; len++)
    offs[len + 1] = offs[len] + h.count[len];
  for (int sym = 0; sym < n; sym++)
    if (length[sym] != 0)
      h.symbol[offs[length[sym]]++] = (short) sym;
  return left;
}

inline int codes(Bits &s, std::string &out, const Huff &lc, const Huff &dc) {
  static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                  193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int sym = decode(s, lc);
    if (sym < 0)
      return sym;
    if (sym < 256) {
      out.push_back((char) sym);
    } else if (sym == 256) {
      return 0;
    } else {
      sym -= 257;
      if (sym >= 29)
        return -10;
      int len = lbase[sym] + s.get(lext[sym]);
      int ds = decode(s, dc);
      if (ds < 0)
        return ds;
      if (ds >= 30)
        return -11;
      size_t dist = (size_t) dbase[ds] + s.get(dext[ds]);
      if (s.err)
        return -1;
      if (dist > out.size())
        return -11;
      size_t from = out.size() - dist;
      while (len--)
        out.push_back(out[from++]);
    }
  }
}

inline int stored(Bits &s, std::string &out) {
  s.buf = 0;
  s.cnt = 0;  // skip to a byte boundary
  if (s.pos + 4 > s.len)
    return -1;
  unsigned len = s.in[s.pos] | (s.in[s.pos + 1] << 8);
  unsigned nlen = s.in[s.pos + 2] | (s.in[s.pos + 3] << 8);
  s.pos += 4;
  if (len != (~nlen & 0xffff))
    return -2;
  if (s.pos + len > s.len)
    return -1;
  out.append((const char *) s.in + s.pos, len);
  s.pos += len;
  return 0;
}

inline int fixed(Bits &s, std::string &out) {
  static Huff lc, dc;
  static bool built = false;
  if (!built) {
    short lengths[288];
    int sym = 0;
    for (; sym < 144; sym++)
      lengths[sym] = 8;
    for (; sym < 256; sym++)
      lengths[sym] = 9;
    for (; sym < 280; sym++)
      lengths[sym] = 7;
    for (; sym < 288; sym++)
      lengths[sym] = 8;
    construct(lc, lengths, 288);
    for (sym = 0; sym < 30; sym++)
      lengths[sym] = 5;
    construct(dc, lengths, 30);
    built = true;
  }
  return codes(s, out, lc, dc);
}

inline int dynamic(Bits &s, std::string &out) {
  static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  short lengths[320];
  Huff lc, dc;
  int nlen = s.get(5) + 257;
  int ndist = s.get(5) + 1;
  int ncode = s.get(4) + 4;
  if (s.err)
    return -1;
  if (nlen > 286 || ndist > 30)
    return -3;
  int index = 0;
  for (; index < ncode; index++)
    lengths[order[index]] = (short) s.get(3);
  for (; index < 19; index++)
    lengths[order[index]] = 0;
  if (s.err)
    return -1;
  if (construct(lc, lengths, 19) != 0)
    return -4;
  index = 0;
  while (index < nlen + ndist) {
    int sym = decode(s, lc);
    if (sym < 0)
      return sym;
    if (sym < 16) {
      lengths[index++] = (short) sym;
    } else {
      short len = 0;
      if (sym == 16) {
        if (index == 0)
          return -5;
        len = lengths[index - 1];
        sym = 3 + s.get(2);
      } else if (sym == 17) {
        sym = 3 + s.get(3);
      } else {
        sym = 11 + s.get(7);
      }
      if (s.err)
        return -1;
      if (index + sym > nlen + ndist)
        return -6;
      while (sym--)
        lengths[index++] = len;
    }
  }
  if (lengths[256] == 0)
    return -9;
  int err = construct(lc, lengths, nlen);
  if (err < 0 || (err > 0 && nlen - lc.count[0] != 1))
    return -7;
  err = construct(dc, lengths + nlen, ndist);
  if (err < 0 || (err > 0 && ndist - dc.count[0] != 1))
    return -8;
  return codes(s, out, lc, dc);
}

}  // namespace inflate_

inline bool is_gzip(const std::string &s) {
  return s.size() >= 18 && (uint8_t) s[0] == 0x1f && (uint8_t) s[1] == 0x8b && (uint8_t) s[2] == 8;
}

// Decompresses a gzip body into *out. Returns false if the data is damaged.
inline bool gunzip(const std::string &gz, std::string *out) {
  if (!is_gzip(gz))
    return false;
  const uint8_t *p = (const uint8_t *) gz.data();
  size_t n = gz.size(), pos = 10;
  uint8_t flags = p[3];
  if (flags & 4) {  // FEXTRA
    if (pos + 2 > n)
      return false;
    pos += 2 + (p[pos] | (p[pos + 1] << 8));
  }
  if (flags & 8)  // FNAME
    while (pos < n && p[pos++] != 0) {
    }
  if (flags & 16)  // FCOMMENT
    while (pos < n && p[pos++] != 0) {
    }
  if (flags & 2)  // FHCRC
    pos += 2;
  if (pos >= n)
    return false;

  inflate_::Bits s{p + pos, n - pos};
  std::string res;
  res.reserve(n * 6);
  int last;
  do {
    last = s.get(1);
    int type = s.get(2);
    if (s.err)
      return false;
    int err = type == 0 ? inflate_::stored(s, res)
              : type == 1 ? inflate_::fixed(s, res)
              : type == 2 ? inflate_::dynamic(s, res)
                          : -1;
    if (err != 0)
      return false;
  } while (!last);

  // Trailer: CRC-32 (not checked) then the uncompressed length mod 2^32.
  size_t trailer = pos + s.pos;
  if (trailer + 8 <= n) {
    uint32_t isize = p[trailer + 4] | (p[trailer + 5] << 8) | (p[trailer + 6] << 16) | ((uint32_t) p[trailer + 7] << 24);
    if (isize != (uint32_t) res.size())
      return false;
  }
  *out = std::move(res);
  return true;
}

inline std::string strip_bom(const std::string &s) {
  if (s.size() >= 3 && (uint8_t) s[0] == 0xEF && (uint8_t) s[1] == 0xBB && (uint8_t) s[2] == 0xBF)
    return s.substr(3);
  return s;
}

}  // namespace muni

#ifndef MUNI_HOST_TEST
// ---------- ESPHome side ----------
#include "esphome/components/json/json_util.h"
#include "esphome/components/display/display.h"
#include "esphome/components/font/font.h"
#include "esphome/core/log.h"
#include "esphome/core/time.h"

namespace muni {

static const char *const TAG = "muni";

// Reads one 511 StopMonitoring response into g_state.
inline void handle_response(int status, const std::string &raw, const std::string &line, int64_t now,
                            uint32_t now_ms) {
  State &st = g_state;
  if (status != 200) {
    char b[40];
    snprintf(b, sizeof b, "511 error %d", status);
    st.error = b;
    ESP_LOGW(TAG, "%s: %.200s", b, raw.c_str());
    return;
  }
  std::string body = raw;
  if (is_gzip(raw) && !gunzip(raw, &body)) {  // 511 compresses its responses
    st.error = "Couldn't unzip 511 response";
    ESP_LOGW(TAG, "gunzip failed on %u bytes", (unsigned) raw.size());
    return;
  }
  std::vector<Arrival> list;
  std::string headsign;
  bool ok = esphome::json::parse_json(strip_bom(body), [&](JsonObject root) -> bool {
    JsonVariant delivery = root["ServiceDelivery"]["StopMonitoringDelivery"];
    if (delivery.is<JsonArray>())
      delivery = delivery[0];
    JsonVariant visits = delivery["MonitoredStopVisit"];

    auto add = [&](JsonVariant v) {
      JsonVariant j = v["MonitoredVehicleJourney"];
      const char *lr = j["LineRef"].as<const char *>();
      if (!line.empty() && (lr == nullptr || line != lr))
        return;
      JsonVariant call = j["MonitoredCall"];
      Arrival a;
      int64_t t;
      if (parse_iso8601(call["ExpectedArrivalTime"].as<const char *>(), &t)) {
        a.when = t;
        a.live = true;
      } else if (parse_iso8601(call["AimedArrivalTime"].as<const char *>(), &t)) {
        a.when = t;
      } else {
        return;
      }
      JsonVariant vas = call["VehicleAtStop"];
      if (vas.is<bool>())
        a.at_stop = vas.as<bool>();
      else if (const char *s = vas.as<const char *>())
        a.at_stop = strcmp(s, "true") == 0;
      if (const char *o = j["Occupancy"].as<const char *>())
        a.occupancy = o;
      if (headsign.empty()) {
        const char *hs = call["DestinationDisplay"].as<const char *>();
        if (hs == nullptr || *hs == '\0')
          hs = j["DestinationName"].as<const char *>();
        if (hs != nullptr)
          headsign = hs;
      }
      list.push_back(a);
    };

    if (visits.is<JsonArray>()) {
      for (JsonVariant v : visits.as<JsonArray>())
        add(v);
    } else if (visits.is<JsonObject>()) {  // one result comes back as an object
      add(visits);
    }
    return true;
  });
  if (!ok) {
    st.error = "Unreadable 511 response";
    ESP_LOGW(TAG, "JSON parse failed: %.200s", raw.c_str());
    return;
  }
  std::sort(list.begin(), list.end(), [](const Arrival &x, const Arrival &y) { return x.when < y.when; });
  st.arrivals = list;
  if (!headsign.empty())
    st.headsign = headsign;
  st.updated = now;
  st.updated_ms = now_ms;
  st.ever_ok = true;
  st.error.clear();
  ESP_LOGI(TAG, "%u arrivals for line %s", (unsigned) list.size(), line.c_str());
}

using esphome::Color;
using esphome::ESPTime;
using esphome::display::Display;
using esphome::display::TextAlign;
using esphome::font::Font;

struct Fonts {
  Font *big;          // next-arrival minutes
  Font *now;          // "Now"
  Font *second;       // second-arrival minutes
  Font *second_unit;  // "m" beside it
  Font *header;       // route name
  Font *body;         // live line, second clock time
  Font *small;        // headsign, stop line
  Font *badge;        // route number in the red disc
};

struct Labels {
  const char *route;       // "5"
  const char *route_name;  // "Fulton"
  const char *stop;        // "Market + 4th"
};

static const Color TEXT(0xF1, 0xEF, 0xE8);
static const Color TEXT_2(0xD3, 0xD1, 0xC7);
static const Color TEXT_3(0xB4, 0xB2, 0xA9);
static const Color TEXT_4(0x88, 0x87, 0x80);
static const Color RULE(0x44, 0x44, 0x41);
static const Color BADGE(0xA3, 0x2D, 0x2D);
static const Color BADGE_TEXT(0xFC, 0xEB, 0xEB);
static const Color LIVE(0x97, 0xC4, 0x59);
static const Color AMBER(0xEF, 0x9F, 0x27);
static const Color FULL_FRAME_MARK(0, 0, 8);  // not pure black, so the driver counts it as a change

static const int CX = 360;           // screen centre
// Row positions (720x720), fitted to the circle with the real Roboto metrics so the widest
// possible text keeps >= 8 px from the edge. See layout notes in README.md.
static const int HDR_Y = 85;      // centre of the route disc
static const int BIG_BASE = 488;   // baseline of the next-arrival number
static const int LIVE_BASE = 533;  // baseline of the live/scheduled line
static const int RULE_Y = 545;
static const int STOP_BASE = 576;  // baseline of the stop / updated line
static const int SEC_BASE = 657;   // baseline of the second arrival
static const uint32_t STALE_MS = 5 * 60 * 1000;

inline int text_w(Display &it, Font *f, const char *s) {
  int x1, y1, w, h;
  it.get_text_bounds(0, 0, s, f, TextAlign::BASELINE_LEFT, &x1, &y1, &w, &h);
  return w;
}

// Draws pieces side by side on one baseline, centred as a group on x = CX.
struct Piece {
  Font *font;
  Color color;
  std::string text;
  int gap_before;
};
inline void draw_row(Display &it, int baseline, const std::vector<Piece> &pieces) {
  int total = 0;
  for (const auto &p : pieces)
    total += p.gap_before + text_w(it, p.font, p.text.c_str());
  int x = CX - total / 2;
  for (const auto &p : pieces) {
    x += p.gap_before;
    it.print(x, baseline, p.font, p.color, TextAlign::BASELINE_LEFT, p.text.c_str());
    x += text_w(it, p.font, p.text.c_str());
  }
}

inline std::string local_clock(int64_t when) {
  ESPTime t = ESPTime::from_epoch_local((time_t) when);
  return clock_text(t.hour, t.minute);
}

inline void render(Display &it, const Fonts &f, const Labels &l, ESPTime now_t, uint32_t now_ms) {
  const State &st = g_state;

  // ESPHome's mipi_dsi clears its buffer before each redraw without marking the cleared
  // area as changed, then sends the panel only the box around what was just drawn. Without
  // this, pieces of an earlier, wider number stay on the glass. Touching the two corners
  // (outside the round panel, so never visible) makes every redraw send the whole frame.
  it.draw_pixel_at(0, 0, FULL_FRAME_MARK);
  it.draw_pixel_at(719, 719, FULL_FRAME_MARK);

  // Header: red route disc, route name, headsign.
  const char *headsign = st.headsign.empty() ? "" : st.headsign.c_str();
  std::string to = st.headsign.empty() ? std::string("") : std::string("to ") + headsign;
  int name_w = std::max(text_w(it, f.header, l.route_name), text_w(it, f.small, to.c_str()));
  int hx = CX - (76 + 16 + name_w) / 2;
  int hy = HDR_Y;
  it.filled_circle(hx + 38, hy, 38, BADGE);
  it.print(hx + 38, hy, f.badge, BADGE_TEXT, TextAlign::CENTER, l.route);
  it.print(hx + 92, hy + 4, f.header, TEXT, TextAlign::BASELINE_LEFT, l.route_name);
  if (!to.empty())
    it.print(hx + 92, hy + 34, f.small, TEXT_3, TextAlign::BASELINE_LEFT, to.c_str());

  if (!now_t.is_valid()) {
    it.print(CX, 330, f.header, TEXT_2, TextAlign::CENTER, "Setting the clock...");
    return;
  }
  int64_t now = now_t.timestamp;

  if (!st.ever_ok) {
    it.print(CX, 330, f.header, TEXT_2, TextAlign::CENTER,
             st.error.empty() ? "Getting arrivals..." : st.error.c_str());
    return;
  }

  std::vector<Arrival> next = upcoming(st.arrivals, now, 2);

  if (next.empty()) {
    char b[48];
    snprintf(b, sizeof b, "No %ss scheduled", l.route);
    it.print(CX, 330, f.header, TEXT_2, TextAlign::CENTER, b);
  } else {
    // Next arrival, as big as the circle allows.
    const Arrival &a = next[0];
    if (is_now(a, now)) {
      it.print(CX, BIG_BASE, f.now, TEXT, TextAlign::BASELINE_CENTER, "Now");
    } else {
      int m = minutes_until(a.when, now);
      // The number stands alone at the largest size the circle allows; the "m" in the
      // bottom row carries the unit. Three digits won't fit, so 100+ drops to the "Now" size.
      it.print(CX, BIG_BASE, m < 100 ? f.big : f.now, TEXT, TextAlign::BASELINE_CENTER, std::to_string(m).c_str());
    }
    // Live / scheduled line with the status dot.
    std::string line = a.live ? "Live" : "Scheduled";
    line += " · " + local_clock(a.when);
    const char *occ = occupancy_text(a.occupancy);
    if (*occ)
      line += std::string(" · ") + occ;
    int lw = text_w(it, f.body, line.c_str());
    int lx = CX - (20 + 12 + lw) / 2;
    int ly = LIVE_BASE;
    it.filled_circle(lx + 10, ly - 10, 10, a.live ? LIVE : TEXT_4);
    it.print(lx + 32, ly, f.body, a.occupancy == "full" ? AMBER : TEXT_2, TextAlign::BASELINE_LEFT, line.c_str());
  }

  // Rule, then stop name and update time (amber once the data is old).
  it.horizontal_line(CX - 200, RULE_Y, 400, RULE);
  bool stale = (now_ms - st.updated_ms) > STALE_MS;
  std::string stop_line = std::string(l.stop) + " · updated " + local_clock(st.updated);
  if (stale && !st.error.empty())
    stop_line = st.error + " · last " + local_clock(st.updated);
  it.print(CX, STOP_BASE, f.small, stale ? AMBER : TEXT_4, TextAlign::BASELINE_CENTER, stop_line.c_str());

  // Second arrival on one line; 100+ minutes shows just the clock time.
  if (next.size() > 1) {
    const Arrival &b = next[1];
    std::vector<Piece> row;
    int m = minutes_until(b.when, now);
    if (is_now(b, now)) {
      row = {{f.second, TEXT, "Now", 0}, {f.body, TEXT_3, local_clock(b.when), 20}};
    } else if (m >= 100) {
      row = {{f.body, TEXT_3, "at " + local_clock(b.when) + (b.live ? "" : " sched."), 0}};
    } else {
      row = {{f.second, TEXT, std::to_string(m), 0},
             {f.second_unit, TEXT_2, "m", 4},
             {f.body, TEXT_3, local_clock(b.when) + (b.live ? "" : " sched."), 20}};
    }
    draw_row(it, SEC_BASE, row);
  }
}

}  // namespace muni
#endif  // MUNI_HOST_TEST
