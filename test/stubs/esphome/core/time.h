#pragma once
#include <cstdint>
#include <ctime>
namespace esphome {
struct ESPTime {
  uint8_t second{}, minute{}, hour{};
  time_t timestamp{};
  bool valid{false};
  bool is_valid() const { return valid; }
  static ESPTime from_epoch_local(time_t t) {  // host: use the process TZ
    std::tm tm{};
    localtime_r(&t, &tm);
    ESPTime r;
    r.hour = tm.tm_hour; r.minute = tm.tm_min; r.second = tm.tm_sec; r.timestamp = t; r.valid = true;
    return r;
  }
};
}
