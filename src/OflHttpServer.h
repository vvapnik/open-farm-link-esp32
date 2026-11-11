// The REST surface from open-farm-link/docs/16-rest-api.md — Phase 1
// subset (read-heavy + Actuators/raw-endpoint writes). WebServer only
// matches exact static paths, so this is a small hand-rolled router over
// onNotFound() rather than per-route .on() registrations — needed for
// {id}-shaped segments like /garden/plants/{id}/vitals.
#pragma once

#include <WebServer.h>

#include <vector>

#include "OflGarden.h"

namespace ofl {

class OflHttpServer {
 public:
  OflHttpServer(WebServer &server, OflGarden &garden);
  void begin();

 private:
  void handleRequest();
  void sendJson(int status, JsonDocument &doc);
  void sendError(int status, const char *error);

  bool handleGardenRoutes(const std::vector<String> &seg, const String &method);
  bool handlePlantRoutes(const std::vector<String> &seg, const String &method);
  bool handleSystemRoutes(const std::vector<String> &seg, const String &method);
  // Shared by plant/system module+actuator sub-routes — groupAnchor is the
  // Plant's anchor address, or OflGarden::kSystemGroup.
  bool handleGroupSubroutes(const std::vector<String> &rest, const String &method, uint8_t groupAnchor);

  WebServer &server_;
  OflGarden &garden_;
};

} // namespace ofl
