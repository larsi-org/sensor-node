# CLAUDE.md

Arduino library for ESP32 sensor nodes that report to
`larsi.org/log.php`. Repo root doubles as the Arduino library root
(`library.properties`, `src/`, `examples/`) -- installed by
cloning/symlinking into `~/Arduino/libraries/`, not via a build step.

## Architecture

- `SensorNodeConfig` (`src/SensorNodeConfig.*`) -- `ssids[]`/
  `passwords[]` (fixed-size arrays, `kMaxNetworks` = 3, most-recently-
  added first, NVS keys `ssid0`/`password0`/`ssid1`/... ) plus
  `nodeName`/`deviceId`/`writeKey` persisted via `Preferences` (NVS).
  Multi-network support (2026-08-15) replaced single `ssid`/`password`
  fields -- a deliberate breaking change to the NVS layout, since a
  node that's already lost its saved network just goes through the
  portal once more, no migration needed. Server host is not part of
  the config; it's a hardcoded constant in `SensorNode.cpp` (`kServer`)
  since one node only ever reports to larsi.org -- the "different X per
  node" axis here is sensors wired to a sketch, not backend servers.
- `SensorNodePortal` (`src/SensorNodePortal.*`) -- open AP + captive
  portal (`WebServer` + `DNSServer` catch-all) used only while none of
  the known networks connect. `runSensorNodeSetupPortal()` blocks
  forever and restarts the device on successful submit; it's not meant
  to run alongside normal operation. `buildFormPage()` pre-fills
  node name/device id/write key from the existing config (loaded fresh
  each render) so the common case -- the node moved to a new location,
  every *known* network is by definition out of range -- only requires
  adding one new network. `handleSave()` starts from the existing
  config too (not a blank one) and calls `addOrUpdateNetwork()`, which
  shifts the new network to the front and drops the oldest if all
  `kMaxNetworks` slots are full, or updates in place if the submitted
  SSID matches an already-known one (password changed, most likely).
- `SensorNode` (`src/SensorNode.*`) -- the class sketches use.
  `begin()` tries the saved config, falls back to the portal, then
  resolves the server and syncs the clock via NTP (see Networking
  below). `log()` builds the `device|values` form per the wire protocol
  (see `https://larsi.org/sensors/sensor-node.php` in the main site
  repo) and writes it as a raw HTTP/1.1 request directly to a
  `WiFiClientSecure` -- not via `HTTPClient`, see Networking below.

## Networking (hard-won, 2026-08-15 debugging session)

A long debugging session first chased what looked like several
Arduino-esp32 bugs on the SparkFun ESP32-C6 Thing Plus (DNS
hangs/crashes, NTP never getting a reply, reproducing across esp32
core 3.0.7 and 3.3.11) before finding the real cause: **the router was
dead.** A second device on the same 2.4GHz network was also silently
offline; resetting the router fixed both, and DNS + NTP retested
cleanly afterward (3/3 each). Worth remembering generally: a string of
"first network operation after boot fails, then things half-work"
symptoms can be the AP's fault, not the firmware's -- test router
health early next time this comes up, before spending hours on
code-level theories.

What's still in the code as a result, and why each piece is or isn't
router-related:

- **DNS** (`SensorNode::resolveServerIp()`) resolves `kServer` once via
  `WiFi.hostByName()`, caches it in `serverIp_`, and only re-resolves
  on demand if a resolve ever fails -- not every `log()` call. That's
  deliberate, not just an optimization: `NetworkManager::hostByName()`
  genuinely doesn't take the TCPIP core lock lwIP requires
  (`arduino-esp32` issue #10526, a real, still-unpatched bug), which is
  what crashed/hung during the bad-router period. It didn't trigger
  once the network was healthy, but the bug is real -- minimizing how
  often this codepath runs is a hedge against it firing again, not
  proof it can't. If DNS starts failing/crashing again, the fallback is
  hardcoding `kServerIp` and passing `kServer` only for TLS SNI/Host
  (what this code did for about a day, revert to that pattern if
  needed).
- **NTP** (`ntpQuery()`) is real NTP again, not a workaround -- an
  earlier version of this file read the clock from an HTTPS response's
  `Date` header instead, because raw NTP over `WiFiUDP` never got a
  reply during the bad-router period (confirmed down to a same-subnet
  LAN round trip with no DNS involved). That turned out to be entirely
  the router; retested cleanly after the fix and reverted back to NTP,
  which is simpler and doesn't depend on hitting larsi.org just for a
  timestamp.
- **`HTTPClient`** is the one piece that's *not* attributed to the
  router: even with a confirmed-healthy connection (after the router
  fix, with DNS and TLS both working), using `HTTPClient`
  (constructing the object, `addHeader()`, etc.) between `connect()`
  succeeding and actually writing the request took long enough that
  the connection was reliably closed before a single byte went out.
  Fixed by dropping `HTTPClient` and writing a raw prebuilt HTTP/1.1
  request straight to the `WiFiClientSecure` immediately after
  `connect()` returns (`rawHttpRequest()`) -- confirmed reliable across
  many repeated resets, after the router fix, on both core versions.
  This one should stay regardless of network health.

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
