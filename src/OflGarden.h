// Bridges ogr_main::OgrMaster's module table to the OFL entity model
// (open-farm-link/docs/03, 04, 09, 12, 14, 17). Owns the Plant/System
// grouping decision (§9.2/§18.1 of the spec deliberately leave "how a Head
// Device derives this" up to the implementation) and all JSON building for
// the REST surface this firmware implements.
#pragma once

#include <ArduinoJson.h>
#include <OgrMainEsp32.h>
#include <Preferences.h>

namespace ofl {

constexpr uint8_t kMaxPlants = OGR_MAX_MODULES; // can't exceed the module count

struct PlantEntry {
  bool used = false;
  uint8_t anchorAddress = 0;
  String plantId;
  String name;
  String description;
};

// How the Head Device currently believes it knows the time —
// open-farm-link/docs/16-rest-api.md §16.6.
enum class TimeSource { None, Ntp, Manual };

class OflGarden {
 public:
  explicit OflGarden(ogr_main::OgrMaster &master);

  // Call once after master.begin(). Generates garden_id.
  void begin();

  // Call every loop(), after master.update(). Assigns plant_id to any
  // newly-discovered anchor that doesn't have one yet (writes it back to
  // the module, per OGR spec §4.1 "anchor modules MUST store and persist
  // PLANT_UID" — this firmware is what actually satisfies that, since
  // node firmware doesn't self-assign one). Also runs the Plant/System
  // grouping heuristic — see assignGrouping()'s own comment.
  void update();

  const String &gardenId() const { return gardenId_; }

  int plantCount() const;
  const PlantEntry *plantAt(int index) const;
  const PlantEntry *findPlant(const String &plantId) const;

  // Persists to NVS (keyed by plant_id, which is itself derived from the
  // anchor's own persisted PLANT_UID — so a name/description survives both
  // this device's reboots and, as long as the anchor keeps the same
  // PLANT_UID, the module being re-enumerated at a different bus address).
  // Either pointer may be null to leave that field unchanged. Returns
  // false if plantId isn't currently known.
  bool updatePlant(const String &plantId, const String *name, const String *description);

  // Garden-level name/description, NVS-persisted (namespace "ofl_garden").
  const String &gardenName() const { return gardenName_; }
  const String &gardenDescription() const { return gardenDescription_; }
  void updateGarden(const String *name, const String *description);

  // ---- Time (open-farm-link/docs/16-rest-api.md §16.6) ------------------

  // Kicks off best-effort NTP sync — call once WiFi is up. Non-blocking;
  // update() below notices once it lands.
  void beginNtpSync();
  // Call every loop() alongside update() — cheap, just checks whether an
  // in-flight NTP sync has landed yet.
  void pollTimeSync();
  void writeTimeInfo(JsonObject out);
  // Sets the device clock directly; source becomes "manual" and stays that
  // way for the rest of this boot session (see §16.6 on why a reboot with
  // no internet uplink reverts to "none"). Returns false if `iso8601`
  // doesn't parse.
  bool setTime(const String &iso8601);

  // ---- JSON builders, one per REST resource this firmware serves -------

  void writeGardenSummary(JsonObject out);
  void writePlantList(JsonArray out);
  bool writePlantSummary(const PlantEntry &plant, JsonObject out);

  // `anchorAddress` is the Plant's anchor address, or kSystemGroup for
  // System. Vitals/Actuators/Capabilities all read live from the bus (one
  // select+read per endpoint) — see the same tradeoff already accepted in
  // OGR Main Test's endpointsHtml().
  void writeVitals(uint8_t groupAnchor, JsonObject out);
  void writeActuators(uint8_t groupAnchor, JsonObject out);
  void writeCapabilities(uint8_t groupAnchor, JsonObject out);
  void writeModules(uint8_t groupAnchor, JsonObject out);

  // capability -> (address, epIndex) resolution, for Actuators PUT and
  // (later) Scenario action execution — mirrors
  // packages/mock-server/src/derive.ts's findEndpointForCapability.
  bool findEndpointForCapability(uint8_t groupAnchor, const String &capability, uint8_t &outAddress,
                                  uint8_t &outEpIndex);

  // Raw select+read/write — Modules' own escape-hatch semantics
  // (open-farm-link/docs/09-modules.md §9.3): no validation beyond what
  // OgrMaster/the bus itself already does. Used directly for Modules
  // endpoint routes, and by Actuators writes once findEndpointForCapability
  // has resolved a capability to an (address, epIndex) pair.
  bool selectAndRead(uint8_t address, uint8_t epIndex, uint32_t &outValue);
  bool selectAndWrite(uint8_t address, uint8_t epIndex, uint32_t value);

  static constexpr uint8_t kSystemGroup = 0xFF; // not a valid I2C address (7-bit) — safe sentinel

 private:
  void assignGrouping(); // see .cpp — the "which modules belong to which Plant" heuristic

  ogr_main::OgrMaster &master_;
  String gardenId_;
  PlantEntry plants_[kMaxPlants];

  // 7-bit I2C address -> group anchor (kSystemGroup for System, or 0 for
  // "not currently a known module"). Indexed by address rather than table
  // position so it stays correct across OgrMaster's own table
  // compaction on module removal. Rebuilt in assignGrouping() whenever
  // moduleCount() changes.
  uint8_t groupOfAddress_[128] = {0};
  uint8_t lastKnownModuleCount_ = 0;

  Preferences namePrefs_;    // NVS namespace "ofl_names" — plant_id -> user-set name
  Preferences descPrefs_;    // NVS namespace "ofl_desc" — plant_id -> user-set description
  Preferences gardenPrefs_;  // NVS namespace "ofl_garden" — "name"/"desc" keys

  String gardenName_ = "My Garden";
  String gardenDescription_;

  TimeSource timeSource_ = TimeSource::None;
};

} // namespace ofl
