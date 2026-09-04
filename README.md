# sensor-node

An Arduino library for ESP32-based sensor nodes that report to
[larsi.org/sensors/log](https://larsi.org/sensors/api.php#log-data).
Handles Wi-Fi provisioning and posting readings; bring your own
sensors.

If the node can't connect to any of its known networks (nothing saved
yet, or none in range), it opens an access point with a captive setup
portal: pick a network from a live scan, enter the password, and set a
device name (required -- defaults to `"Weather <last 6 MAC hex
digits>"` when nothing's saved yet, so a fresh device never ships
with a blank name), device id (0-15), API key, log frequency
(1, 2, 3, 5, 10, 15, 20, 30, or 60 minutes -- default 5), and report
frequency (1-12 cycles -- default 1, meaning every cycle is reported
immediately; higher values batch that many cycles into one flush, see
`log()`/`reportEveryCycles` below). It
saves the settings to flash (NVS) and reboots, then connects normally
on every later boot.

Up to `SensorNodeConfig::kMaxNetworks` (3) networks are remembered,
most-recently-added first -- a node that moves between a handful of
locations (e.g. two homes) reconnects immediately without
reprovisioning every time it moves back. `begin()` tries each in turn;
only once none of them connect does the portal come up, pre-filled
with the existing device name/id/API key (but not the
network password -- see Notes) so moving to a new location only means
adding one new network.

