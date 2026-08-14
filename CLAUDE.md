# CLAUDE.md

Arduino library for ESP32 sensor nodes that report to
`larsi.org/log.php`. Repo root doubles as the Arduino library root
(`library.properties`, `src/`, `examples/`) -- installed by
cloning/symlinking into `~/Arduino/libraries/`, not via a build step.

## Architecture

- `SensorNodeConfig` (`src/SensorNodeConfig.*`) -- `ssid`/`password`/
  `nodeName`/`deviceId`/`writeKey` persisted via `Preferences` (NVS).
  Server host is not part of the config; it's a hardcoded constant in
  `SensorNode.cpp` (`kServer`) since one node only ever reports to
  larsi.org -- the "different X per node" axis here is sensors wired
  to a sketch, not backend servers.
- `SensorNodePortal` (`src/SensorNodePortal.*`) -- open AP + captive
  portal (`WebServer` + `DNSServer` catch-all) used only while
  unconfigured or disconnected. `runSensorNodeSetupPortal()` blocks
  forever and restarts the device on successful submit; it's not
  meant to run alongside normal operation.
- `SensorNode` (`src/SensorNode.*`) -- the class sketches use.
  `begin()` tries the saved config, falls back to the portal.  `log()`
  builds the `device|values` form per the wire protocol (see
  `https://larsi.org/sensors/sensor-node.php` in the main site repo)
  and POSTs over `WiFiClientSecure`.

## TLS

`SensorNode.cpp` pins the specific root CA larsi.org's cert chain
currently validates against (GoDaddy's "Go Daddy Root Certificate
Authority - G2", self-signed, valid to 2037), not a generic trust
store or `setInsecure()` -- verified by hand against
`openssl s_client -connect larsi.org:443 -showcerts` on 2026-08-13,
fetched from `https://certs.godaddy.com/repository/gdroot-g2.crt`. If
larsi.org's certificate provider ever changes, TLS POSTs start failing
and this constant needs re-pinning to match -- check the actual chain
before touching it, don't guess or hand-type a replacement.

## Conventions

Same discipline as the `larsi-org/html` repo this device reports to:
MIT license, no `Co-Authored-By` in commits, README covers usage and
API, this file covers architecture/non-obvious decisions for future
sessions.
