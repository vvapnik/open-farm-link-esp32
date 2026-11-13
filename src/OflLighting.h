// Generic "lighting" scenario (open-farm-link/docs/11-scenarios.md §11.2),
// built into the firmware. Per Plant, while grow_profile.lighting_enabled:
// the light window opens at the end of quiet hours (Preferences
// quiet_hours.to, device local time) and closes when quiet hours begin or
// targets.light_hours_per_day after it opened, whichever is first.
//
// Edge-triggered: entering the window (incl. booting inside it, the light
// module re-appearing, or lighting being switched back on inside it) sets
// dimmer = targets.light_lux, capped by Preferences max_brightness; leaving
// it sets 0. A manual change of the light switches lighting_enabled off
// (OflHttpServer -> OflGarden::disableAutoLighting), so it isn't undone.
// Needs a light with a PWM -> lux curve (Settings dimmer_lux_curve) and a
// valid clock; without quiet hours there is no window start, so it idles.
#pragma once

#include <map>

#include "OflGarden.h"

namespace ofl {

class OflLighting {
 public:
  explicit OflLighting(OflGarden &garden) : garden_(garden) {}
  void update(bool timeValid);

 private:
  struct State {
    bool active = false;     // lighting_enabled && in window && light usable
    bool inWindow = false;
    uint8_t lightAddr = 0;   // to notice the module re-enumerating
    float appliedLux = -1;   // re-apply if the target changes inside the window
    float appliedCap = -1;
  };

  OflGarden &garden_;
  std::map<String, State> states_;
  uint32_t lastTickMs_ = 0;
};

} // namespace ofl
