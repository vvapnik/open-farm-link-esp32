// Implements the Head Device hostname negotiation from
// open-farm-link/docs/16-rest-api.md §16.4: probe ofl-peer1.local,
// ofl-peer2.local, ... and claim the first one nothing else answers for.
// Same algorithm as packages/mock-server/src/mdns.ts, in C++ against
// ESPmDNS instead of multicast-dns/bonjour-service.
#pragma once

#include <Arduino.h>

namespace ofl {

// Claims a hostname and starts advertising _ofl._tcp on `httpPort` with
// `gardenId` in the TXT record. Returns the claimed hostname (without
// ".local") — empty string if MDNS.begin() itself failed.
String mdnsClaimHostname(const String &gardenId, uint16_t httpPort);

} // namespace ofl
