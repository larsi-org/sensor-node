# CLAUDE.md

Arduino library for ESP32 sensor nodes that report to
`larsi.org/sensors/log`. Repo root doubles as the Arduino library root
(`library.properties`, `src/`, `examples/`) -- installed by
cloning/symlinking into `~/Arduino/libraries/`, not via a build step.

## Architecture

- `SensorNodeConfig` (`src/SensorNodeConfig.*`) -- `ssids[]`/
  `passwords[]` (fixed-size arrays, `kMaxNetworks` = 3, most-recently-
  added first, NVS keys `ssid0`/`password0`/`ssid1`/...) plus
  `deviceName`/`deviceLocation`/`deviceId`/`writeKey`/`logIntervalMinutes`, all
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
  NTP. `log()` sends `device` and `data` as separate form fields per the
  wire protocol (see `https://larsi.org/sensors/sensor-node.php` in the
  main site repo) and writes it as a raw HTTP/1.1 request directly to a
  `WiFiClientSecure` (see Networking below for why, not `HTTPClient`).
  `provision()` posts the device name/location/id and a `channel_id,sensor,property,unit`
  list (`SensorNodeChannel[]`, one entry per channel, fixed per sketch)
  to `/sensors/provision` -- safe to call every boot right after
  `begin()`, before the first `log()`: server-side it's a non-empty
  upsert (creates the row if missing, otherwise updates only the
  fields this call actually sent a non-empty value for), not a pure
  create-once. A blank `deviceLocation` (never set through the portal)
  never blanks out a location set by hand server-side, but a real
  device name/sensor/property/unit this call reports does overwrite
  whatever was there. Both `log()` and
  `provision()` share a `postToServer(path, body)` helper for the
  connect/write/read boilerplate. `sanitizeHostname()` derives the
  network hostname from `deviceName` (letters/digits/hyphens only,
  runs of anything else collapsed to one `-`) -- `deviceName` itself
  stays free-text and unsanitized everywhere else (`provision()`'s
  `name=`, reports), since `WiFi.setHostname()` is the only consumer
  that actually needs the restricted shape. `openPortal()` is a thin
  wrapper around `runSensorNodeSetupPortal()` -- an on-demand way to
  reach the portal (pre-filled, nothing erased) without going through
  `resetConfig()`'s full wipe first. `checkPortalButton(pin,
  wipeHoldMs)` is what both examples actually call at the top of
  `setup()`: reads `pin` (`INPUT_PULLUP`) and picks between the two
  by hold duration -- short calls `openPortal()`, past `wipeHoldMs`
  calls `resetConfig()` instead (letting the `begin()` right after
  fall into the portal blank) -- same reset-pin button doing both
  jobs, no second pin needed, same pattern as a router's reset button.
  This one method is the only thing extracted into the library so far
  from what was originally per-example boilerplate; `Serial.begin()`/
  `delay(1000)` and the `provision()` call are left in each sketch on
  purpose (baud rate and channel list are legitimately per-sketch).
  `checkPortalButton()` only runs once, at the top of `setup()` --
  holding the pin while the device is already looping does nothing;
  it has to be held through an actual reset (button press or power
  cycle) so `setup()` re-reads it. `kResetPin` in both examples is
  GPIO2, not arbitrary: on the SparkFun ESP32-C6 Thing Plus, GPIO9 is
  the chip's boot-mode strapping pin *and* the board's onboard BOOT
  button -- holding it through a reset sends the chip into USB
  download/bootloader mode instead of running any application code at
  all (confirmed 2026-08-16: silent serial output, no boot banner,
  after Lars held BOOT thinking it was this feature's trigger). Per
  Espressif's own ESP-IDF docs (verified directly -- a first search
  wrongly suggested GPIO0 was also a strapping pin on this chip, which
  is only true on classic ESP32, not C6/S3/C3), GPIO4/5/8/9/15 are the
  ESP32-C6's only strapping pins. GPIO12/13 are USB D-/D+ (the same
  USB-JTAG connection used for flashing/serial), GPIO6/7/11/18-23 are
  tied to the Thing Plus's onboard Qwiic/battery-gauge/microSD/RGB LED,
  and the Qwiic Pocket Dev Board only breaks out GPIO2/3/4/5/16/17/18/19
  to headers at all -- GPIO2 and GPIO3 are the only pins free of every
  reservation on both boards, so that's the one to keep using as new
  board variants get added. `INPUT_PULLUP` is already set in code, no
  external resistor needed.

- `SensorNodeBattery` (`src/SensorNodeBattery.*`) -- thin wrapper around
  SparkFun's `SFE_MAX1704X` for the MAX17048 fuel gauge that's on the
  board itself (not a plug-in sensor like BME280, so it lives in `src/`
  rather than an example sketch). `kSocChannel = 15` is the sitewide
  reservation (the last of a device's 16 channels) for battery state of
  charge -- see README.md. Because this is compiled unconditionally as
  part of the library (Arduino builds every `.cpp` under `src/`
  regardless of what a sketch `#include`s), the "SparkFun MAX1704x Fuel
  Gauge Arduino Library" dependency now applies to every sketch using
  `SensorNode`, including ones like `examples/BasicNode` that don't
  touch the battery channel at all -- a real departure from the
  library's "bring your own sensors, no other dependencies" stance
  everywhere else, accepted because the chip itself is on-board
  hardware rather than an optional peripheral.

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
