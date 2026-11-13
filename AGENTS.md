# AGENTS.md

Guidance for AI agents working in this repo.

## What this is

The basic (open-source) Open Farm Link Head Device firmware — see
README.md. Sibling to [ogr-main](../../core/ogr-main) (the OGR master engine this
firmware is built on, unmodified — see it for OGR-side context),
[open-farm-link](../../../specs/open-farm-link) (the protocol spec this firmware
implements a subset of), and [open-farm-link-panel](../open-farm-link-panel)
(the Control Panel this firmware is meant to talk to instead of the mock
server, once both exist on the same LAN).

## Environment

- PlatformIO CLI at `~/.platformio/penv/bin/pio` (not necessarily on
  `PATH`).
- Board connects at `/dev/cu.usbserial-0001` (may differ — check
  `ls /dev/cu.*`, update `platformio.ini`'s `upload_port`/`monitor_port`).
- `pio run` (compile-only, no hardware needed) catches most mistakes fast
  — always do this before `pio run -t upload`. Real hardware is a shared
  physical resource with actuators attached; don't flash speculatively.
- **Serial monitoring via repeated pyserial open/close (as opposed to
  `pio device monitor` held open once) produced garbled, massively
  duplicated output in this sandbox** — same timestamp repeated tens of
  thousands of times. Confirmed via HTTP (`curl .../garden` before/after,
  same `plant_id` both times, 200 both times) that this was a host-side
  buffering artifact, not a real firmware crash/reboot-loop — the device
  was healthy throughout. Don't chase this as a firmware bug; if serial
  output looks like this again, verify over HTTP first before assuming
  the device is actually in a bad state.

## Architecture

- `OflCatalog.h` — the *only* OGR-specific corner of this codebase:
  translates `ogr::ClassId`/`TypeId`/`Mode`/`Format` enums into the OFL
  standard dictionaries' string keys
  (`open-farm-link/docs/18-20-*.md`). Everything above this layer speaks
  OFL vocabulary only, per `open-farm-link/docs/01-philosophy.md` §1.7.
- `OflGarden` — owns Plant/System grouping (see below) and every JSON
  builder for the REST surface. Reads live from the bus per request
  (one `selectEndpoint`+read per endpoint) rather than caching — same
  tradeoff `OGR Main Test`'s `endpointsHtml()` already made and validated:
  cheap enough for a handful of modules on an HTTP request, safe to
  interleave with `master.update()` since both run on the single Arduino
  task/loop.
- `OflHttpServer` — hand-rolled router over `WebServer::onNotFound()`,
  since the stock (synchronous) `WebServer` only matches exact static
  paths, not `{id}`-shaped segments. Deliberately **not**
  `ESPAsyncWebServer`: this firmware wants exactly one request in flight
  at a time (`open-farm-link/docs/01-philosophy.md` §1.2), which a
  synchronous server serviced from the same `loop()` as `master.update()`
  gives for free — async would need to actively work against that, not
  with it.
- `OflMdns` — the `ofl-peer{N}.local` claim algorithm
  (`open-farm-link/docs/16-rest-api.md` §16.4), same shape as
  `open-farm-link-panel/packages/mock-server/src/mdns.ts` but against
  `ESPmDNS`/`MDNS.queryHost()` instead of `multicast-dns`. `MDNS.begin()`
  needs *some* hostname before `queryHost()` works at all, so this begins
  under a throwaway chip-ID-derived name, probes `ofl-peer1..N`, then
  `MDNS.end()` + re-`begin()`s under whichever one it claims.

### Plant/System grouping — a firmware heuristic, not a protocol rule

`open-farm-link/docs/03-plant.md` §3.3 explicitly leaves "how a Head
Device derives Plant membership" up to the implementation. This firmware:

1. An anchor module with no `PLANT_UID` yet (`OGR_NO_PLANT_UID`) gets one
   generated (`esp_random()`) and written back to it — nothing else in
   this ecosystem assigns one yet (no Control Panel "create a Plant"
   flow), so without this a fresh anchor would never become a Plant.
2. A non-anchor module with no `PLANT_UID` is grouped with the **most
   recently enumerated anchor**. OGR enumeration order is physical
   stacking order (OGR spec §3.1), so a light/valve module discovered
   right after an anchor is very likely stacked with it as one kit. A
   module enumerated before any anchor falls back to System.

Originally implemented as "group with the *most recently* discovered
anchor" — **wrong**, caught on the real 2-module rig: OGR addresses
bottom-up, and a kit's accessory can sit physically *below* its own
anchor just as easily as above it, so it can enumerate *before* the
anchor exists yet and silently fall into System. Fixed 2026-09-07 to
"nearest anchor by enumeration distance, checked both directions"
(`assignGrouping()` in `OflGarden.cpp`) — symmetric, handles either
arrangement. Also confirmed directly: module addresses aren't stable
across reboots (the anchor and light module swapped which one got 0x09
vs 0x0A between two boots of the *same* physical rig, address order
depending on boot-time race rather than a fixed physical property) — the
distance-based grouping doesn't care which one has the lower address,
only their relative enumeration position, so this doesn't matter.

This is a real simplification, not a guess dressed up as one — **revisit
once a real per-module "assign to Plant" flow exists** (presumably a
Control Panel feature, not yet designed).

## Time, and Garden/Plant name + description (2026-09-07)

`GET/PUT /garden/time` (`open-farm-link/docs/16-rest-api.md` §16.6) exists
because this board has no battery-backed RTC: `OflGarden::beginNtpSync()`
fires `configTime()` once WiFi connects (fire-and-forget, ESP-IDF's SNTP
client runs in the background), and `pollTimeSync()` (called every
`loop()`) just watches `time(nullptr)` cross a "year 2020" sanity
threshold to notice it landed — cheap, and answers exactly the question a
Scenario's `time` pseudo-field cares about ("is this clock meaningful
yet"), without depending on framework-internal SNTP status APIs that
aren't stable across ESP32 Arduino core versions. `PUT` (manual override)
always wins for the rest of the current boot session; there's no NVS
persistence of the manual value itself, since a stale replayed epoch would
be actively worse than `source: "none"` on the next boot — see the doc's
own §16.6 for why a reboot without internet just goes back to `"none"`
until re-set. The panel auto-`PUT`s its own clock once, silently, the
first time it observes `source: "none"` (`GardenOverview.tsx`) — a
zero-config convenience, not a protocol requirement.

**Toolchain gotcha:** this project's newlib doesn't provide `timegm()` —
only the locale/timezone-dependent `mktime()`, wrong for a UTC-only wire
format. `OflGarden.cpp`'s `timegmUtc()` is Howard Hinnant's
`days_from_civil` algorithm instead (proleptic Gregorian, UTC-only, no
libc dependency) — don't reach for `timegm` again without checking the
toolchain first.

`name`/`description` for both Garden and Plant are NVS-persisted, separate
namespaces per field (`ofl_names`, `ofl_desc`, `ofl_garden`) rather than
one JSON blob per entity — keeps `Preferences`' 15-char key limit trivially
satisfied (`plant_id` alone is already ~14 chars) and each field
independently readable/writable without a decode step.
`OflGarden::updatePlant()`/`updateGarden()` take `const String *` for each
field so a `PUT` with only one of `name`/`description` doesn't clobber the
other — mirrors the REST doc's "either field alone is a valid partial
update" language.

## Configuration entities (2026-11, TASK-97)

- `OflConfig` validates Preferences / Grow Profile / Settings the same way
  `open-farm-link-panel/packages/mock-server/src/validate.ts` does and
  stores the validated JSON text in NVS (`ofl_pref`, `ofl_grow`,
  `ofl_set`). Keep the two validators in step when either changes.
- **Settings keys:** the wire `module_id` is the bus address, which is not
  stable across reboots (see the grouping notes above). The NVS key is
  therefore `OflGarden::settingsKey()`: a hash of owning Plant (or
  System) + module class + `blueprint_id` + position among same-class,
  same-blueprint modules in that group (class matters: unprogrammed
  modules all report blueprint `0x00000000`, as on the test rig). Moving a module to another Plant, or two identical modules
  swapping places, loses or swaps their Settings — acceptable until a
  stable per-module identity exists in OGR.
- **Grow Profile revalidation** (`docs/14` §14.4) is computed on every
  `GET` instead of stored: missing capability -> `status: "noop"`, back to
  `"active"` once the module returns.
- **Moisture %** (TASK-104): the pot module sends raw NE555 counts (OGR
  0x40 Raw16); `collectVitals()` maps them linearly between
  `moisture_dry_raw` (0 %) and `moisture_wet_raw` (100 %) from the module's
  Settings, then applies the Grow Profile offset. Without stored points the
  firmware defaults apply (`kDefaultDryRaw`/`kDefaultWetRaw`, 11500/4500,
  taken from the pot firmware's 45–115 kHz range — refine from a real
  batch); Settings GET shows them as the module's "Default Settings"
  (docs/07 §7.2). A module already reporting `percent` is passed through.
- **Hardware Calibration** is always `{ "calibrations": [] }` — there's no
  hardware-model library on the device (import is deferred by the spec,
  §6.4).
- **Vitals History** (`OflHistory`) is a 256-record RAM ring shared by all
  owners, one raw sample per owner every 10 minutes, only after the clock
  is valid. Not persisted: writing flash every few minutes isn't worth it
  on the basic tier.
- **Idle connection timeout** (§16.3, 5 s) comes from the stock
  `WebServer` (`HTTP_MAX_DATA_WAIT = 5000`). A second connection is
  queued by lwIP rather than refused at the TCP level — left for the
  security work (TASK-103, §22.7).

## Not implemented yet (deliberately, not overlooked)

- **Security** (`docs/22`) — tracked separately; until then the device
  runs as an open dev endpoint (HTTP:80, no auth).
- **Scenarios** — the actual automation engine. Until this exists, **no
  Plant runs any automation at all** — every actuator stays exactly where
  it was last explicitly PUT. This is a safety property of what's
  *missing*, not a feature: don't "fix" it by wiring something ad hoc
  into `loop()` before the Scenario entity (generic/specialized/script,
  `open-farm-link/docs/11-scenarios.md`) is properly implemented.
- **Culture Pack** — installs Scenarios atomically (`docs/15` §15.3), so
  it waits for the Scenario engine.

## Verified on real hardware (2026-09-07)

Compiled clean first try (`pio run`: RAM 14.7%, Flash 63.2% — confirms
the resource-budget estimate from `open-farm-link-panel`'s own AGENTS.md
history was directionally right). Flashed to the connected `esp32dev`
board and exercised over HTTP against the real device's IP (not just the
mock server):