The device id is what makes a single API key usable for more than
one physical node without their channel numbers colliding -- each
device id gets its own block of 16 channels (see "Data Channel" in the
[API reference](https://larsi.org/sensors/api.php#data-channel)).
Different sensor nodes attached to the same location/key get different
device ids; what sensors are actually wired to a given node is up to
each sketch. If you have more than one device at a station and want to
tell them apart, that's what free-text `deviceName` is for (e.g.
`"sensor-node-basement"`) -- there's no separate location field.

Channel 15 (the last of each device's 16) is reserved sitewide for
battery state of charge, via `SensorNodeBattery` -- see the API section
below. That leaves 0-14 for whatever the sketch actually measures.

## Install

Clone (or symlink) this repo into your Arduino `libraries` folder:

```bash
git clone https://github.com/larsi-org/sensor-node.git ~/Arduino/libraries/sensor-node
```

Restart the Arduino IDE and `#include <SensorNode.h>` becomes
available. Everything used by `SensorNode` itself (`WiFi`, `WebServer`,
`DNSServer`, `Preferences`, `WiFiClientSecure`) ships with the ESP32
Arduino core; `SensorNodeBattery` additionally needs the "SparkFun
MAX1704x Fuel Gauge Arduino Library" from the Library Manager -- since
it's compiled as part of this library regardless of whether a given
sketch includes `SensorNodeBattery.h`, that dependency applies even to
sketches that don't use it (e.g. `examples/BasicNode`).

## Quick start

```cpp
#include <SensorNode.h>

SensorNode node;
const std::vector<SensorNodeChannel> kChannels = {{0, "BME280", "Temperature", "C"}};

void setup() {
  Serial.begin(115200);
  node.begin();            // connects, or runs the setup portal if it can't
  if (node.needsProvisioning()) node.provision(kChannels);  // only after a portal save
}

void loop() {
  float temperatureC = readTemperature();
  node.log(kChannels, {temperatureC});
  delay(node.config().logIntervalMinutes * 60UL * 1000);  // set via the portal
}
```

See `examples/BasicNode` for a fuller version, including a pin you can
hold at boot to reach the setup portal on demand, or `examples/
BME280Node` for a real sensor (temperature, dew point, humidity,
pressure, plus battery state of charge on channel 15) -- needs the
separate "SparkFun BME280" and "SparkFun MAX1704x Fuel Gauge Arduino
Library" libraries from the Library Manager.

## API

- `void begin(unsigned long connectTimeoutMs = 15000)` -- loads saved
  settings and connects. If nothing is saved, or the connection times
  out, this runs the setup portal instead and never returns; the
  device restarts once the portal is submitted.
- `void resetConfig()` -- erases saved settings. Call before `begin()`
  to force the setup portal on the next call (e.g. gate it behind a
  button held at boot).
- `void openPortal()` -- opens the setup portal directly, without
  erasing anything; pre-filled from whatever's already saved, same as
  `begin()`'s automatic fallback when no known network connects. Never
  returns. Useful as a lighter on-demand trigger than `resetConfig()`
  when you just want to tweak one field (e.g. the device name).
- `void checkPortalButton(uint8_t pin, unsigned long wipeHoldMs = 5000)`
  -- sets `pin` to `INPUT_PULLUP` and, if it's held low, blocks
  measuring how long: past `wipeHoldMs` calls `resetConfig()` (so the
  `begin()` call after this returns comes up blank), less than that
  calls `openPortal()` directly (pre-filled, never returns). A no-op
  if `pin` reads high. Call once at the top of `setup()`, before
  `begin()` -- see `examples/BasicNode`/`examples/BME280Node` for the
  wiring (a button to GND).
- `void checkPendingCommand()` -- checks for a one-shot test command left by an earlier
  `log()` call (see below and [larsi.org/sensors/api.php](https://larsi.org/sensors/api.php)'s
  `Command:`/`pending_command` docs), and if it's `"open_portal"` -- the only value this
  owns -- clears it and opens the setup portal (never returning). A no-op for anything
  else, including a command this build doesn't recognize at all: that's deliberately not
  cleared here, so a sketch checking `pendingCommand()`/`clearPendingCommand()` itself (see
  the Notes section below) for its own command name still gets to see it. Call once at the
  top of `setup()`, alongside `checkFirmwareVersion()`/`checkPortalButton()` and before
  `begin()` -- see `examples/BasicNode`. This is the "next boot" half of the mechanism, for
  commands that can't run until `begin()` hasn't connected yet (like `open_portal`);
  `applyPendingCommand()` (below) is what triggers the reboot that brings execution back
  here for those -- a command it can run immediately
  instead (like `scan_i2c`) never reaches this method at all.
- `bool needsProvisioning() const` -- true if the setup portal saved
  settings since the last confirmed `provision()` call. Gate
  `provision()` on this instead of calling it every boot: a normal
  reconnect-only boot has nothing new to register. Valid after
  `begin()` returns.
- `bool provision(const std::vector<SensorNodeChannel> &channels)` --
  registers this device and its channels with the server. Each
  `SensorNodeChannel` is `{id, sensor, property, unit, label = "", decimalPlaces = 1}`
  (e.g. `{0, "BME280", "Temperature", "C", "Temp", 1}`), fixed by what's
  wired to the sketch. `label`/`decimalPlaces` are never sent to the
  server -- `provision()` only reads `id`/`sensor`/`property`/`unit` --
  they're there so a sketch can pull the same per-channel short label
  and precision into a display and `log()` (see below) instead of
  keeping a second list. Server-side it's a non-empty upsert: creates the device/channel rows
  if missing, otherwise updates only the fields this call sent a
  non-empty value for (so an empty `name=` never blanks out one set by
  hand, but a real name/sensor/property/unit does overwrite what was
  there) -- so it's harmless to call outside `needsProvisioning()`
  too, just redundant once already confirmed. Also sends this device's
  full MAC address (`WiFi.macAddress()`, colons stripped) as `mac=` --
  the server has no way to see this itself (a remote HTTP request only
  carries the client's IP, never its link-layer address), and it's the
  one field that gets the non-empty-upsert treatment rather than
  always-overwrite, same reasoning as `sensor`/`property`/`unit`. Call
  it after `begin()`,
  guarded by `needsProvisioning()`, before the first `log()`. Returns
  true once the server confirms, which also clears
  `needsProvisioning()`; a false return leaves it set so the next
  boot's `begin()` gets another attempt.
- `bool log(const std::vector<SensorNodeChannel> &channels, const std::vector<float> &values)`
  -- buffers `values`, zipped positionally against `channels`: `values[0]`
  is `channels[0]`'s reading, `values[1]` is `channels[1]`'s, and so on
  -- **not** matched up by id, so reordering `channels` without
  updating every `values` list built against it would silently misfile
  a reading onto the wrong channel. Each entry uses its `channels[i]`'s
  `id` (for wire position, within this node's configured device id)
  and `decimalPlaces` (for rounding) -- `channels` is typically the
  same list passed to `provision()`. `values` can be shorter than
  `channels` to report only the first several (e.g.
  `node.log(kChannels, {ch0, ch1})` to skip channels 2+ some call). A
  `NAN` value is left out of the request entirely, which the log
  endpoint treats as "skip this channel" rather than logging a zero.
  Every call queues its reading in a fixed 64-entry RTC-memory ring
  buffer first, unconditionally -- so a call never loses data even if
  this wake doesn't also flush, or the flush fails. Only every
  `config().reportEveryCycles`-th call actually POSTs anything,
  batching everything queued since the last confirmed flush into one
  request (`data[]`/`t[]` pairs -- see
  [larsi.org/sensors/api.php#batched-reports](https://larsi.org/sensors/api.php#batched-reports)
  section); `reportEveryCycles == 1` (the default)
  flushes every call, identical to this method's behavior before
  buffering existed. A queued-but-not-this-wake call returns `true`
  (queued, nothing was supposed to send yet); a flush-wake call returns
  `true` only once the server confirms the whole batch was logged -- a
  `false` return leaves everything queued for the next flush attempt to
  retry, combined with whatever accumulates in the meantime. The ring
  only ever holds the most recent 64 unflushed readings; a longer
  outage silently evicts the oldest rather than growing or erroring.
  If a flush's response also carries a `Command: ...` line, it's
  persisted for `applyPendingCommand()`/`checkPendingCommand()` to act
  on -- `log()` itself never acts on it.
- `void applyPendingCommand()` -- acts on a command `log()` persisted on this or an
  earlier call, if it's one this method owns. `"scan_i2c"` runs immediately, right here
  (sweeps every I2C address on whatever `Wire` is already using and prints the result to
  `Serial`, e.g. `[SensorNode] I2C scan: 0x36,0x3C,0x77` or `...: none`; a bench-testing
  aid only -- never sent to the server, since whoever triggers it is physically at the
  device with a serial connection already open) and clears the flag itself. `"open_portal"`
  restarts the device instead, without clearing the flag -- the point of restarting is to
  reach `checkPendingCommand()`, at the top of the next `setup()`, which is what actually
  consumes it. Anything else -- including nothing pending, or a command this library
  doesn't recognize at all -- is left completely untouched, so a sketch can check
  `pendingCommand()`/`clearPendingCommand()` itself (see Notes) for its own command name
  without this method interfering. Call this right after `log()` in `loop()` -- see
  `examples/BasicNode`.
- `const SensorNodeConfig &config() const` -- read-only access to the
  loaded settings (`ssids`/`passwords` arrays, `deviceName`,
  `deviceId`, `writeKey`, `logIntervalMinutes`, `reportEveryCycles`). Sketches read
  `logIntervalMinutes` themselves to compute their own `loop()` delay
  -- this library doesn't call `log()` on a timer itself. `reportEveryCycles` drives
  `log()`'s own flush cadence (see above) and is also sent along to `provision()` so the
  server can size its own alerting tolerance to match.
- `String fetchConfig()` -- POSTs `key`/`device` (the same auth every other endpoint uses) to
  the configured server's `config` endpoint and returns just the response body -- empty on any
  failure (not connected, DNS, TLS connect, or a non-200 status). POST, not GET, so the write
  key never ends up in the web server's access log, same reason `log()`/`provision()` are
  POST-only. Unlike those two, this doesn't touch NVS or any persisted state -- a plain fetch
  for a sketch that needs its own server-hosted config beyond `provision()`'s fixed fields, e.g.
  `examples/DS18B20GridNode`/`examples/OneWireNode`'s per-deployment ROM-ID grid layout, served by
  `larsi-org/html`'s `sensors/config.php` (which resolves `key` to a location prefix
  server-side, the same way `sensors/log.php`/`provision.php` do). Call after `begin()` has
  connected.

### `SensorNodeBattery`

Thin wrapper around the onboard MAX17048 fuel gauge (see `#include
<SensorNodeBattery.h>`), for boards in this family that have one
(e.g. the SparkFun ESP32-C6 Thing Plus):

- `static const uint8_t kSocChannel = 15` -- the reserved channel number;
  use it in both `kChannels` (`{SensorNodeBattery::kSocChannel, "MAX17048",
  "State of Charge", "%"}`) and the matching position in `log()`'s
  vector, so the two never drift apart.
- `bool begin(TwoWire &wirePort = Wire)` -- connects to the chip and
  runs `quickStart()`. Returns false if it doesn't respond.
- `float readSOC()` -- state of charge, 0-100% (clamped in software --
  the MAX17048 chip's own estimate can overshoot past 100%, especially
  while charging). Returns `NAN` (which `log()` then skips entirely) if
  `begin()` wasn't called or failed.

## Notes

- This library doesn't sleep between `loop()` cycles yet -- `log()`'s RTC ring buffer
  (above) exists specifically to make that a drop-in later, but no sketch calls any sleep
  API today; `delay(node.config().logIntervalMinutes * 60UL * 1000)` is still what every
  example uses. See `CLAUDE.md`'s "Sleep" note for the open modem-sleep/light-sleep/
  deep-sleep tradeoff this hasn't settled on yet.
- A sketch can implement its own pending commands on top of the same mailbox
  `applyPendingCommand()`/`checkPendingCommand()` use, via `pendingCommand()`/
  `setPendingCommand()`/`clearPendingCommand()` (`SensorNodeConfig.h`, plain functions, not
  tied to a `SensorNode` instance). Both library methods only ever touch a command they
  specifically recognize and leave everything else alone, so this is the way to add a
  command needing a sketch-specific dependency (e.g. a 1-Wire scan needing the `OneWire`
  library) without forcing that dependency onto every sketch that uses `SensorNode` --
  check `pendingCommand()` for your own command name (typically right after
  `applyPendingCommand()` in `loop()`, or at the top of `setup()` for one that needs to run
  before `begin()` connects) and call `clearPendingCommand()` once you've handled it.
- The server is a portal-configured "Server URL" field (default
  `https://larsi.org/sensors/`, `SensorNodeConfig::serverUrl`), not a
  hardcoded constant -- `log()`/`provision()` build their requests
  against it directly. The scheme is accepted but ignored: this
  library always connects over TLS on port 443 regardless of what's
  typed, and there's no port-override support.
- TLS is verified against a curated 5-root CA bundle
  (`src/SensorNodeCertBundle.h`), not a single pinned cert -- covers
  larsi.org's current provider (GoDaddy) plus the ones it would
  realistically end up on if that changes (Let's Encrypt, GlobalSign,
  Sectigo, DigiCert), picked from public CA market-share data. See
  that header for the full reasoning and the removal order if flash
  pressure ever forces trimming it back down. A full public-CA-store
  bundle doesn't fit this device's flash budget -- ~55KB of cert data
  plus ~71KB of extra linked code to search/verify a bundle at all,
  versus a single pinned cert's ~1.4KB.
- The setup portal's access point is open (no password) and only runs
  while unconfigured/disconnected -- get an API key first (see
  [Add a Sensor Node](https://larsi.org/sensors/sensor-node.php)).
- The portal's Wi-Fi Password field is never pre-filled, even when
  re-opening the portal just to tweak the device name. Leave
  it blank to keep the saved password for an already-known network --
  it's only saved as typed for a genuinely new network.
- The API key must match `larsi.org`'s own generator (`id.php`'s
  `generateID(16)`): 16 characters, starting with a letter, the rest
  letters/digits/`-`/`_`. `handleSave()` trims the submitted value
  first (accidental leading/trailing whitespace from copy-paste is
  common) and only then checks it against that shape, both
  client-side (input `pattern`, which tolerates surrounding whitespace
  so a padded paste doesn't just fail to submit) and server-side
  (`isValidWriteKey()`) -- not just the length. A wrong-shape key would
  otherwise only fail once talking to the real server, with a generic
  "Key not found".
- `deviceName` is sent as-is to the provision endpoint/shown in reports, but
  the network hostname (`WiFi.setHostname()`) is a sanitized version --
  runs of anything other than letters/digits/hyphens collapse to a
  single `-`, and leading/trailing junk is dropped. `WiFi.setHostname()`
  itself doesn't validate anything (just silently truncates past 31
  chars), but routers' DHCP/mDNS handling of a raw name with spaces
  etc. can be unpredictable, so a free-text device name like
  `"Batcave BME280 #1"` becomes the hostname `Batcave-BME280-1`.
- The device clock is set via NTP (`WiFiUDP`, Cloudflare's and
  Google's public servers) before `log()`'s first HTTPS connection --
  needed for TLS certificate date validation to pass.
- `log()` writes a raw HTTP/1.1 request directly to the socket
  immediately after `connect()` rather than going through `HTTPClient`
  -- the gap between `connect()` succeeding and `HTTPClient` actually
  writing a request (constructing the object, `addHeader()`, etc.) was
  long enough in testing that the connection was reliably closed before
  a single byte went out. See `src/SensorNode.cpp` for the details.

## License

MIT, see `LICENSE`.
