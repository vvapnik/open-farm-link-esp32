#include "OflMdns.h"

#include <ESPmDNS.h>

namespace ofl {

namespace {
constexpr uint32_t kProbeTimeoutMs = 800;
constexpr int kMaxAttempts = 64; // sanity bound — a real LAN won't have 64 head devices
} // namespace

String mdnsClaimHostname(const String &gardenId, uint16_t httpPort) {
  // MDNS.begin() needs *some* name to initialize the responder before
  // queryHost() can be used at all — a throwaway name derived from the
  // chip ID, unlikely enough to collide that we don't bother probing it
  // too. We rebind to the real claimed name once we find one.
  char probeName[24];
  snprintf(probeName, sizeof(probeName), "ofl-probe-%04lx", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFF));
  if (!MDNS.begin(probeName)) return String();

  String claimed;
  for (int n = 1; n <= kMaxAttempts; ++n) {
    String candidate = "ofl-peer" + String(n);
    IPAddress found = MDNS.queryHost(candidate, kProbeTimeoutMs);
    if (found == IPAddress(0, 0, 0, 0)) {
      claimed = candidate;
      break;
    }
  }
  if (claimed.isEmpty()) return String(); // every ofl-peer{1..64} taken — give up rather than loop forever

  MDNS.end();
  if (!MDNS.begin(claimed.c_str())) return String();

  MDNS.addService("ofl", "tcp", httpPort);
  MDNS.addServiceTxt("ofl", "tcp", "garden_id", gardenId.c_str());
  // Also advertise plain http so a browser/tool that only knows _http._tcp
  // (not this protocol's own _ofl._tcp) still finds the REST API.
  MDNS.addService("http", "tcp", httpPort);

  return claimed;
}

} // namespace ofl
