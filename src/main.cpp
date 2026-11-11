// Open Farm Link basic (open-source) Head Device firmware — Phase 1:
// OGR bus integration (proven, via ogr-main) + a read-heavy OFL REST
// subset (Garden/Plants/System summaries, Vitals, Actuators, Modules,
// Capabilities) + mDNS self-advertisement as ofl-peer{N}.local
// (open-farm-link/docs/16-rest-api.md §16.4).
//
// Deliberately NOT yet implemented (see open-farm-link-esp32/AGENTS.md
// for the full list): Grow Profile, Settings, Hardware Calibration,
// Scenarios, Preferences, Culture Pack, Vitals History. Every Plant
// currently defaults to no automation at all — safe by construction,
// not by omission: there is no Scenario engine yet to accidentally run
// one.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <OgrMainEsp32.h>
#include <WebServer.h>
#include <WiFi.h>
#include <stdarg.h>

#include "OflGarden.h"
#include "OflHttpServer.h"
#include "OflMdns.h"
#include "wifi_secrets.h"

using namespace ogr_hal_esp32;
using namespace ogr_main;

// Same physical pinout as OGR Main Test (same test rig) — see that
// project's main.cpp for how these were determined.
constexpr int kSdaPin = 21;
constexpr int kSclPin = 22;
constexpr int kEnPin = 5;
constexpr uint16_t kHttpPort = 80;
constexpr uint32_t kWifiConnectTimeoutMs = 20000;

OgrEsp32Transport transport(Wire, kSdaPin, kSclPin, kEnPin);
OgrMaster master(transport);
ofl::OflGarden garden(master);
WebServer webServer(kHttpPort);
ofl::OflHttpServer oflServer(webServer, garden);

void dprintf(const char *fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
}

void onDiscovered(const ModuleInfo &m) {
  dprintf("[OFL] module discovered addr=0x%02X class=0x%02X epCount=%u sysFlags=0x%02X\n", m.address,
          static_cast<uint8_t>(m.classId), m.epCount, m.sysFlags);
}
void onProblem(const ModuleInfo &m) { dprintf("[OFL] module PROBLEM addr=0x%02X\n", m.address); }
void onRecovered(const ModuleInfo &m) { dprintf("[OFL] module recovered addr=0x%02X\n", m.address); }
void onRemoved(const ModuleInfo &m) { dprintf("[OFL] module removed addr=0x%02X\n", m.address); }

// Per-probe tracing — off by default in OGR Main Test's own history (too
// noisy for routine use) but turned on here temporarily to diagnose why
// enumeration is finding nothing at all. Remove once resolved.
void onTrace(const char *stage, uint8_t address, bool ok) {
  dprintf("[OFL][trace] %-16s addr=0x%02X %s\n", stage, address, ok ? "ok" : "fail");
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // avoid modem-sleep latency delaying mDNS/HTTP responses — see OGR Main Test
  WiFi.begin(kWifiSsid, kWifiPassword);
  dprintf("[OFL] connecting to Wi-Fi \"%s\"...\n", kWifiSsid);

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < kWifiConnectTimeoutMs) {
    delay(250);
    dprintf(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    dprintf("\n[OFL] Wi-Fi connect failed, will keep retrying in background\n");
    return;
  }
  dprintf("\n[OFL] Wi-Fi connected, IP=%s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(500);
  dprintf("[OFL] Open Farm Link basic firmware booting...\n");
  dprintf("[OFL] SDA=%d SCL=%d EN=%d\n", kSdaPin, kSclPin, kEnPin);

  master.onModuleDiscovered(onDiscovered);
  master.onModuleProblem(onProblem);
  master.onModuleRecovered(onRecovered);
  master.onModuleRemoved(onRemoved);
  master.onDebugTrace(onTrace);
  master.setScanForNewPeriodMs(3000); // slow down so trace lines are readable — see OGR Main Test's own history
  master.begin();
  dprintf("[OFL] OGR master.begin() done\n");

  garden.begin();
  dprintf("[OFL] garden_id=%s\n", garden.gardenId().c_str());

  connectWifi();
  garden.beginNtpSync(); // best-effort; garden.pollTimeSync() in loop() notices once it lands

  String claimed = ofl::mdnsClaimHostname(garden.gardenId(), kHttpPort);
  if (claimed.isEmpty()) {
    dprintf("[OFL] mDNS claim failed — REST API still reachable by IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    dprintf("[OFL] claimed hostname: http://%s.local/\n", claimed.c_str());
  }

  oflServer.begin();
  dprintf("[OFL] REST API listening on port %u\n", kHttpPort);
}

void loop() {
  master.update(millis());
  garden.update();
  garden.pollTimeSync();
  webServer.handleClient();

  static uint32_t lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 5000) {
    lastHeartbeat = millis();
    dprintf("[OFL] heartbeat t=%lums modules=%u plants=%d\n", static_cast<unsigned long>(millis()),
            master.moduleCount(), garden.plantCount());
  }
}
