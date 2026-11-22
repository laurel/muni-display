// Desktop tests for muni.h: the pure helpers, plus a layout check that every element
// drawn in each screen state stays inside the round 720x720 panel.
//   c++ -std=c++17 -Itest/stubs test/test_muni.cpp -o /tmp/test_muni && /tmp/test_muni
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../muni.h"
#include "gzip_fixtures.h"

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

using namespace muni;

static void test_parse() {
  int64_t t;
  CHECK(parse_iso8601("1970-01-01T00:00:00Z", &t) && t == 0);
  CHECK(parse_iso8601("2026-11-08T23:05:01Z", &t) && t == 1794179101);
  CHECK(parse_iso8601("2026-11-08T15:05:01-08:00", &t) && t == 1794179101);
  CHECK(parse_iso8601("2026-11-08T23:05:01.250Z", &t) && t == 1794179101);
  CHECK(parse_iso8601("2024-02-29T12:00:00Z", &t) && t == 1709208000);  // leap day
  CHECK(!parse_iso8601(nullptr, &t));
  CHECK(!parse_iso8601("", &t));
  CHECK(!parse_iso8601("not a time at all!!", &t));
}

static void test_helpers() {
  CHECK(minutes_until(1000, 1000) == 0);
  CHECK(minutes_until(1059, 1000) == 0);
  CHECK(minutes_until(1060, 1000) == 1);
  CHECK(minutes_until(1000 + 12 * 60 + 5, 1000) == 12);
  CHECK(minutes_until(900, 1000) == 0);

  CHECK(clock_text(15, 5) == "3:05 PM");
  CHECK(clock_text(0, 7) == "12:07 AM");
  CHECK(clock_text(12, 0) == "12:00 PM");
  CHECK(clock_text(11, 59) == "11:59 AM");

  CHECK(std::string(occupancy_text("seatsAvailable")) == "seats available");
  CHECK(std::string(occupancy_text("standingAvailable")) == "standing room");
  CHECK(std::string(occupancy_text("full")) == "full");
  CHECK(std::string(occupancy_text("")) == "");

  CHECK(strip_bom("\xEF\xBB\xBF{}") == "{}");
  CHECK(strip_bom("{}") == "{}");

  std::vector<Arrival> all(4);
  all[0].when = 2000;
  all[1].when = 500;   // long gone
  all[2].when = 1500;
  all[3].when = 950;   // 50 s past due: still shown
  auto u = upcoming(all, 1000, 2);
  CHECK(u.size() == 2 && u[0].when == 950 && u[1].when == 1500);
}

static void test_gunzip() {
  for (const auto &c : gz_cases()) {
    std::string out;
    bool ok = gunzip(c.gz, &out);
    printf("  gunzip %-42s %6zu -> %6zu bytes  %s\n", c.name, c.gz.size(), out.size(),
           ok && out == c.plain ? "ok" : "FAIL");
    CHECK(ok && out == c.plain);
  }
  // The real response, unzipped and with its byte-order mark stripped, is the JSON.
  std::string real;
  CHECK(gunzip(gz_cases()[2].gz, &real));
  CHECK(strip_bom(real).rfind("{\"ServiceDelivery\"", 0) == 0);

  // Damaged input is rejected, not crashed on.
  std::string gz = gz_cases()[2].gz, out;
  CHECK(!gunzip(gz.substr(0, gz.size() / 2), &out));  // cut short
  std::string flipped = gz;
  flipped[40] ^= 0x55;
  gunzip(flipped, &out);                                   // must not crash
  CHECK(!gunzip("{\"not\":\"gzip\"}", &out));
  CHECK(!is_gzip("{}"));
  CHECK(is_gzip(gz));
}

// ---------- layout ----------

// Real Roboto metrics at the sizes in muni_display.yaml (weight, size).
static esphome::font::Font F_BIG{500, 482}, F_NOW{500, 300}, F_SECOND{500, 92},
    F_SECOND_UNIT{400, 36}, F_HEADER{500, 36}, F_BODY{400, 28}, F_SMALL{400, 24}, F_BADGE{500, 38};

static Fonts fonts() {
  return {&F_BIG, &F_NOW, &F_SECOND, &F_SECOND_UNIT, &F_HEADER, &F_BODY, &F_SMALL, &F_BADGE};
}


