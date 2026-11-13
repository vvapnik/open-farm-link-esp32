#include "OflHistory.h"

#include "OflCatalog.h"

namespace ofl {

uint32_t OflHistory::ownerKey(const String &owner) {
  if (owner.isEmpty()) return 0;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < owner.length(); ++i) h = (h ^ static_cast<uint8_t>(owner[i])) * 16777619u;
  return h ? h : 1;  // 0 is reserved for System
}

void OflHistory::update(bool timeValid) {
  if (!timeValid) return;
  uint32_t nowMs = millis();
  if (sampledOnce_ && nowMs - lastSampleMs_ < kSamplePeriodMs) return;
  // Retry soon instead of waiting a full period when nothing was recorded —
  // right after boot the clock is often set before OGR enumeration finishes.
  if (!sampledOnce_ && nowMs - lastSampleMs_ < 5000) return;
  lastSampleMs_ = nowMs;

  time_t now = time(nullptr);
  bool any = false;
  for (int i = 0; i < garden_.plantCount(); ++i) {
    const PlantEntry *p = garden_.plantAt(i);
    if (p) any |= sample(p->anchorAddress, p->plantId, now);
  }
  any |= sample(OflGarden::kSystemGroup, String(), now);
  if (any) sampledOnce_ = true;
}

bool OflHistory::sample(uint8_t groupAnchor, const String &owner, time_t now) {
  std::vector<VitalReading> readings;
  garden_.collectVitals(groupAnchor, readings);
  if (readings.empty()) return false;

  Record &r = ring_[head_];
  r.timestamp = now;
  r.ownerKey = ownerKey(owner);
  r.count = 0;
  for (const auto &v : readings) {
    if (r.count >= kMaxVitals) break;
    r.types[r.count] = static_cast<uint8_t>(v.type);
    r.values[r.count] = v.value;
    ++r.count;
  }
  head_ = (head_ + 1) % kCapacity;
  if (size_ < kCapacity) ++size_;
  return true;
}

void OflHistory::write(const String &owner, bool hasSince, time_t since, uint16_t limit, JsonArray out) const {
  uint32_t key = ownerKey(owner);
  uint16_t oldest = (head_ + kCapacity - size_) % kCapacity;

  // Collect matching ring positions, oldest first.
  std::vector<uint16_t> match;
  for (uint16_t i = 0; i < size_; ++i) {
    uint16_t pos = (oldest + i) % kCapacity;
    const Record &r = ring_[pos];
    if (r.ownerKey != key) continue;
    if (hasSince && r.timestamp < since) continue;
    match.push_back(pos);
  }
  size_t first = 0;
  if (!hasSince && match.size() > limit) first = match.size() - limit;
  size_t last = min(match.size(), first + limit);

  for (size_t i = first; i < last; ++i) {
    const Record &r = ring_[match[i]];
    JsonObject rec = out.add<JsonObject>();
    if (owner.isEmpty()) rec["owner_id"] = nullptr;
    else rec["owner_id"] = owner;
    struct tm tmVal;
    gmtime_r(&r.timestamp, &tmVal);
    char buf[25];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmVal.tm_year + 1900, tmVal.tm_mon + 1,
             tmVal.tm_mday, tmVal.tm_hour, tmVal.tm_min, tmVal.tm_sec);
    rec["timestamp"] = buf;
    rec["interval"] = 0;
    JsonArray vitals = rec["vitals"].to<JsonArray>();
    for (uint8_t v = 0; v < r.count; ++v) {
      JsonObject e = vitals.add<JsonObject>();
      auto type = static_cast<ogr::TypeId>(r.types[v]);
      e["type"] = ofl_catalog::endpointType(type);
      e["value"] = r.values[v];
      e["unit"] = ofl_catalog::catalogUnit(type);
    }
  }
}

}  // namespace ofl