- `GET /garden` → real `garden_id` (derived from `ESP.getEfuseMac()`,
  stable across the reboot exercised below), one Plant.
- `GET /garden/plants/{id}/modules` → both real modules (a
  `plant_container` anchor and a `light` module) correctly grouped into
  one Plant, with real live sensor/actuator values.
- `PUT .../actuators/dimmer` → changed real hardware brightness, read
  back correctly, restored to its original value afterward.
- **mDNS collision, for real**: `open-farm-link-panel`'s mock server was
  still running in the background (from earlier work) and had already
  claimed `ofl-peer1.local`. The real device correctly detected this and
  claimed `ofl-peer2.local` instead — this is two independent
  implementations (this firmware's `OflMdns.cpp` and the panel repo's
  `mdns.ts`) actually negotiating over real mDNS on the same LAN, not a
  simulated/mocked test of the algorithm.

## Debugging session (2026-09-07): "modules stopped being found"

After the user fully power-cycled the whole rig, this firmware found zero
modules for several minutes straight (`[trace] scan-for-new addr=0x08
fail`, thousands of consecutive NACKs, confirmed via `master.onDebugTrace`
— not wired up in `main.cpp` originally, added specifically to debug
this). Before concluding it was a firmware bug, **isolated it by flashing
the unmodified, previously-proven `OGR Main Test` project to the same
physical device** — it *also* found nothing at first, then found both
modules after ~35-40 seconds (much slower than this same rig's earlier
sessions). Conclusion: a marginal/slow-to-establish physical connection
(connector, most likely), not a code regression in either firmware —
confirmed by reflashing this firmware afterward and it, too, found both
modules after patiently waiting (~1 minute) rather than checking too
early. **Lesson for next time a "device isn't responding" report comes
in: reach for this same isolation test (known-good reference firmware on
the same hardware) before assuming new code is at fault, and give OGR
enumeration real time (up to a minute-plus observed) before concluding
it's stuck** — a `curl` at 20-30s in produced a false "it's broken"
signal twice in this same session.

