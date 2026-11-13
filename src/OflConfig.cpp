#include "OflConfig.h"

#include <math.h>

#include <algorithm>

namespace ofl {
namespace config {

namespace {

bool isNumber(JsonVariantConst v) { return v.is<double>() && !v.is<bool>() && isfinite(v.as<double>()); }

bool numberIn(JsonVariantConst v, double min, double max) {
  return isNumber(v) && v.as<double>() >= min && v.as<double>() <= max;
}

// "HH:MM", 24h — same check as the mock's HHMM regex.
bool isHhMm(JsonVariantConst v) {
  if (!v.is<const char *>()) return false;
  const char *s = v.as<const char *>();
  if (strlen(s) != 5 || s[2] != ':') return false;
  for (int i : {0, 1, 3, 4})
    if (s[i] < '0' || s[i] > '9') return false;
  int h = (s[0] - '0') * 10 + (s[1] - '0');
  int m = (s[3] - '0') * 10 + (s[4] - '0');
  return h < 24 && m < 60;
}

bool copyRange(JsonVariantConst v, double min, double max, JsonObject out) {
  if (!v.is<JsonObjectConst>()) return false;
  JsonVariantConst lo = v["min"], hi = v["max"];
  if (!numberIn(lo, min, max) || !numberIn(hi, min, max) || lo.as<double>() > hi.as<double>()) return false;
  out["min"] = lo;
  out["max"] = hi;
  return true;
}

bool isPrimitive(JsonVariantConst v) { return isNumber(v) || v.is<bool>() || v.is<const char *>(); }

} // namespace

// ---- Preferences (docs/13 §13.2) ---------------------------------------------

void defaultPreferences(JsonObject out) {
  out["quiet_hours"] = nullptr;
  out["max_brightness"] = nullptr;
  out["vacation_mode"] = false;
  out["timezone"] = nullptr;
}

void loadPreferences(JsonDocument &out) {
  String stored = load(kPrefsNamespace, "garden");
  JsonDocument defaults;
  defaultPreferences(defaults.to<JsonObject>());
  if (stored.isEmpty() || deserializeJson(out, stored) != DeserializationError::Ok) out.to<JsonObject>();
  for (JsonPairConst kv : defaults.as<JsonObjectConst>())
    if (out[kv.key()].isUnbound()) out[kv.key()] = kv.value(); // fields added after it was stored
}

void applyTimezone(JsonVariantConst tz) {
  // POSIX TZ rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"; null = UTC.
  setenv("TZ", tz.is<const char *>() ? tz.as<const char *>() : "UTC0", 1);
  tzset();
}

bool mergePreferences(JsonVariantConst body, JsonObject current) {
  if (!body.is<JsonObjectConst>()) return false;

  // Validate everything before touching `current` — no partial apply on a 400.
  JsonVariantConst qh = body["quiet_hours"];
  if (!qh.isUnbound() && !qh.isNull() && !(qh.is<JsonObjectConst>() && isHhMm(qh["from"]) && isHhMm(qh["to"])))
    return false;
  JsonVariantConst mb = body["max_brightness"];
  if (!mb.isUnbound() && !mb.isNull() && !numberIn(mb, 0, 100)) return false;
  JsonVariantConst vm = body["vacation_mode"];
  if (!vm.isUnbound() && !vm.is<bool>()) return false;
  JsonVariantConst tz = body["timezone"];
  if (!tz.isUnbound() && !tz.isNull() &&
      !(tz.is<const char *>() && strlen(tz.as<const char *>()) > 2 && strlen(tz.as<const char *>()) < 64))
    return false;

  if (!qh.isUnbound()) {
    if (qh.isNull()) {
      current["quiet_hours"] = nullptr;
    } else {
      JsonObject o = current["quiet_hours"].to<JsonObject>();
      o["from"] = qh["from"];
      o["to"] = qh["to"];
    }
  }
  if (!mb.isUnbound()) current["max_brightness"] = mb;
  if (!vm.isUnbound()) current["vacation_mode"] = vm;
  if (!tz.isUnbound()) current["timezone"] = tz;
  return true;
}

// ---- Grow Profile (docs/08 §8.2) ---------------------------------------------

void defaultGrowProfile(JsonObject out) {
  out["species"] = "";
  out["watering_enabled"] = true;
  out["lighting_enabled"] = true;
  out["climate_enabled"] = true;
  out["targets"].to<JsonObject>();
  out["required_capabilities"].to<JsonArray>();
}

bool parseGrowProfile(JsonVariantConst body, JsonObject out) {
  if (!body.is<JsonObjectConst>() || !body["species"].is<const char *>()) return false;
  out["species"] = body["species"];

  for (JsonPairConst kv : body.as<JsonObjectConst>()) {
    String key = kv.key().c_str();
    if (!key.endsWith("_enabled")) continue;
    if (!kv.value().is<bool>()) return false;
    out[key] = kv.value();
  }

  // Per-Plant automation switches (§8.3) are always present: a profile
  // that leaves one out keeps that automation on, as the default does.
  for (const char *flag : {"watering_enabled", "lighting_enabled", "climate_enabled"})
    if (out[flag].isNull()) out[flag] = true;

  JsonVariantConst targets = body["targets"];
  if (!targets.is<JsonObjectConst>()) return false;
  JsonObject outTargets = out["targets"].to<JsonObject>();
  for (JsonPairConst kv : targets.as<JsonObjectConst>()) {
    const char *key = kv.key().c_str();
    JsonVariantConst v = kv.value();
    if (strcmp(key, "moisture_percent") == 0) {
      if (!copyRange(v, 0, 100, outTargets[key].to<JsonObject>())) return false;
    } else if (strcmp(key, "light_hours_per_day") == 0) {
      if (!numberIn(v, 0, 24)) return false;
      outTargets[key] = v;
    } else if (strcmp(key, "light_lux") == 0 || strcmp(key, "light_flux") == 0) {
      if (!numberIn(v, 0, INFINITY)) return false;
      outTargets[key] = v;
    } else if (isNumber(v)) { // extensible: other targets are a number or a { min, max } range
      outTargets[key] = v;
    } else if (!copyRange(v, -INFINITY, INFINITY, outTargets[key].to<JsonObject>())) {
      return false;
    }
  }

  JsonVariantConst cal = body["sensor_calibration"];
  if (!cal.isUnbound()) {
    if (!cal.is<JsonObjectConst>()) return false;
    JsonObject outCal = out["sensor_calibration"].to<JsonObject>();
    for (JsonPairConst kv : cal.as<JsonObjectConst>()) {
      JsonVariantConst offset = kv.value()["offset"];
      if (!isNumber(offset)) return false;
      outCal[kv.key()]["offset"] = offset;
    }
  }

  JsonVariantConst req = body["required_capabilities"];
  if (!req.is<JsonArrayConst>()) return false;
  JsonArray outReq = out["required_capabilities"].to<JsonArray>();
  for (JsonVariantConst flag : req.as<JsonArrayConst>()) {
    if (!flag.is<const char *>() || strlen(flag.as<const char *>()) == 0) return false;
    outReq.add(flag);
  }
  return true;
}

// ---- Settings (docs/07 §7.3) ---------------------------------------------------

bool parseSettings(JsonVariantConst body, JsonObject out) {
  JsonVariantConst settings = body["settings"];
  if (!settings.is<JsonObjectConst>()) return false;
  for (JsonPairConst kv : settings.as<JsonObjectConst>()) {
    JsonVariantConst v = kv.value();
    const char *key = kv.key().c_str();
    if (strcmp(key, "mount_height_mm") == 0) {
      if (!v.is<uint32_t>() || v.is<bool>()) return false;
    } else if (strcmp(key, "moisture_dry_raw") == 0 || strcmp(key, "moisture_wet_raw") == 0) {
      if (!v.is<uint16_t>() || v.is<bool>()) return false; // OGR Raw16
    } else if (strcmp(key, kLuxCurveKey) == 0) {
      std::vector<CurvePoint> pts;
      if (!readCurve(v, pts)) return false;
      JsonArray arr = out[kLuxCurveKey].to<JsonArray>();
      for (const auto &p : pts) {
        JsonObject o = arr.add<JsonObject>();
        o["command"] = p.command;
        o["lux"] = p.physical;
      }
      continue; // stored sorted by command
    } else if (!isPrimitive(v)) {
      return false;
    }
    out[kv.key()] = v;
  }
  // Equal points would divide by zero in the % mapping.
  JsonVariantConst dry = settings["moisture_dry_raw"], wet = settings["moisture_wet_raw"];
  if (!dry.isUnbound() && !wet.isUnbound() && dry.as<uint32_t>() == wet.as<uint32_t>()) return false;
  return true;
}

// ---- Light curve (docs/07 §7.3 dimmer_lux_curve) --------------------------------

bool readCurve(JsonVariantConst v, std::vector<CurvePoint> &out) {
  out.clear();
  if (!v.is<JsonArrayConst>()) return false;
  JsonArrayConst arr = v.as<JsonArrayConst>();
  if (arr.size() > kMaxCurvePoints) return false;
  for (JsonVariantConst p : arr) {
    JsonVariantConst c = p["command"], l = p["lux"];
    if (!numberIn(c, 0, 100) || !numberIn(l, 0, INFINITY)) return false;
    out.push_back({c.as<float>(), l.as<float>()});
  }
  std::sort(out.begin(), out.end(), [](const CurvePoint &a, const CurvePoint &b) { return a.command < b.command; });
  // Lux must rise strictly with PWM, or lux -> PWM wouldn't be a function.
  for (size_t i = 1; i < out.size(); ++i)
    if (out[i].command == out[i - 1].command || out[i].physical <= out[i - 1].physical) return false;
  return true;
}

namespace {
float lerp(float x, float x0, float x1, float y0, float y1) {
  return x1 == x0 ? y0 : y0 + (x - x0) * (y1 - y0) / (x1 - x0);
}
} // namespace

float curveToPhysical(const std::vector<CurvePoint> &c, float command) {
  if (command <= c.front().command) return c.front().physical;
  if (command >= c.back().command) return c.back().physical;
  for (size_t i = 1; i < c.size(); ++i)
    if (command <= c[i].command)
      return lerp(command, c[i - 1].command, c[i].command, c[i - 1].physical, c[i].physical);
  return c.back().physical;
}

float curveToCommand(const std::vector<CurvePoint> &c, float physical) {
  if (physical <= c.front().physical) return c.front().command;
  if (physical >= c.back().physical) return c.back().command;
  for (size_t i = 1; i < c.size(); ++i)
    if (physical <= c[i].physical)
      return lerp(physical, c[i - 1].physical, c[i].physical, c[i - 1].command, c[i].command);
  return c.back().command;
}

// ---- NVS --------------------------------------------------------------------------

String load(const char *ns, const char *key) {
  Preferences prefs;
  if (!prefs.begin(ns, /*readOnly=*/true)) return String(); // namespace not created yet
  String out = prefs.isKey(key) ? prefs.getString(key, "") : String();
  prefs.end();
  return out;
}

void store(const char *ns, const char *key, const String &json) {
  Preferences prefs;
  prefs.begin(ns, /*readOnly=*/false);
  prefs.putString(key, json);
  prefs.end();
}

} // namespace config
} // namespace ofl
