# sensor-node

An Arduino library for ESP32-based sensor nodes that report to
[larsi.org/sensors/log](https://larsi.org/sensors/sensor-node.php).
Handles Wi-Fi provisioning and posting readings; bring your own
sensors.

If the node can't connect to any of its known networks (nothing saved
yet, or none in range), it opens an access point with a captive setup
portal: pick a network from a live scan, enter the password, and set a
device name, device id (0-15), write key, and log frequency (1, 2, 3,
5, 10, 15, 20, 30, or 60 minutes -- default 3). It saves the settings
to flash (NVS) and reboots, then connects normally on every later
boot.

Up to `SensorNodeConfig::kMaxNetworks` (3) networks are remembered,
most-recently-added first -- a node that moves between a handful of
locations (e.g. two homes) reconnects immediately without
reprovisioning every time it moves back. `begin()` tries each in turn;
only once none of them connect does the portal come up, pre-filled
with the existing device name/id/write key so moving to a new location
only means adding one new network.

The device id is what makes a single write key usable for more than
one physical node without their channel numbers colliding -- each
device id gets its own block of 16 channels (see "Data Channel" in the
[wire protocol docs](https://larsi.org/sensors/sensor-node.php)).
Different sensor nodes attached to the same location/key get different
device ids; what sensors are actually wired to a given node is up to
each sketch.

## Install

Clone (or symlink) this repo into your Arduino `libraries` folder:

```bash
git clone https://github.com/larsi-org/sensor-node.git ~/Arduino/libraries/sensor-node
```

Restart the Arduino IDE and `#include <SensorNode.h>` becomes
available. No extra dependencies -- everything used (`WiFi`,
`WebServer`, `DNSServer`, `Preferences`, `WiFiClientSecure`) ships with
the ESP32 Arduino core.

## Quick start

```cpp
#include <SensorNode.h>

SensorNode node;
const std::vector<SensorNodeChannel> kChannels = {{0, "BME280", "Temperature", "C"}};

void setup() {
  Serial.begin(115200);
  node.begin();            // connects, or runs the setup portal if it can't
  node.provision(kChannels);  // registers the device + channels (idempotent)
}

void loop() {
  float temperatureC = readTemperature();
  node.log({temperatureC});
  delay(node.config().logIntervalMinutes * 60UL * 1000);  // set via the portal
}
```

See `examples/BasicNode` for a fuller version, including a pin you can
hold at boot to reach the setup portal on demand, or `examples/
BME280Node` for a real sensor (temperature, dew point, humidity,
pressure) -- needs the separate "SparkFun BME280" library from the
Library Manager.

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
- `bool provision(const std::vector<SensorNodeChannel> &channels)` --
  registers this device and its channels with the server. Each
  `SensorNodeChannel` is `{id, sensor, property, unit}` (e.g.
  `{0, "BME280", "Temperature", "C"}`), fixed by what's wired to the sketch.
  Idempotent server-side -- only creates rows that don't exist yet, so
  it's safe to call every boot; call it once after `begin()`, before
  the first `log()`.
- `bool log(const std::vector<float> &values, int decimalPlaces = 2)`
  -- posts one reading per channel, starting at this node's configured
  device id. A `NAN` entry is left out of the request entirely, which
  the log endpoint treats as "skip this channel" rather than logging a zero.
  Returns whether the server confirmed the data was logged.
- `const SensorNodeConfig &config() const` -- read-only access to the
  loaded settings (`ssids`/`passwords` arrays, `deviceName`,
  `deviceId`, `writeKey`, `logIntervalMinutes`). Sketches read
  `logIntervalMinutes` themselves to compute their own `loop()` delay
  -- this library doesn't call `log()` on a timer itself.

## Notes

- The server is hardcoded to `larsi.org` -- this library is for nodes
  reporting there, not a generic multi-backend client.
- TLS is verified against the specific root CA `larsi.org`'s
  certificate currently chains to (GoDaddy), pinned in
  `src/SensorNode.cpp`. If the site ever switches certificate
  providers, logging will start failing with a TLS handshake error
  until that constant is updated to match.
- The setup portal's access point is open (no password) and only runs
  while unconfigured/disconnected -- get a write key first (see the
  [wire protocol docs](https://larsi.org/sensors/sensor-node.php)).
- The write key must match `larsi.org`'s own generator (`id.php`'s
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