static FILE *dump = nullptr;
static void check_layout(const char *name, esphome::ESPTime now, uint32_t now_ms, bool expect_big = false) {
  esphome::display::Display d;
  render(d, fonts(), Labels{"5", "Fulton", "Market + 4th"}, now, now_ms);
  printf("\n[%s]\n", name);
  bool saw_big = false;
  double closest = 1e9;
  int corner_marks = 0;
  for (auto &op : d.ops) {
    if (op.kind == "pixel") {  // deliberate full-frame marks in the hidden corners
      CHECK((op.x == 0 && op.y == 0) || (op.x == 719 && op.y == 719));
      CHECK(op.c.r || op.c.g || (op.c.b >> 3));  // must not be pure black in RGB565
      corner_marks++;
      continue;
    }
    double worst = 1e9;
    for (int cx : {op.x, op.x + op.w})
      for (int cy : {op.y, op.y + op.h})
        worst = std::min(worst, 360.0 - std::hypot(cx - 360.0, cy - 360.0));
    closest = std::min(closest, worst);
    bool ok = worst >= 8;  // keep at least 8 px of black between ink and the bezel
    printf("  %-6s x=%3d..%3d y=%3d..%3d  edge %5.1f %s %s\n", op.kind.c_str(), op.x, op.x + op.w, op.y,
           op.y + op.h, worst, ok ? "  " : "!!", op.text.c_str());
    if (!ok) {
      fprintf(stderr, "FAIL [%s] too close to the edge: %s '%s'\n", name, op.kind.c_str(), op.text.c_str());
      ++failures;
    }
    if (op.h >= 200)
      saw_big = true;
    if (dump)
      fprintf(dump, "%s{\"scene\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                    "\"weight\":%d,\"size\":%d,\"px\":%d,\"py\":%d,\"c\":[%d,%d,%d]}",
              ftell(dump) > 1 ? ",\n" : "", name, op.kind.c_str(), op.text.c_str(), op.x, op.y, op.w, op.h,
              op.weight, op.size, op.px, op.py, op.c.r, op.c.g, op.c.b);
  }
  printf("  closest to edge: %.1f px\n", closest);
  CHECK(corner_marks == 2);
  CHECK(d.missing.empty());
  if (expect_big)
    CHECK(saw_big);
}

static Arrival arr(int64_t when, bool live, const char *occ, bool at_stop = false) {
  Arrival a;
  a.when = when;
  a.live = live;
  a.occupancy = occ;
  a.at_stop = at_stop;
  return a;
}

static void test_layout() {
  setenv("TZ", "America/Los_Angeles", 1);
  tzset();
  const int64_t now = 1794178386;  // 2026-11-08 2:53:06 PM PT, from the real response
  esphome::ESPTime t = esphome::ESPTime::from_epoch_local(now);
  esphome::ESPTime invalid;

  check_layout("clock not set", invalid, 0);

  g_state = State{};
  check_layout("first fetch pending", t, 1000);

  g_state.ever_ok = true;
  g_state.headsign = "Transit Center";
  g_state.updated = now;
  g_state.updated_ms = 1000;
  g_state.arrivals = {arr(1794179101, true, "seatsAvailable"), arr(1794179912, true, "seatsAvailable"),
                      arr(1794180589, true, "seatsAvailable")};
  check_layout("normal (12 and 25 min)", t, 2000, true);

  g_state.arrivals = {arr(now + 99 * 60 + 30, false, ""), arr(now + 140 * 60, false, "standingAvailable")};
  check_layout("widest: 99 min scheduled, 140 min second", t, 2000, true);

  g_state.arrivals = {arr(now + 125 * 60, false, ""), arr(now + 150 * 60, false, "")};
  check_layout("three-digit minutes", t, 2000, true);

  g_state.arrivals = {arr(now + 20, true, "standingAvailable", true), arr(now + 8 * 60, true, "full")};
  check_layout("bus at the stop", t, 2000, true);

  g_state.arrivals = {arr(now + 88 * 60 + 30, false, "standingAvailable"), arr(now + 99 * 60 + 10, false, "")};
  check_layout("widest two-digit: 88 m scheduled, 99 m scheduled", t, 2000, true);

  g_state.arrivals = {arr(now + 3 * 60, true, "seatsAvailable"), arr(now + 20, true, "", true)};
  check_layout("second bus also at the stop", t, 2000, true);

  g_state.arrivals = {arr(now + 5 * 60, true, "full")};
  check_layout("only one bus listed", t, 2000, true);

  g_state.arrivals = {};
  check_layout("no buses", t, 2000);

  g_state.arrivals = {arr(now + 3 * 60, true, "seatsAvailable"), arr(now + 15 * 60, true, "")};
  g_state.error = "Can't reach 511";
  check_layout("stale with error", t, 2000 + 6 * 60 * 1000, true);
}

int main() {
  dump = fopen("/tmp/muni_ops.json", "w");
  if (dump) fputs("[", dump);
  test_parse();
  test_helpers();
  test_gunzip();
  test_layout();
  if (dump) { fputs("]\n", dump); fclose(dump); }
  printf("\n%s (%d failures)\n", failures ? "FAILED" : "all tests passed", failures);
  return failures ? 1 : 0;
}
