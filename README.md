# sensor-node

An Arduino library for ESP32-based sensor nodes that report to
[larsi.org/log.php](https://larsi.org/sensors/sensor-node.php). Handles
Wi-Fi provisioning and posting readings; bring your own sensors.

If the node can't connect to Wi-Fi (nothing saved yet, or a bad
password/out-of-range network), it opens an access point with a
captive setup portal: pick a network from a live scan, enter the
password, and set a node name, device id (0-255), and write key. It
saves the settings to flash (NVS) and reboots, then connects normally
on every later boot.

The device id is what makes a single write key usable for more than
one physical node without their channel numbers colliding -- each
device id gets its own block of 256 channels (see "Starting at a
Different Channel" in the [wire protocol docs](https://larsi.org/sensors/sensor-node.php)).
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

void setup() {
  Serial.begin(115200);
  node.begin();  // connects, or runs the setup portal if it can't
}

void loop() {
  float temperatureC = readTemperature();
  node.log({temperatureC});
  delay(60000);
}
```

See `examples/BasicNode` for a fuller version, including a pin you can
hold low at boot to force reconfiguration, or `examples/BME280Node`
for a real sensor (temperature, dew point, humidity, pressure) --
needs the separate "SparkFun BME280" library from the Library Manager.

## API

- `void begin(unsigned long connectTimeoutMs = 15000)` -- loads saved
  settings and connects. If nothing is saved, or the connection times
  out, this runs the setup portal instead and never returns; the
  device restarts once the portal is submitted.
- `void resetConfig()` -- erases saved settings. Call before `begin()`
  to force the setup portal on the next call (e.g. gate it behind a
  button held at boot).
- `bool log(const std::vector<float> &values, int decimalPlaces = 2)`
  -- posts one reading per channel, starting at this node's configured
  device id. A `NAN` entry is left out of the request entirely, which
  `log.php` treats as "skip this channel" rather than logging a zero.
  Returns whether the server confirmed the data was logged.
- `const SensorNodeConfig &config() const` -- read-only access to the
  loaded settings (`ssid`, `password`, `nodeName`, `deviceId`,
  `writeKey`).

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
  letters/digits/`-`/`_`. The portal enforces this shape both
  client-side (input `pattern`) and server-side (`isValidWriteKey()`
  in `handleSave()`), not just the length -- a wrong-shape key would
  otherwise only fail once talking to the real server, with a generic
  "Key not found".
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
