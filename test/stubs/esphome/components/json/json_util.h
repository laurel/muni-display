// Type-only stand-in for ArduinoJson + esphome::json, so muni.h compiles on a desktop.
#pragma once
#include <functional>
#include <string>
#include <type_traits>
struct JsonArray;
struct JsonObject;
struct JsonVariant {
  JsonVariant operator[](const char *) const { return {}; }
  JsonVariant operator[](int) const { return {}; }
  template <typename T> bool is() const { return false; }
  template <typename T> T as() const { return T{}; }
};
struct JsonArray {
  JsonVariant *begin() { return nullptr; }
  JsonVariant *end() { return nullptr; }
};
struct JsonObject {
  JsonVariant operator[](const char *) const { return {}; }
};
template <> inline JsonArray JsonVariant::as<JsonArray>() const { return {}; }
namespace esphome { namespace json {
inline bool parse_json(const std::string &, const std::function<bool(JsonObject)> &f) { return f(JsonObject{}); }
}}
