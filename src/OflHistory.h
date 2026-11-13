// Vitals History (open-farm-link/docs/05-vitals-history.md). Retention is
// the device's own business (§5.2): this keeps a fixed RAM ring of raw
// samples (`interval: 0`), one per Plant/System every kSamplePeriodMs, and
// only once the clock is meaningful (a record needs a real timestamp). It
// does not survive a reboot — flash wear from a write every few minutes
// isn't worth it for the basic tier.
#pragma once

#include <ArduinoJson.h>

#include "OflGarden.h"

namespace ofl {

class OflHistory {
 public:
  static constexpr uint32_t kSamplePeriodMs = 10UL * 60UL * 1000UL;
  static constexpr uint16_t kCapacity = 256;  // records, shared by every owner
  static constexpr uint8_t kMaxVitals = 6;    // readings kept per record
  static constexpr uint16_t kLimitDefault = 100;
  static constexpr uint16_t kLimitCap = 200;  // §16.5: server caps `limit`

  explicit OflHistory(OflGarden &garden) : garden_(garden) {}

  // Call every loop(); samples when the period elapses and time is valid.
  void update(bool timeValid);

  // GET .../vitals/history — oldest first: the first `limit` records at or
  // after `since` when given, else the latest `limit` (mock server's rule).
  // `owner` is a plant_id, or "" for System.
  void write(const String &owner, bool hasSince, time_t since, uint16_t limit, JsonArray out) const;

 private:
  struct Record {
    time_t timestamp;
    uint32_t ownerKey;  // hash of plant_id, 0 = System
    uint8_t count;
    uint8_t types[kMaxVitals];
    float values[kMaxVitals];
  };

  static uint32_t ownerKey(const String &owner);
  bool sample(uint8_t groupAnchor, const String &owner, time_t now);

  OflGarden &garden_;
  Record ring_[kCapacity] = {};
  uint16_t head_ = 0;  // next write position
  uint16_t size_ = 0;
  uint32_t lastSampleMs_ = 0;
  bool sampledOnce_ = false;
};

} // namespace ofl
