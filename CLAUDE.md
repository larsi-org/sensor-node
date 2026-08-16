# CLAUDE.md

Arduino library for ESP32 sensor nodes that report to
`larsi.org/sensors/log.php`. Repo root doubles as the Arduino library root
(`library.properties`, `src/`, `examples/`) -- installed by
cloning/symlinking into `~/Arduino/libraries/`, not via a build step.

## Architecture

- `SensorNodeConfig` (`src/SensorNodeConfig.*`) -- `ssids[]`/
  `passwords[]` (fixed-size arrays, `kMaxNetworks` = 3, most-recently-
  added first, NVS keys `ssid0`/`password0`/`ssid1`/...) plus
  `deviceName`/`deviceId`/`writeKey`/`logIntervalMinutes`, all
  persisted via `Preferences` (NVS). Server host isn't part of the
  config; it's a hardcoded constant in `SensorNode.cpp` (`kServer`)
  since one node only ever reports to larsi.org -- the "different X
  per node" axis here is sensors wired to a sketch, not backend
  servers.
- `SensorNodePortal` (`src/SensorNodePortal.*`) -- open AP + captive
  portal (`WebServer` + `DNSServer` catch-all) used only while none of
  the known networks connect. `runSensorNodeSetupPortal()` blocks
  forever and restarts the device on successful submit. `buildFormPage()`
  pre-fills device name/id/write key/log frequency from the existing
  config (loaded fresh each render), since the common reason the
  portal is running at all is that the node moved somewhere new --
  only the network fields need filling in. `handleSave()` starts from
  the existing config too (not a blank one) and calls
  `addOrUpdateNetwork()`, which shifts the new network to the front and
  drops the oldest once all `kMaxNetworks` slots are full, or updates
  in place if the submitted SSID matches an already-known one.
- `SensorNode` (`src/SensorNode.*`) -- the class sketches use.
  `begin()` tries each known network in turn, falls back to the portal
  if none connect, then resolves the server and syncs the clock via
  NTP. `log()` builds the `device|values` form per the wire protocol
  (see `https://larsi.org/sensors/sensor-node.php` in the main site
  repo) and writes it as a raw HTTP/1.1 request directly to a
  `WiFiClientSecure` (see Networking below for why, not `HTTPClient`).
  `provision()` posts the device name/id and a `channel_id,property,unit`
  list (`SensorNodeChannel[]`, one entry per channel, fixed per sketch)
  to `/sensors/provision.php` -- idempotent server-side (only creates
  rows that don't exist yet), so sketches call it once every boot right
  after `begin()`, before the first `log()`. Both `log()` and
  `provision()` share a `postToServer(path, body)` helper for the
  connect/write/read boilerplate.

## Networking

- **DNS** (`SensorNode::resolveServerIp()`) resolves `kServer` once via
  `WiFi.hostByName()` and caches it in `serverIp_`, re-resolving only
  on failure -- deliberately not every `log()` call, since
  `NetworkManager::hostByName()` doesn't take the TCPIP core lock lwIP
  requires (`arduino-esp32` issue #10526, still unpatched as of core
  3.3.11) and can hang or crash under load. If DNS starts
  failing/crashing again, fall back to hardcoding `kServerIp` and
  passing `kServer` to `connect()` only for TLS SNI/the HTTP Host
  header.
- **`HTTPClient` is deliberately not used.** `log()` writes a raw
  HTTP/1.1 request straight to the socket immediately after
  `connect()` instead (`rawHttpRequest()`) -- on this hardware, the
  time `HTTPClient` takes between `connect()` succeeding and actually
  writing the request (constructing the object, `addHeader()`, etc.)
  was enough for the connection to get closed before a byte went out.
  Don't reintroduce `HTTPClient` here without retesting on hardware.
- If a future round of connectivity symptoms looks like "first network
  operation after boot fails, then things half-work," check the
  router/AP before chasing firmware-side theories -- that exact
  pattern once turned out to be a dead router, not code.

## TLS

`SensorNode.cpp` pins the specific root CA larsi.org's cert chain
currently validates against (GoDaddy's "Go Daddy Root Certificate
Authority - G2", self-signed, valid to 2037), not a generic trust
store or `setInsecure()`. If larsi.org's certificate provider ever
changes, TLS connections start failing and this constant needs
re-pinning to match -- get the real chain (e.g.
`openssl s_client -connect larsi.org:443 -showcerts`) before touching
it, don't guess or hand-type a replacement.

## Conventions

Same discipline as the `larsi-org/html` repo this device reports to:
MIT license, no `Co-Authored-By` in commits, README covers usage and
API, this file covers architecture/non-obvious decisions for future
sessions.
