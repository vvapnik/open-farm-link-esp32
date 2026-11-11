// Translates OGR's wire-level enums (OgrTypes.h) into the OFL standard
// dictionaries' string keys (open-farm-link/docs/18-20). This firmware's
// only OGR-specific corner — everything above this layer speaks OFL only,
// per open-farm-link/docs/01-philosophy.md §1.7.
#pragma once

#include <OgrTypes.h>

namespace ofl_catalog {

// docs/18-module-classes.md
inline const char *moduleClass(ogr::ClassId id) {
  switch (id) {
    case ogr::ClassId::PlantContainer: return "plant_container";
    case ogr::ClassId::Light: return "light";
    case ogr::ClassId::WaterSupply: return "water_supply";
    case ogr::ClassId::Climate: return "climate";
    default: return "custom";
  }
}

// docs/19-endpoint-types.md / docs/20-capability-catalog.md — the same
// string serves both: Endpoint Type (raw, on Modules) and Capability
// Catalog key (normalized, on Vitals/Actuators/Capabilities), since OFL's
// catalog was deliberately modeled 1:1 on this OGR enum (docs/19 §19.4
// still allows vendor-specific values beyond this list; not handled here).
inline const char *endpointType(ogr::TypeId id) {
  switch (id) {
    case ogr::TypeId::Moisture: return "moisture";
    case ogr::TypeId::Temp: return "temp";
    case ogr::TypeId::Humidity: return "humidity";
    case ogr::TypeId::Light: return "light";
    case ogr::TypeId::Weight: return "weight";
    case ogr::TypeId::Distance: return "distance";
    case ogr::TypeId::WaterLevel: return "water_level";
    case ogr::TypeId::Relay: return "relay";
    case ogr::TypeId::Dimmer: return "dimmer";
    case ogr::TypeId::WaterValve: return "valve";
    case ogr::TypeId::LightSwitch: return "light_switch";
    case ogr::TypeId::WaterPump: return "pump";
    case ogr::TypeId::Vibro: return "vibro";
    case ogr::TypeId::Camera: return "camera";
    default: return "unknown";
  }
}

// docs/20-capability-catalog.md §20.1/§20.2 — the *normalized* unit, not
// the OGR wire format (Format::Percent etc.) — kept separate from
// `format()` below on purpose, since Vitals/Actuators report in this unit
// even before real Hardware Calibration exists (Phase 2; raw value passed
// through unchanged until then, same simplification the mock server
// makes — see open-farm-link-panel/packages/mock-server/src/derive.ts).
inline const char *catalogUnit(ogr::TypeId id) {
  switch (id) {
    case ogr::TypeId::Moisture: return "percent";
    case ogr::TypeId::Temp: return "celsius";
    case ogr::TypeId::Humidity: return "percent";
    case ogr::TypeId::Light: return "lux";
    case ogr::TypeId::Weight: return "gram";
    case ogr::TypeId::Distance: return "millimeter";
    case ogr::TypeId::WaterLevel: return "percent";
    case ogr::TypeId::Relay: return "binary";
    case ogr::TypeId::Dimmer: return "lux";
    case ogr::TypeId::WaterValve: return "binary";
    case ogr::TypeId::LightSwitch: return "binary";
    case ogr::TypeId::WaterPump: return "percent";
    case ogr::TypeId::Vibro: return "percent";
    default: return "raw";
  }
}

inline bool isSensorType(ogr::TypeId id) {
  return id == ogr::TypeId::Moisture || id == ogr::TypeId::Temp || id == ogr::TypeId::Humidity ||
         id == ogr::TypeId::Light || id == ogr::TypeId::Weight || id == ogr::TypeId::Distance ||
         id == ogr::TypeId::WaterLevel;
}

inline bool isActuatorType(ogr::TypeId id) {
  return id == ogr::TypeId::Relay || id == ogr::TypeId::Dimmer || id == ogr::TypeId::WaterValve ||
         id == ogr::TypeId::LightSwitch || id == ogr::TypeId::WaterPump || id == ogr::TypeId::Vibro;
}

inline const char *mode(ogr::Mode m) {
  switch (m) {
    case ogr::Mode::Read: return "read";
    case ogr::Mode::Write: return "write";
    default: return "rw";
  }
}

inline const char *format(ogr::Format f) {
  switch (f) {
    case ogr::Format::Binary: return "binary";
    case ogr::Format::Percent: return "percent";
    default: return "raw16";
  }
}

} // namespace ofl_catalog
