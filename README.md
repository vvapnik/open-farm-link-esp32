# Open Farm Link — Basic ESP32 Head Device Firmware

The open-source, community-facing Head Device firmware implementing both
[OpenGardenRack](https://github.com/vvapnik/open-garden-rack) (the I2C bus
to sensor/actuator modules, via the existing
[`ogr-main`](../../core/ogr-main) library) and
[Open Farm Link](../../../specs/open-farm-link) (the network protocol to a Control
Panel). PlatformIO, Arduino framework, ESP32.

Deliberately the **basic** tier (see
`open-farm-link/AGENTS.md`'s naming/scope notes and
[docs/21-system-fields.md](../../../specs/open-farm-link/docs/21-system-fields.md)
§21.4): no Scenario engine, no impulse dosing, no `engine.*` advanced
capabilities. That's intentional, not a gap to rush closing — this is the
tier meant to run on a plain ESP32/C3 within tight resource limits, per
`open-farm-link/docs/01-philosophy.md` §1.6.

## Status: Phase 1, verified on real hardware

**Implemented:** OGR enumeration/health-check/orphan-recovery (all via
`ogr-main`, already hardware-proven before this project started) +
Plant/System grouping + a read-heavy OFL REST subset: `GET/PUT /garden`,
`GET /garden/plants`, `GET/PUT /garden/plants/{id}`, `GET .../vitals`,
`GET .../modules`, `GET .../actuators`, `PUT .../actuators/{capability}`,
`GET .../capabilities`, `GET/PUT .../modules/{module_id}/endpoints/{index}`,
`GET/PUT /garden/time` (`open-farm-link/docs/16-rest-api.md` §16.6 —
best-effort NTP on WiFi connect, Control Panel `PUT` as the fallback/
override for internet-less LANs), and the `/garden/system/...`
equivalents. Plant/Garden `name` and `description` are both user-editable
and NVS-persisted. mDNS self-advertisement as `ofl-peer{N}.local`
(`open-farm-link/docs/16-rest-api.md` §16.4).

**Not yet implemented** (Phase 2 — see AGENTS.md for the full list and
why each is deferred, not just "TODO"): Grow Profile, Settings, Hardware
Calibration, Preferences, Culture Pack, Scenarios (**no automation runs
at all yet** — every Plant is inert until this exists), Vitals History,
Capabilities-based add-time validation (nothing to validate against yet,
since nothing writable that needs it exists).

## Hardware

Same test rig as `ogr-main`'s own `OGR Main Test` project — SDA=21,
SCL=22, EN=5 on a plain `esp32dev` board. See `src/main.cpp`.

## Building

```sh
cp src/wifi_secrets.h.example src/wifi_secrets.h  # fill in your network; gitignored
pio run              # compile only
pio run -t upload    # flash (upload_port in platformio.ini)
pio device monitor    # serial, 115200 baud
```

`lib_deps` pulls in `ogr-main` via `symlink://../../core/ogr-main` — this repo
expects the workspace layout (`~/Lab/open-garden/code/core/ogr-main`), same convention as
`OGR Main Test`.

## License

MIT — see [LICENSE](LICENSE).

Development is assisted by [Claude Code](https://claude.com/claude-code) (Anthropic).
