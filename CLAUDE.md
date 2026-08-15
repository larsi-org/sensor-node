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
  `begin()` tries the saved config, falls back to the portal, then
  syncs the clock (see Networking below). `log()` builds the
  `device|values` form per the wire protocol (see
  `https://larsi.org/sensors/sensor-node.php` in the main site repo)
  and writes it as a raw HTTP/1.1 request directly to a
  `WiFiClientSecure` -- not via `HTTPClient`, see Networking below.

## Networking (hard-won, 2026-08-15 debugging session)

`SensorNode.cpp` avoids two Arduino-esp32 pieces that both proved
unreliable on the SparkFun ESP32-C6 Thing Plus, across both esp32 core
3.0.7 and 3.3.11 (so not the Matter/`CONFIG_LWIP_CHECK_THREAD_SAFETY`
locking regression -- that was a real, separate bug ruled out along the
way, see `github.com/espressif/arduino-esp32` issue #10526):

- **DNS.** `WiFi.hostByName()`/hostname-based `connect()` reliably hung
  or crashed while the router (see below) was in its bad state --
  traced to `NetworkManager::hostByName()` not taking the TCPIP core
  lock lwIP requires (`arduino-esp32` issue #10526), a real bug, but
  one that in practice didn't trigger once the network was healthy.
  Re-tested after the router fix (3 clean resolves in a row) and
  switched back to real DNS: `SensorNode::resolveServerIp()` resolves
  `kServer` once via `WiFi.hostByName()` and caches the result in
  `serverIp_`, re-resolving on demand only if a resolve ever fails --
  deliberately not re-resolving every `log()` call, to keep exposure to
  that still-latent lock bug low even though it's not currently firing.
  If DNS starts failing/crashing again, that's the first thing to
  revert (hardcode `kServerIp`, pass `kServer` only for TLS SNI/Host).
- **NTP.** Raw NTP over `WiFiUDP` sent successfully (per `endPacket()`)
  but never once received a reply, even to a plain LAN target with no
  DNS involved -- confirmed the protocol itself was fine (a plain
  Python UDP client got instant replies from the same servers on the
  same network). Root cause never fully identified; worked around
  entirely by reading the clock off an HTTPS response's `Date` header
  instead (`syncTimeFromHttpDate()`), since TCP was reliable.
- **`HTTPClient`.** Even with a good TCP/TLS connection, using
  `HTTPClient` (constructing the object, `addHeader()`, etc.) between
  `connect()` succeeding and actually writing the request took long
  enough that the connection was reliably closed before a single byte
  went out. Fixed by dropping `HTTPClient` and writing a raw prebuilt
  HTTP/1.1 request straight to the `WiFiClientSecure` immediately after
  `connect()` returns (`rawHttpRequest()`) -- confirmed reliable across
  many repeated resets on both core versions.
- **The actual "first request always fails" pattern turned out to be a
  dead router**, not code: a second device on the same 2.4GHz network
  was also silently offline, and resetting the router fixed both.
  Everything above except the `HTTPClient` timing-gap fix was
  investigated *before* that reset, chasing what looked like a
  code-level bug -- worth remembering that a string of "first attempt
  after boot fails" symptoms can be the AP's fault, not the firmware's.
  Test that early if it comes up again.

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