Two things fixed/added in this same session, both verified against real
hardware once the bus stabilized: the grouping heuristic fix above, and
**Plant rename** — `PUT /garden/plants/{id}` with `{"name": "..."}`,
persisted to NVS (`Preferences`, namespace `ofl_names`, keyed by
`plant_id`) via `OflGarden::renamePlant()`. Also wired up in
`open-farm-link-panel/packages/panel/src/pages/PlantDetail.tsx` (an
inline-editable name field, `.name-edit` in `styles.css`) — Advanced mode
only; Simple mode has no rename UI yet (out of scope for that mode's
"no raw config" philosophy — revisit if this turns out to be wanted
there too).

### Update — real root cause found (later session, same rig)

The "marginal/slow-to-establish physical connection" conclusion above was
**incomplete, not wrong** — after another disconnect/reconnect the same
symptom came back, this time as a *total, sustained* failure (`addr=0x08`
NACK continuously for 3+ minutes, `modules=0`, correct anchor+light
topology confirmed present) rather than just a slow one. The user traced
it to an actual hardware fault: **the linear regulator's thermal pad on
the board was shorting against a pin of the moisture sensor chip.** That
single physical defect plausibly explains every enumeration symptom seen
across both debugging sessions — intermittent/slow bus establishment,
NACKs concentrated at the bottom-of-stack address, "light blinks on power
but never enumerates" — since a short at the anchor module's own sensor
pin would corrupt or block I2C signaling for everything behind it in the
chain, with severity depending on exact contact pressure at any given
moment. **Lesson refined:** when this rig "loses" modules again, don't
stop at "it's a slow/marginal connector" — that's a symptom description,
not a cause. If isolation testing (previous section) rules out firmware,
the next step is inspecting the board itself for shorts/contact issues
near the anchor's regulator and sensor pins, not just re-seating
connectors.
