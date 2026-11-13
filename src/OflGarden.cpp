#include "OflGarden.h"

#include <esp_system.h>
#include <time.h>

#include "OflCatalog.h"
#include "OflConfig.h"

using namespace ogr;
using namespace ogr_main;

namespace ofl {

OflGarden::OflGarden(OgrMaster &master) : master_(master) {}

namespace {
constexpr char kNamePrefsNamespace[] = "ofl_names";
constexpr char kDescPrefsNamespace[] = "ofl_desc";
constexpr char kGardenPrefsNamespace[] = "ofl_garden";
// Anything before this is obviously not a real synced clock (this project
// was written well after 2020) — used to tell "NTP hasn't landed yet" apart
// from "NTP landed and it's actually that time".
constexpr time_t kValidTimeThreshold = 1577836800; // 2020-01-01T00:00:00Z

// Moisture: the pot module reports the raw NE555 frequency (OGR 0x40, Raw16,
// 10 Hz units) and the Head Device maps it to 0-100 % between two points
// (OGR spec 07, OFL docs/06 §6.1), kept in the module's Settings. These
// typical points apply until the user calibrates — taken from the pot
// firmware's working range (~45-115 kHz); refine them from a real batch.
constexpr char kDryKey[] = "moisture_dry_raw";
constexpr char kWetKey[] = "moisture_wet_raw";
constexpr uint32_t kDefaultDryRaw = 11500; // in air
constexpr uint32_t kDefaultWetRaw = 4500;  // in water

float moisturePercent(uint32_t raw, uint32_t dry, uint32_t wet) {
  if (dry == wet) return 0;
  float pct = (static_cast<float>(dry) - raw) * 100.0f / (static_cast<float>(dry) - wet);
  return constrain(pct, 0.0f, 100.0f);
}

// UTC calendar date -> Unix epoch seconds. Not using libc's `timegm()` —
// this toolchain's newlib doesn't provide it (only the (locale-dependent)
// `mktime()`, which isn't what we want for a UTC-only wire format).
// Howard Hinnant's days_from_civil algorithm, proleptic Gregorian, valid
// for any year newlib's `struct tm` can represent.
time_t timegmUtc(const struct tm &tmVal) {
  int y = tmVal.tm_year + 1900;
  int m = tmVal.tm_mon + 1;
  int d = tmVal.tm_mday;
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = static_cast<unsigned>(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097 + static_cast<long>(doe) - 719468;
  return static_cast<time_t>(days) * 86400 + tmVal.tm_hour * 3600 + tmVal.tm_min * 60 + tmVal.tm_sec;
}
} // namespace

void OflGarden::begin() {
  // Stable across reboots (unlike a random value) — derived from the
  // chip's own factory-programmed MAC, so garden_id doesn't change just
  // because Plant/System module reconfiguration shuffled ofl-peer{N}.
  uint64_t mac = ESP.getEfuseMac();
  char buf[24];
  snprintf(buf, sizeof(buf), "garden-%012llx", static_cast<unsigned long long>(mac));
  gardenId_ = buf;

  gardenPrefs_.begin(kGardenPrefsNamespace, /*readOnly=*/true);
  gardenName_ = gardenPrefs_.getString("name", "My Garden");
  gardenDescription_ = gardenPrefs_.getString("desc", "");
  gardenPrefs_.end();
}

void OflGarden::update() {
  uint8_t count = master_.moduleCount();
  if (count != lastKnownModuleCount_) {
    assignGrouping();
    lastKnownModuleCount_ = count;
  }
}

// Decides which Plant (or System) each currently-known module belongs to.
// Two things happen here, neither of which is an OFL protocol requirement
// (open-farm-link/docs/03-plant.md §3.3 explicitly leaves this to the Head
// Device) — both are this firmware's own, deliberately simple, choices:
//
// 1. An anchor module with no PLANT_UID yet gets one generated and written
//    back to it. Nothing else in this ecosystem currently assigns one
//    (there's no Control Panel "create a Plant" flow yet) — without this,
//    a fresh anchor would never become a Plant at all.
// 2. A non-anchor module with no PLANT_UID is grouped with whichever
//    anchor is *nearest to it in enumeration order* — not just "the most
//    recent one so far". OGR addresses modules bottom-up (spec §3.1: the
//    master enables the bottom connector first), so enumeration order is
//    physical stacking order — but a kit's accessory module can be
//    physically *below* its own anchor in the stack just as easily as
//    above it, and would then enumerate *before* that anchor. Picking
//    "nearest by enumeration distance" (checked both directions) handles
//    both arrangements; picking only "the most recent anchor so far"
//    silently dropped a below-anchor accessory into System instead —
//    caught on the real 2-module test rig, not hypothetical. A module
//    enumerated with no anchor anywhere in the current table at all falls
//    back to System. Still a heuristic, not a guarantee — revisit once a
//    real per-module "assign to Plant" flow exists.
void OflGarden::assignGrouping() {
  for (auto &p : plants_) p.used = false;
  uint8_t plantIdx = 0;

  uint8_t count = master_.moduleCount();

  // Snapshot this pass's (address, isAnchor) in enumeration order — small
  // and cheap (<= OGR_MAX_MODULES = 12), scanned a second time below.
  uint8_t addr[OGR_MAX_MODULES];
  bool anchor[OGR_MAX_MODULES];

  for (uint8_t i = 0; i < count; ++i) {
    const ModuleInfo *m = master_.moduleAt(i);
    addr[i] = m ? m->address : 0;
    anchor[i] = m && (m->sysFlags & kFlagIsAnchor) != 0;

    if (!anchor[i] || m == nullptr) continue;

    uint32_t uid = kNoPlantUid;
    master_.readPlantUid(m->address, uid);
    if (uid == kNoPlantUid) {
      uid = esp_random();
      if (uid == kNoPlantUid) uid = 1; // avoid colliding with the sentinel
      master_.writePlantUid(m->address, uid);
    }

    if (plantIdx < kMaxPlants) {
      PlantEntry &p = plants_[plantIdx++];
      p.used = true;
      p.anchorAddress = m->address;
      char buf[16];
      snprintf(buf, sizeof(buf), "plant-%08lx", static_cast<unsigned long>(uid));
      p.plantId = buf;
      namePrefs_.begin(kNamePrefsNamespace, /*readOnly=*/true);
      p.name = namePrefs_.getString(p.plantId.c_str(), p.plantId); // default: id doubles as the name until renamed
      namePrefs_.end();
      descPrefs_.begin(kDescPrefsNamespace, /*readOnly=*/true);
      p.description = descPrefs_.getString(p.plantId.c_str(), "");
      descPrefs_.end();
      p.growProfile = config::load(config::kGrowNamespace, p.plantId.c_str());
    }
    groupOfAddress_[m->address] = m->address;
  }

  for (uint8_t i = 0; i < count; ++i) {
    if (anchor[i]) continue; // already grouped with itself above

    int nearest = -1;
    int nearestDist = 1000;
    for (uint8_t j = 0; j < count; ++j) {
      if (!anchor[j]) continue;
      int dist = (i > j) ? (i - j) : (j - i);
      if (dist < nearestDist) {
        nearestDist = dist;
        nearest = j;
      }
    }
    groupOfAddress_[addr[i]] = (nearest >= 0) ? addr[nearest] : kSystemGroup;
  }
}

int OflGarden::plantCount() const {
  int n = 0;
  for (const auto &p : plants_)
    if (p.used) ++n;
  return n;
}

const PlantEntry *OflGarden::plantAt(int index) const {
  int seen = 0;
  for (const auto &p : plants_) {
    if (!p.used) continue;
    if (seen == index) return &p;
    ++seen;
  }
  return nullptr;
}

const PlantEntry *OflGarden::findPlant(const String &plantId) const {
  for (const auto &p : plants_) {
    if (p.used && p.plantId == plantId) return &p;
  }
  return nullptr;
}

void OflGarden::writeGardenSummary(JsonObject out) {
  out["garden_id"] = gardenId_;
  out["name"] = gardenName_;
  out["description"] = gardenDescription_;
  out["protocol_version"] = "0.1";
  out["type"] = "ogr";

  bool anyProblem = false;
  uint8_t count = master_.moduleCount();
  for (uint8_t i = 0; i < count; ++i) {
    const ModuleInfo *m = master_.moduleAt(i);
    if (m && m->state == ModuleState::Problem) anyProblem = true;
  }
  out["status"] = anyProblem ? "problem" : "ok";

  JsonArray plantIds = out["plant_ids"].to<JsonArray>();
  for (const auto &p : plants_)
    if (p.used) plantIds.add(p.plantId);
}

void OflGarden::writePlantList(JsonArray out) {
  for (const auto &p : plants_) {
    if (!p.used) continue;
    JsonObject o = out.add<JsonObject>();
    writePlantSummary(p, o);
  }
}

bool OflGarden::writePlantSummary(const PlantEntry &plant, JsonObject out) {
  out["plant_id"] = plant.plantId;
  out["name"] = plant.name;
  out["description"] = plant.description;
  const ModuleInfo *anchor = master_.findModule(plant.anchorAddress);
  out["status"] = (anchor && anchor->state == ModuleState::Problem) ? "problem" : "ok";
  return true;
}

void OflGarden::collectVitals(uint8_t groupAnchor, std::vector<VitalReading> &out) {
  // Substrate correction from the Plant's Grow Profile (docs/08 §8.5) — the
  // same "add offset, clamp percent" the mock server applies.
  JsonDocument profile;
  const PlantEntry *plant = plantForGroup(groupAnchor);
  if (plant && !plant->growProfile.isEmpty()) deserializeJson(profile, plant->growProfile);
  JsonObjectConst calibration = profile["sensor_calibration"];

  uint8_t count = master_.moduleCount();
  for (uint8_t i = 0; i < count; ++i) {
    const ModuleInfo *m = master_.moduleAt(i);
    if (!m || groupOfAddress_[m->address] != groupAnchor) continue;

    for (uint8_t ep = 0; ep < m->epCount; ++ep) {
      TypeId type;
      Mode mode;
      Format format;
      if (!master_.selectEndpoint(m->address, ep) || !master_.readEndpointDescriptor(m->address, type, mode, format))
        continue;
      if (!ofl_catalog::isSensorType(type)) continue;
      if (mode != Mode::Read && mode != Mode::ReadWrite) continue;

      uint32_t raw = 0;
      if (!master_.readEndpointData(m->address, raw)) continue;

      float value = static_cast<float>(raw);
      if (type == TypeId::Moisture && format == Format::Raw16) {
        JsonDocument settings;
        writeEffectiveSettings(m->address, settings.to<JsonObject>());
        value = moisturePercent(raw, settings[kDryKey] | kDefaultDryRaw, settings[kWetKey] | kDefaultWetRaw);
        // writeEffectiveSettings re-selected endpoints; nothing else is read below.
      }
      JsonVariantConst offset = calibration[ofl_catalog::endpointType(type)]["offset"];
      if (offset.is<float>()) {
        value += offset.as<float>();
        if (strcmp(ofl_catalog::catalogUnit(type), "percent") == 0) value = constrain(value, 0.0f, 100.0f);
      }
      out.push_back({type, value});
    }
  }
}

void OflGarden::writeVitals(uint8_t groupAnchor, JsonObject out) {
  std::vector<VitalReading> readings;
  collectVitals(groupAnchor, readings);
  JsonArray vitals = out["vitals"].to<JsonArray>();
  for (const auto &r : readings) {
    JsonObject entry = vitals.add<JsonObject>();
    entry["type"] = ofl_catalog::endpointType(r.type);
    entry["value"] = r.value;
    entry["unit"] = ofl_catalog::catalogUnit(r.type);
  }
}

void OflGarden::writeActuators(uint8_t groupAnchor, JsonObject out) {
  JsonArray actuators = out["actuators"].to<JsonArray>();
  uint8_t count = master_.moduleCount();
  for (uint8_t i = 0; i < count; ++i) {
    const ModuleInfo *m = master_.moduleAt(i);
    if (!m || groupOfAddress_[m->address] != groupAnchor) continue;

    for (uint8_t ep = 0; ep < m->epCount; ++ep) {
      TypeId type;
      Mode mode;
      Format format;
      if (!master_.selectEndpoint(m->address, ep) || !master_.readEndpointDescriptor(m->address, type, mode, format))
        continue;
      if (!ofl_catalog::isActuatorType(type)) continue;
      if (mode != Mode::Write && mode != Mode::ReadWrite) continue;

      uint32_t value = 0;
      if (!master_.readEndpointData(m->address, value)) continue;

      JsonObject entry = actuators.add<JsonObject>();
      entry["capability"] = ofl_catalog::endpointType(type);
      entry["value"] = value; // Hardware Calibration not implemented yet (Phase 2) — raw value passed through
      entry["unit"] = ofl_catalog::catalogUnit(type);
    }
  }
}

// Basic tier: no engine.* features (README, docs/21 §21.4). Kept as a list so
// a richer build only has to add entries here.
static const char *const kEngineCapabilities[] = {nullptr};

void OflGarden::collectCapabilities(uint8_t groupAnchor, std::vector<String> &out) {
  uint8_t count = master_.moduleCount();
  for (uint8_t i = 0; i < count; ++i) {
    const ModuleInfo *m = master_.moduleAt(i);
    if (!m || groupOfAddress_[m->address] != groupAnchor) continue;

    for (uint8_t ep = 0; ep < m->epCount; ++ep) {
      TypeId type;
      Mode mode;
      Format format;
      if (!master_.selectEndpoint(m->address, ep) || !master_.readEndpointDescriptor(m->address, type, mode, format))
        continue;
      String key = ofl_catalog::endpointType(type);
      bool already = false;
      for (const auto &k : out)
        if (k == key) already = true;
      if (!already) out.push_back(key);
    }
  }
}

static void writeEngineCapabilities(JsonArray out) {
  for (const char *const *f = kEngineCapabilities; *f; ++f) out.add(*f);
}

void OflGarden::writeCapabilities(uint8_t groupAnchor, JsonObject out) {
  std::vector<String> caps;
  collectCapabilities(groupAnchor, caps);
  JsonArray modules = out["modules"].to<JsonArray>();
  for (const auto &k : caps) modules.add(k);
  writeEngineCapabilities(out["engine"].to<JsonArray>());
}

void OflGarden::writeGardenCapabilities(JsonObject out) { writeEngineCapabilities(out["engine"].to<JsonArray>()); }

std::vector<String> OflGarden::missingCapabilities(uint8_t groupAnchor, JsonArrayConst required) {
  std::vector<String> own, system, missing;
  collectCapabilities(groupAnchor, own);
  if (groupAnchor == kSystemGroup) system = own;
  else collectCapabilities(kSystemGroup, system);
  for (const char *const *f = kEngineCapabilities; *f; ++f) {
    own.push_back(*f);
    system.push_back(*f);
  }

  for (JsonVariantConst v : required) {
    String flag = v.as<const char *>();
    const std::vector<String> *set = &own;
    String key = flag;
    if (flag.startsWith("system.")) {
      set = &system;
      key = flag.substring(7);
    }
    bool found = false;
    for (const auto &k : *set)
      if (k == key) found = true;
    if (!found) missing.push_back(flag);
  }
  return missing;
}

bool OflGarden::groupHasModule(uint8_t groupAnchor, uint8_t address) const {
  return address < 128 && master_.findModule(address) != nullptr && groupOfAddress_[address] == groupAnchor;
}

const PlantEntry *OflGarden::plantForGroup(uint8_t groupAnchor) const {
  if (groupAnchor == kSystemGroup) return nullptr;
  for (const auto &p : plants_)
    if (p.used && p.anchorAddress == groupAnchor) return &p;
  return nullptr;
}

String OflGarden::settingsKey(uint8_t address) {
  uint8_t group = groupOfAddress_[address];
  const PlantEntry *plant = plantForGroup(group);
  const ModuleInfo *self = master_.findModule(address);
  if (!self) return String();
  uint32_t bp = kNoBlueprintUid;
  master_.readBlueprintUid(address, bp);

  // Unprogrammed modules all report blueprint 0, so the class is part of the
  // key too. Position among same-class, same-blueprint modules of this group, in enumeration order.
  uint8_t ordinal = 0;
  uint8_t count = master_.moduleCount();
  for (uint8_t i = 0; i < count; ++i) {
    const ModuleInfo *m = master_.moduleAt(i);
    if (!m || m->address == address) break;
    if (groupOfAddress_[m->address] != group) continue;
    if (m->classId != self->classId) continue;
    uint32_t otherBp = kNoBlueprintUid;
    master_.readBlueprintUid(m->address, otherBp);
    if (otherBp == bp) ++ordinal;
  }

  // FNV-1a over "owner|class|blueprint|ordinal" -> 8 hex chars, under NVS's 15-char key cap.
  char src[48];
  snprintf(src, sizeof(src), "%s|%02x|%08lx|%u", plant ? plant->plantId.c_str() : "system",
           static_cast<unsigned>(self->classId), static_cast<unsigned long>(bp), ordinal);
  uint32_t h = 2166136261u;
  for (const char *c = src; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
  char key[12];
  snprintf(key, sizeof(key), "s%08lx", static_cast<unsigned long>(h));
  return key;
}

bool OflGarden::writeEffectiveSettings(uint8_t address, JsonObject out) {
  const ModuleInfo *m = master_.findModule(address);
  if (!m) return false;
  bool any = false;

  for (uint8_t ep = 0; ep < m->epCount; ++ep) {
    TypeId type;
    Mode mode;
    Format format;
    if (!master_.selectEndpoint(address, ep) || !master_.readEndpointDescriptor(address, type, mode, format)) continue;
    if (type == TypeId::Moisture && format == Format::Raw16) {
      out[kDryKey] = kDefaultDryRaw;
      out[kWetKey] = kDefaultWetRaw;
      any = true;
    }
  }

  String stored = config::load(config::kSettingsNamespace, settingsKey(address).c_str());
  JsonDocument doc;
  if (!stored.isEmpty() && deserializeJson(doc, stored) == DeserializationError::Ok) {
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) out[kv.key()] = kv.value();
    any = true;
  }
  return any;
}

void OflGarden::writeHardwareCalibration(uint8_t, JsonObject out) { out["calibrations"].to<JsonArray>(); }

void OflGarden::writeSettingsList(uint8_t groupAnchor, JsonObject out) {
  JsonArray list = out["settings"].to<JsonArray>();
  uint8_t count = master_.moduleCount();
  for (uint8_t i = 0; i < count; ++i) {
    const ModuleInfo *m = master_.moduleAt(i);
    if (!m || groupOfAddress_[m->address] != groupAnchor) continue;
    JsonDocument settings;
    if (!writeEffectiveSettings(m->address, settings.to<JsonObject>())) continue;
    JsonObject entry = list.add<JsonObject>();
    char idBuf[8];
    snprintf(idBuf, sizeof(idBuf), "0x%02X", m->address);
    entry["module_id"] = idBuf;
    entry["settings"] = settings.as<JsonObjectConst>();
  }
}

bool OflGarden::setGrowProfile(const String &plantId, const String &json) {
  for (auto &p : plants_) {
    if (!p.used || p.plantId != plantId) continue;
    config::store(config::kGrowNamespace, plantId.c_str(), json);
    p.growProfile = json;
    return true;
  }
  return false;
}

void OflGarden::writeModules(uint8_t groupAnchor, JsonObject out) {
  JsonArray modules = out["modules"].to<JsonArray>();
  uint8_t count = master_.moduleCount();
  for (uint8_t i = 0; i < count; ++i) {
    const ModuleInfo *m = master_.moduleAt(i);
    if (!m || groupOfAddress_[m->address] != groupAnchor) continue;

    JsonObject mo = modules.add<JsonObject>();
    char idBuf[8];
    snprintf(idBuf, sizeof(idBuf), "0x%02X", m->address);
    mo["id"] = idBuf;
    mo["class"] = ofl_catalog::moduleClass(m->classId);
    mo["is_anchor"] = (m->sysFlags & kFlagIsAnchor) != 0;

    uint32_t bpUid = kNoBlueprintUid;
    master_.readBlueprintUid(m->address, bpUid);
    char bpBuf[16];
    snprintf(bpBuf, sizeof(bpBuf), "0x%08lx", static_cast<unsigned long>(bpUid));
    mo["blueprint_id"] = bpBuf;

    JsonArray endpoints = mo["endpoints"].to<JsonArray>();
    for (uint8_t ep = 0; ep < m->epCount; ++ep) {
      TypeId type;
      Mode mode;
      Format format;
      if (!master_.selectEndpoint(m->address, ep) || !master_.readEndpointDescriptor(m->address, type, mode, format))
        continue;
      uint32_t value = 0;
      master_.readEndpointData(m->address, value);

      JsonObject eo = endpoints.add<JsonObject>();
      eo["index"] = ep;
      eo["type"] = ofl_catalog::endpointType(type);
      eo["mode"] = ofl_catalog::mode(mode);
      eo["format"] = ofl_catalog::format(format);
      eo["value"] = value;
    }
  }
}

bool OflGarden::findEndpointForCapability(uint8_t groupAnchor, const String &capability, uint8_t &outAddress,
                                           uint8_t &outEpIndex) {
  uint8_t count = master_.moduleCount();
  for (uint8_t i = 0; i < count; ++i) {
    const ModuleInfo *m = master_.moduleAt(i);
    if (!m || groupOfAddress_[m->address] != groupAnchor) continue;

    for (uint8_t ep = 0; ep < m->epCount; ++ep) {
      TypeId type;
      Mode mode;
      Format format;
      if (!master_.selectEndpoint(m->address, ep) || !master_.readEndpointDescriptor(m->address, type, mode, format))
        continue;
      if (capability == ofl_catalog::endpointType(type)) {
        outAddress = m->address;
        outEpIndex = ep;
        return true;
      }
    }
  }
  return false;
}

bool OflGarden::updatePlant(const String &plantId, const String *name, const String *description) {
  for (auto &p : plants_) {
    if (!p.used || p.plantId != plantId) continue;
    if (name) {
      namePrefs_.begin(kNamePrefsNamespace, /*readOnly=*/false);
      namePrefs_.putString(plantId.c_str(), *name);
      namePrefs_.end();
      p.name = *name;
    }
    if (description) {
      descPrefs_.begin(kDescPrefsNamespace, /*readOnly=*/false);
      descPrefs_.putString(plantId.c_str(), *description);
      descPrefs_.end();
      p.description = *description;
    }
    return true;
  }
  return false;
}

void OflGarden::updateGarden(const String *name, const String *description) {
  gardenPrefs_.begin(kGardenPrefsNamespace, /*readOnly=*/false);
  if (name) {
    gardenPrefs_.putString("name", *name);
    gardenName_ = *name;
  }
  if (description) {
    gardenPrefs_.putString("desc", *description);
    gardenDescription_ = *description;
  }
  gardenPrefs_.end();
}

// ---- Time (open-farm-link/docs/16-rest-api.md §16.6) ---------------------
//
// This firmware has no battery-backed RTC, so "does the device's clock mean
// anything" is genuinely two different states, not one: `configTime()` is
// fire-and-forget (ESP-IDF's SNTP client runs in the background), so rather
// than depend on framework-internal SNTP status APIs, this just watches
// `time(nullptr)` cross kValidTimeThreshold — cheap, and exactly the
// question a Scenario's `time` pseudo-field actually cares about ("is this
// value meaningful yet"), not "which mechanism produced it".

void OflGarden::beginNtpSync() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
}

void OflGarden::pollTimeSync() {
  if (timeSource_ == TimeSource::Manual) return; // manual wins for this boot session — see header comment
  if (time(nullptr) >= kValidTimeThreshold) timeSource_ = TimeSource::Ntp;
}

void OflGarden::writeTimeInfo(JsonObject out) {
  time_t now = time(nullptr);
  struct tm tmVal;
  gmtime_r(&now, &tmVal);
  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday,
            tmVal.tm_hour, tmVal.tm_min, tmVal.tm_sec);
  out["time"] = buf;
  out["source"] = timeSource_ == TimeSource::Ntp      ? "ntp"
                   : timeSource_ == TimeSource::Manual ? "manual"
                                                        : "none";
}

bool OflGarden::parseIso8601(const String &iso8601, time_t &out) {
  int y, mo, d, h, mi, se;
  if (sscanf(iso8601.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;

  struct tm tmVal = {};
  tmVal.tm_year = y - 1900;
  tmVal.tm_mon = mo - 1;
  tmVal.tm_mday = d;
  tmVal.tm_hour = h;
  tmVal.tm_min = mi;
  tmVal.tm_sec = se;
  out = timegmUtc(tmVal);
  return true;
}

bool OflGarden::setTime(const String &iso8601) {
  time_t t;
  if (!parseIso8601(iso8601, t)) return false;
  struct timeval tv;
  tv.tv_sec = t;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  timeSource_ = TimeSource::Manual;
  return true;
}

bool OflGarden::selectAndRead(uint8_t address, uint8_t epIndex, uint32_t &outValue) {
  return master_.selectEndpoint(address, epIndex) && master_.readEndpointData(address, outValue);
}

bool OflGarden::selectAndWrite(uint8_t address, uint8_t epIndex, uint32_t value) {
  return master_.selectEndpoint(address, epIndex) && master_.writeEndpointData(address, value);
}

} // namespace ofl
