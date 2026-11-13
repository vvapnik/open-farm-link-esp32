// User-owned configuration entities: Preferences (open-farm-link/docs/13),
// Grow Profile (docs/08) and Settings (docs/07). Each is validated the same
// way open-farm-link-panel's mock server does (packages/mock-server/src/
// validate.ts), so the panel sees identical 400s from both, and persisted
// to NVS as the validated JSON text.
#pragma once

#include <ArduinoJson.h>
#include <Preferences.h>

namespace ofl {
namespace config {

// ---- Validation: each returns false on a shape/type error (-> 400) --------

// Partial update: every standard field present in `body` replaces the one
// in `current` (which is updated in place).
bool mergePreferences(JsonVariantConst body, JsonObject current);
void defaultPreferences(JsonObject out);

// Whole-profile replace: copies only the fields the spec defines into `out`.
bool parseGrowProfile(JsonVariantConst body, JsonObject out);
void defaultGrowProfile(JsonObject out);

// Body `{ settings: {...} }` -> the inner object, copied into `out`.
bool parseSettings(JsonVariantConst body, JsonObject out);

// ---- NVS storage ------------------------------------------------------------
// One JSON string per entry. NVS caps a key at 15 chars and a string value at
// ~4000 bytes; callers keep keys short (plant_id is 14 chars) and the
// validated documents are far below the value limit.

String load(const char *ns, const char *key);
void store(const char *ns, const char *key, const String &json);

constexpr char kPrefsNamespace[] = "ofl_pref";   // key "garden"
constexpr char kGrowNamespace[] = "ofl_grow";    // key plant_id
constexpr char kSettingsNamespace[] = "ofl_set"; // key OflGarden::settingsKey()

} // namespace config
} // namespace ofl
