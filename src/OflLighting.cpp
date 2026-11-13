#include "OflLighting.h"

#include <time.h>

#include "OflConfig.h"

namespace ofl {

namespace {

int minutesOf(const char *hhmm) {
  if (!hhmm || strlen(hhmm) != 5) return -1;
  return ((hhmm[0] - '0') * 10 + (hhmm[1] - '0')) * 60 + (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
}

// Minutes since the window opened if inside it, else -1.
bool inWindow(int now, int start, int quietStart, float hours) {
  int len = static_cast<int>(hours * 60 + 0.5f);
  int untilQuiet = (quietStart - start + 1440) % 1440; // 0 when quiet hours are empty
  if (untilQuiet > 0 && untilQuiet < len) len = untilQuiet;
  if (len <= 0) return false;
  if (len >= 1440) return true;
  return (now - start + 1440) % 1440 < len;
}

} // namespace

void OflLighting::update(bool timeValid) {
  uint32_t nowMs = millis();
  if (nowMs - lastTickMs_ < 1000) return;
  lastTickMs_ = nowMs;

  JsonDocument prefs;
  config::loadPreferences(prefs);
  int start = minutesOf(prefs["quiet_hours"]["to"]);
  int quietStart = minutesOf(prefs["quiet_hours"]["from"]);
  float cap = prefs["max_brightness"].is<float>() ? prefs["max_brightness"].as<float>() : 100.0f;

  int nowMin = -1;
  if (timeValid) {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    nowMin = lt.tm_hour * 60 + lt.tm_min;
  }

  for (int i = 0; i < garden_.plantCount(); ++i) {
    const PlantEntry *p = garden_.plantAt(i);
    if (!p) continue;
    State &st = states_[p->plantId];

    JsonDocument gp;
    if (!p->growProfile.isEmpty()) deserializeJson(gp, p->growProfile);
    bool enabled = gp["lighting_enabled"] | true; // default profile has it on
    JsonVariantConst luxV = gp["targets"]["light_lux"];
    JsonVariantConst hoursV = gp["targets"]["light_hours_per_day"];

    uint8_t lightAddr = 0;
    bool usable = garden_.hasLuxCurve(p->anchorAddress, lightAddr);
    bool window = nowMin >= 0 && start >= 0 && quietStart >= 0 && hoursV.is<float>() &&
                  inWindow(nowMin, start, quietStart, hoursV.as<float>());
    bool active = enabled && usable && window && luxV.is<float>();

    JsonDocument scratch;
    if (active) {
      float lux = luxV.as<float>();
      bool reapply = !st.active || lightAddr != st.lightAddr || lux != st.appliedLux || cap != st.appliedCap;
      if (reapply &&
          garden_.setActuator(p->anchorAddress, "dimmer", lux, scratch.to<JsonObject>(), cap)) {
        st.appliedLux = lux;
        st.appliedCap = cap;
        st.lightAddr = lightAddr;
        Serial.printf("[OFL] lighting %s: on, %.0f lx (cap %.0f%%)\n", p->plantId.c_str(), lux, cap);
      } else if (reapply) {
        continue; // write failed; try again next tick
      }
    } else if (st.inWindow && !window && enabled && usable) {
      // The window just closed: lights out once. Turning lighting_enabled off
      // inside the window leaves the light alone (manual control, §8.3).
      garden_.setActuator(p->anchorAddress, "dimmer", 0, scratch.to<JsonObject>());
      Serial.printf("[OFL] lighting %s: off\n", p->plantId.c_str());
    }
    st.inWindow = window;
    st.active = active;
    if (!active) st.appliedLux = st.appliedCap = -1;
  }
}

} // namespace ofl
