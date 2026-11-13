#include "OflHttpServer.h"

#include "OflConfig.h"

namespace ofl {

namespace {

std::vector<String> splitPath(const String &uri) {
  std::vector<String> out;
  int start = 0;
  while (start < static_cast<int>(uri.length())) {
    int slash = uri.indexOf('/', start);
    if (slash < 0) slash = uri.length();
    if (slash > start) out.push_back(uri.substring(start, slash));
    start = slash + 1;
  }
  return out;
}

// Module ids are rendered as "0x42" (OflGarden::writeModules) — parse back
// to the raw 7-bit address for bus access.
bool parseModuleId(const String &id, uint8_t &outAddress) {
  if (!id.startsWith("0x") && !id.startsWith("0X")) return false;
  outAddress = static_cast<uint8_t>(strtol(id.c_str(), nullptr, 16));
  return true;
}

} // namespace

OflHttpServer::OflHttpServer(WebServer &server, OflGarden &garden, OflHistory &history)
    : server_(server), garden_(garden), history_(history) {}

void OflHttpServer::begin() {
  server_.onNotFound([this]() { handleRequest(); });
  server_.begin();
}

void OflHttpServer::sendJson(int status, JsonDocument &doc) {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  String body;
  serializeJson(doc, body);
  server_.send(status, "application/json", body);
}

void OflHttpServer::sendError(int status, const char *error) {
  JsonDocument doc;
  doc["error"] = error;
  sendJson(status, doc);
}

void OflHttpServer::sendMissing(const std::vector<String> &missing) {
  JsonDocument doc;
  doc["error"] = "missing_capabilities";
  JsonArray arr = doc["missing"].to<JsonArray>();
  for (const auto &m : missing) arr.add(m);
  sendJson(422, doc);
}

bool OflHttpServer::readBody(JsonDocument &body) {
  if (deserializeJson(body, server_.arg("plain")) != DeserializationError::Ok) {
    sendError(400, "bad_request");
    return false;
  }
  return true;
}

void OflHttpServer::handleRequest() {
  // CORS preflight — the panel (a different origin) sends this before any
  // PUT with a JSON body.
  if (server_.method() == HTTP_OPTIONS) {
    server_.sendHeader("Access-Control-Allow-Origin", "*");
    server_.sendHeader("Access-Control-Allow-Methods", "GET,PUT,POST,DELETE,OPTIONS");
    server_.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server_.send(204);
    return;
  }

  std::vector<String> seg = splitPath(server_.uri());
  String method = server_.method() == HTTP_GET    ? "GET"
                   : server_.method() == HTTP_PUT  ? "PUT"
                   : server_.method() == HTTP_POST ? "POST"
                   : server_.method() == HTTP_DELETE ? "DELETE"
                                                    : "?";

  if (seg.empty() || seg[0] != "garden") {
    sendError(404, "not_found");
    return;
  }

  bool handled = seg.size() == 1  ? handleGardenRoutes(seg, method)
                 : seg[1] == "plants" ? handlePlantRoutes(seg, method)
                 : seg[1] == "system" ? handleSystemRoutes(seg, method)
                                      : handleGardenRoutes(seg, method); // /garden/time, /preferences, /capabilities
  if (!handled) sendError(404, "not_found");
}

bool OflHttpServer::handleGardenRoutes(const std::vector<String> &seg, const String &method) {
  if (seg.size() == 1) {
    if (method == "GET") {
      JsonDocument doc;
      garden_.writeGardenSummary(doc.to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    if (method == "PUT") {
      JsonDocument body;
      if (deserializeJson(body, server_.arg("plain")) != DeserializationError::Ok) {
        sendError(400, "bad_request");
        return true;
      }
      String nameVal, descVal;
      bool hasName = body["name"].is<const char *>();
      bool hasDesc = body["description"].is<const char *>();
      if (hasName) nameVal = body["name"].as<const char *>();
      if (hasDesc) descVal = body["description"].as<const char *>();
      garden_.updateGarden(hasName ? &nameVal : nullptr, hasDesc ? &descVal : nullptr);
      JsonDocument doc;
      garden_.writeGardenSummary(doc.to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    return false;
  }

  if (seg.size() == 2 && seg[1] == "capabilities" && method == "GET") {
    JsonDocument doc;
    garden_.writeGardenCapabilities(doc.to<JsonObject>());
    sendJson(200, doc);
    return true;
  }

  // docs/13 Preferences — PUT is a partial update of the standard fields.
  if (seg.size() == 2 && seg[1] == "preferences") {
    JsonDocument prefs;
    String stored = config::load(config::kPrefsNamespace, "garden");
    if (stored.isEmpty() || deserializeJson(prefs, stored) != DeserializationError::Ok)
      config::defaultPreferences(prefs.to<JsonObject>());
    if (method == "GET") {
      sendJson(200, prefs);
      return true;
    }
    if (method == "PUT") {
      JsonDocument body;
      if (!readBody(body)) return true;
      if (!config::mergePreferences(body.as<JsonVariantConst>(), prefs.as<JsonObject>())) {
        sendError(400, "bad_request");
        return true;
      }
      String json;
      serializeJson(prefs, json);
      config::store(config::kPrefsNamespace, "garden", json);
      sendJson(200, prefs);
      return true;
    }
    return false;
  }

  if (seg.size() == 2 && seg[1] == "time") {
    if (method == "GET") {
      JsonDocument doc;
      garden_.writeTimeInfo(doc.to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    if (method == "PUT") {
      JsonDocument body;
      if (deserializeJson(body, server_.arg("plain")) != DeserializationError::Ok || !body["time"].is<const char *>()) {
        sendError(400, "bad_request");
        return true;
      }
      if (!garden_.setTime(body["time"].as<const char *>())) {
        sendError(400, "bad_request");
        return true;
      }
      JsonDocument doc;
      garden_.writeTimeInfo(doc.to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    return false;
  }

  return false;
}

bool OflHttpServer::handlePlantRoutes(const std::vector<String> &seg, const String &method) {
  // /garden/plants
  if (seg.size() == 2) {
    if (method != "GET") return false;
    JsonDocument doc;
    garden_.writePlantList(doc.to<JsonArray>());
    sendJson(200, doc);
    return true;
  }

  const String &plantId = seg[2];
  const PlantEntry *plant = garden_.findPlant(plantId);
  if (!plant) {
    sendError(404, "not_found");
    return true; // handled (as an error) — stops the outer router falling through to a generic 404
  }

  // /garden/plants/{id}
  if (seg.size() == 3) {
    if (method == "GET") {
      JsonDocument doc;
      garden_.writePlantSummary(*plant, doc.to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    if (method == "PUT") {
      JsonDocument body;
      if (deserializeJson(body, server_.arg("plain")) != DeserializationError::Ok) {
        sendError(400, "bad_request");
        return true;
      }
      String nameVal, descVal;
      bool hasName = body["name"].is<const char *>();
      bool hasDesc = body["description"].is<const char *>();
      if (hasName) nameVal = body["name"].as<const char *>();
      if (hasDesc) descVal = body["description"].as<const char *>();
      garden_.updatePlant(plantId, hasName ? &nameVal : nullptr, hasDesc ? &descVal : nullptr);
      JsonDocument doc;
      garden_.writePlantSummary(*plant, doc.to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    return false;
  }

  // /garden/plants/{id}/grow-profile (docs/08) — Plant only, System has none (docs/10 §10.3).
  if (seg.size() == 4 && seg[3] == "grow-profile") {
    if (method == "GET") {
      JsonDocument doc;
      if (plant->growProfile.isEmpty() || deserializeJson(doc, plant->growProfile) != DeserializationError::Ok)
        config::defaultGrowProfile(doc.to<JsonObject>());
      // Revalidation (docs/14 §14.4): computed on read, so a profile whose
      // capability went missing reports "noop" and recovers by itself once
      // the module is back — no stored flag to keep in sync.
      bool ok = garden_.missingCapabilities(plant->anchorAddress, doc["required_capabilities"]).empty();
      doc["status"] = ok ? "active" : "noop";
      sendJson(200, doc);
      return true;
    }
    if (method == "PUT") {
      JsonDocument body;
      if (!readBody(body)) return true;
      JsonDocument profile;
      if (!config::parseGrowProfile(body.as<JsonVariantConst>(), profile.to<JsonObject>())) {
        sendError(400, "bad_request");
        return true;
      }
      std::vector<String> missing = garden_.missingCapabilities(plant->anchorAddress, profile["required_capabilities"]);
      if (!missing.empty()) {
        sendMissing(missing);
        return true;
      }
      String json;
      serializeJson(profile, json);
      garden_.setGrowProfile(plantId, json);
      profile["status"] = "active";
      sendJson(200, profile);
      return true;
    }
    return false;
  }

  std::vector<String> rest(seg.begin() + 3, seg.end());
  return handleGroupSubroutes(rest, method, plant->anchorAddress);
}

bool OflHttpServer::handleSystemRoutes(const std::vector<String> &seg, const String &method) {
  if (seg.size() == 2) {
    if (method != "GET") return false;
    JsonDocument doc;
    doc["name"] = "Main System";
    sendJson(200, doc);
    return true;
  }
  std::vector<String> rest(seg.begin() + 2, seg.end());
  return handleGroupSubroutes(rest, method, OflGarden::kSystemGroup);
}

bool OflHttpServer::handleGroupSubroutes(const std::vector<String> &rest, const String &method, uint8_t groupAnchor) {
  if (rest.empty()) return false;

  if (rest[0] == "vitals" && rest.size() == 1 && method == "GET") {
    JsonDocument doc;
    garden_.writeVitals(groupAnchor, doc.to<JsonObject>());
    sendJson(200, doc);
    return true;
  }

  // docs/05 — ?since=<ISO8601>&limit=<n>
  if (rest[0] == "vitals" && rest.size() == 2 && rest[1] == "history" && method == "GET") {
    bool hasSince = server_.hasArg("since");
    time_t since = 0;
    if (hasSince && !OflGarden::parseIso8601(server_.arg("since"), since)) {
      sendError(400, "bad_request");
      return true;
    }
    long limit = OflHistory::kLimitDefault;
    if (server_.hasArg("limit")) {
      limit = server_.arg("limit").toInt();
      if (limit < 1) {
        sendError(400, "bad_request");
        return true;
      }
    }
    if (limit > OflHistory::kLimitCap) limit = OflHistory::kLimitCap;
    const PlantEntry *plant = garden_.plantForGroup(groupAnchor);
    JsonDocument doc;
    history_.write(plant ? plant->plantId : String(), hasSince, since, static_cast<uint16_t>(limit),
                   doc.to<JsonArray>());
    sendJson(200, doc);
    return true;
  }

  if (rest[0] == "hardware-calibration" && rest.size() == 1 && method == "GET") {
    JsonDocument doc;
    garden_.writeHardwareCalibration(groupAnchor, doc.to<JsonObject>());
    sendJson(200, doc);
    return true;
  }

  // docs/07 Settings — /settings and /settings/{module_id}
  if (rest[0] == "settings") {
    if (rest.size() == 1 && method == "GET") {
      JsonDocument doc;
      garden_.writeSettingsList(groupAnchor, doc.to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    if (rest.size() != 2) return false;
    uint8_t address;
    if (!parseModuleId(rest[1], address) || !garden_.groupHasModule(groupAnchor, address)) {
      sendError(404, "not_found");
      return true;
    }
    String key = garden_.settingsKey(address);
    if (method == "GET") {
      JsonDocument doc;
      doc["module_id"] = rest[1];
      garden_.writeEffectiveSettings(address, doc["settings"].to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    if (method == "PUT") {
      JsonDocument body;
      if (!readBody(body)) return true;
      JsonDocument settings;
      if (!config::parseSettings(body.as<JsonVariantConst>(), settings.to<JsonObject>())) {
        sendError(400, "bad_request");
        return true;
      }
      String json;
      serializeJson(settings, json);
      config::store(config::kSettingsNamespace, key.c_str(), json);
      JsonDocument doc;
      doc["module_id"] = rest[1];
      garden_.writeEffectiveSettings(address, doc["settings"].to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    return false;
  }

  if (rest[0] == "capabilities" && rest.size() == 1 && method == "GET") {
    JsonDocument doc;
    garden_.writeCapabilities(groupAnchor, doc.to<JsonObject>());
    sendJson(200, doc);
    return true;
  }

  if (rest[0] == "modules" && rest.size() == 1 && method == "GET") {
    JsonDocument doc;
    garden_.writeModules(groupAnchor, doc.to<JsonObject>());
    sendJson(200, doc);
    return true;
  }

  if (rest[0] == "actuators") {
    if (rest.size() == 1 && method == "GET") {
      JsonDocument doc;
      garden_.writeActuators(groupAnchor, doc.to<JsonObject>());
      sendJson(200, doc);
      return true;
    }
    if (rest.size() == 2 && method == "PUT") {
      const String &capability = rest[1];
      uint8_t address, epIndex;
      if (!garden_.findEndpointForCapability(groupAnchor, capability, address, epIndex)) {
        sendError(404, "not_found");
        return true;
      }
      JsonDocument body;
      if (deserializeJson(body, server_.arg("plain")) != DeserializationError::Ok) {
        sendError(400, "bad_request");
        return true;
      }
      uint32_t value = body["value"].as<uint32_t>();
      if (!garden_.selectAndWrite(address, epIndex, value)) {
        sendError(404, "not_found");
        return true;
      }
      JsonDocument doc;
      doc["capability"] = capability;
      doc["value"] = value;
      sendJson(200, doc);
      return true;
    }
  }

  // /modules/{module_id}/endpoints/{index}
  if (rest[0] == "modules" && rest.size() == 4 && rest[2] == "endpoints") {
    uint8_t address;
    if (!parseModuleId(rest[1], address) || !garden_.groupHasModule(groupAnchor, address)) {
      sendError(404, "not_found");
      return true;
    }
    uint8_t epIndex = static_cast<uint8_t>(rest[3].toInt());

    if (method == "GET") {
      uint32_t value = 0;
      bool ok = garden_.selectAndRead(address, epIndex, value);
      if (!ok) {
        sendError(404, "not_found");
        return true;
      }
      JsonDocument doc;
      doc["index"] = epIndex;
      doc["value"] = value;
      sendJson(200, doc);
      return true;
    }
    if (method == "PUT") {
      JsonDocument body;
      if (deserializeJson(body, server_.arg("plain")) != DeserializationError::Ok) {
        sendError(400, "bad_request");
        return true;
      }
      uint32_t value = body["value"].as<uint32_t>();
      bool ok = garden_.selectAndWrite(address, epIndex, value);
      if (!ok) {
        sendError(404, "not_found");
        return true;
      }
      JsonDocument doc;
      doc["index"] = epIndex;
      doc["value"] = value;
      sendJson(200, doc);
      return true;
    }
  }

  return false;
}

} // namespace ofl
