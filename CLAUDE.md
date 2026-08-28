# CLAUDE.md

Arduino library for ESP32 sensor nodes that report to
`larsi.org/sensors/log`. Repo root doubles as the Arduino library root
(`library.properties`, `src/`, `examples/`) -- installed by
cloning/symlinking into `~/Arduino/libraries/`, not via a build step.

## Architecture

- `SensorNodeConfig` (`src/SensorNodeConfig.*`) -- `ssids[]`/
  `passwords[]` (fixed-size arrays, `kMaxNetworks` = 3, most-recently-
  added first, NVS keys `ssid0`/`password0`/`ssid1`/...) plus
  `deviceName`/`deviceId`/`writeKey`/`logIntervalMinutes`/`reportEveryCycles`, all
  persisted via `Preferences` (NVS). `writeKey` is displayed/documented everywhere as "API key"
  (renamed 2026-08-27 -- the same credential now also gates the read-only `sensors/config`
  endpoint, not just writes) but keeps its old field/NVS-key name deliberately, to avoid
  orphaning what's already saved on a flashed device -- see `SensorNodePortal.cpp`'s note at the
  API key form field. `reportEveryCycles` (1-12, default 1, added
  2026-08-25) drives `SensorNode::log()`'s own flush cadence (added 2026-08-26, see the RTC
  ring buffer section below) and also rides along on `provision()`'s POST body so the
  server can size `zeus_minutes` to match. `serverUrl` (added 2026-08-27, default
  `"https://larsi.org/sensors/"` via `loadSensorNodeConfig()`'s `getString()` default) is the
  base URL `log()`/`provision()` build their requests against -- `parseServerUrl()`
  (`SensorNodePortal.h`) splits it into a bare hostname and a base path guaranteed to end in
  `/`, discarding whatever scheme was typed (this library always connects over TLS on port 443
  regardless -- no port-override support). A `deviceLocation` field briefly existed here (2026-08-23 to
  2026-08-24) alongside `device.device_location` server-side -- merged
  back into `deviceName` and dropped: most stations only ever have one
  device, and where they don't (`thecoop`), what actually distinguishes
  them is role ("Solar Radiation", "ElitePro Power Meter"), not
  physical location, so a dedicated location field was the wrong shape
  more often than it fit. `saveSensorNodeConfig()` purges any stale
  `deviceLocation` NVS key left over from that window.
- `SensorNodePortal` (`src/SensorNodePortal.*`) -- open AP + captive
  portal (`WebServer` + `DNSServer` catch-all) used only while none of
  the known networks connect. `runSensorNodeSetupPortal()` blocks
  forever and restarts the device on successful submit. `buildFormPage()`
  pre-fills device name/id/API key/log frequency/report frequency/server URL from the
  existing config (loaded fresh each render), since the common reason
  the portal is running at all is that the node moved somewhere new --
  only the network fields need filling in (the password field is a
  deliberate exception, see below). Device Name is required (`required`
  client-side, rejected with 400 in `handleSave()` if empty after
  `trim()` server-side too) and defaults to `"Weather " + macSuffix()`
  when nothing's saved yet, so a fresh device is never left with a
  blank/generic name -- `macSuffix()` (last 6 hex digits of the MAC,
  shared with the AP name below) is the one piece of per-device
  uniqueness available before the user's typed anything. `handleSave()` starts from the
  existing config too (not a blank one) and calls
  `addOrUpdateNetwork()`, which shifts the new network to the front and
  drops the oldest once all `kMaxNetworks` slots are full, or updates
  in place if the submitted SSID matches an already-known one -- but
  only if the submitted password is non-empty; a blank password on an
  already-known SSID leaves the saved one alone (a blank password on a
  genuinely new SSID is still saved as-is, for a real open network).
  Without this, resubmitting the form just to change the device
  name -- the password field is never pre-filled -- would
  silently erase a working Wi-Fi password.
- `SensorNode` (`src/SensorNode.*`) -- the class sketches use.
  `begin()` tries each known network in turn, falls back to the portal
  if none connect, then resolves the server and syncs the clock via
  NTP. `log()` sends `device` and `data` as separate form fields per the
  wire protocol (see `https://larsi.org/sensors/api.php` in the
  main site repo -- split out of `sensor-node.php`, now onboarding-only, 2026-08-27) and writes it as a raw HTTP/1.1 request directly to a
  `WiFiClientSecure` (see Networking below for why, not `HTTPClient`).
  `log()` takes `(SensorNodeChannel[], float[])`, not just a plain
  `float[]`. `values` is zipped *positionally* against `channels`
  (`values[i]` is `channels[i]`'s reading) -- not matched up by id --
  so it can pick up each entry's `id` (for wire position) and
  `decimalPlaces` (for rounding) straight from the matching
  `SensorNodeChannel` instead of the caller repeating either at the
  call site. Internally `log()` builds its own `float[16]`/
  `decimalPlaces[16]` (NaN-filled, id-indexed) from the zip before
  buffering it (see the RTC ring buffer paragraph below) -- that's
  what lets a sketch skip hand-padding `NAN`s up to a gap like channel
  15 (`examples/BME280Node`: `node.log(kChannels, {ch0, ch1, ch2, ch3,
  ch15})` -- no channels 4-14 to write out by hand). A flush now always
  serializes all 16 ids per queued entry (added 2026-08-26, see below),
  not just up to the highest id used that call -- the log endpoint's
  per-entry parsing already treats a trailing empty field the same as
  a missing one, so this is wire-compatible, just a few bytes larger.
  `values` can also
  be shorter than `channels`, to report only the first several without
  slicing `channels` itself. The zip being positional (not id-keyed)
  is a real trade -- reordering `channels` without updating every
  `values` list built against it would silently misfile a reading onto
  the wrong channel, not just look wrong somewhere -- but it's the
  same trade already made for the OLED's `printReadings()` (below), so
  this doesn't introduce a new kind of fragility, just extends the
  existing one from display-only to the actual logged data. `channels`
  is typically the same list passed to `provision()`, but doesn't have
  to match exactly -- `log()` only reads it for `id`/`decimalPlaces`
  lookups.
  `SensorNodeChannel` also carries `label = ""` and `decimalPlaces = 1`
  (most hobby-grade sensors' real accuracy backs up one decimal place,
  not two) -- `provision()` never reads either (only `id`/`sensor`/`property`/
  `unit` go on the wire), they exist so a sketch's one per-channel
  table can feed both `provision()`'s identity and `log()`/a display's
  formatting (`examples/BME280Node`: `kChannels[i].label`,
  `kChannels[i].decimalPlaces`, `kChannels[i].unit`, all read by a
  local `printReadings(values)` helper that walks `values` zipped
  against `kChannels` the same way `log()` does, printing one OLED
  line per entry) instead of a second list kept in sync by hand at the
  same index. `label` exists separately from `property` because
  `property` is meant for the server/reports and can run long
  (`"Dew Point Temperature"`), too long for the 128x64 OLED's
  default-font grid (21 columns x 8 rows at `setTextSize(1)`).
  `provision()` posts the device name/id/`reportEveryCycles` and a
  `channel_id,sensor,property,unit` list (`SensorNodeChannel[]`, one entry per channel --
  fixed per sketch in every example except `DS18B20GridNode.ino`, which builds its list at
  runtime from a fetched grid config, see below) to `/sensors/provision` -- gated by `needsProvisioning()`
  (`SensorNodeConfig`'s `provisionPending` NVS flag), not called
  unconditionally every boot: it can't run from inside the portal
  itself (the device isn't online as a station yet at that point), so
  `handleSave()` just marks the flag and the sketch checks it after
  the reboot's `begin()` reconnects -- `if (node.needsProvisioning())
  node.provision(kChannels);`, right after `begin()`, before the first
  `log()`. Server-side it's a non-empty upsert (creates the row if
  missing, otherwise updates only the fields this call actually sent a
  non-empty value for), not a pure create-once, which is what makes it
  safe to call outside `needsProvisioning()` too if ever needed -- just
  redundant once already confirmed. A blank `name=` never blanks out a
  device name set by hand server-side, but a real device
  name/sensor/property/unit this call reports does overwrite whatever
  was there. A confirmed response (`"Provisioned"` in the body) clears
  the pending flag; anything else (no connectivity, server error)
  leaves it set so the next boot's `begin()` gets another attempt --
  see `SensorNode::provision()`. Both `log()` and
  `provision()` share a `postToServer(host, path, body)` helper for the
  connect/write/read boilerplate; `connectToServer()` factors out just the TLS
  connect-with-one-retry step, in case a future caller besides `postToServer()` needs it.
  `SensorNode::fetchConfig()` (added 2026-08-27) POSTs `key`/`device` (the same auth every other
  endpoint uses -- POST, not GET, so the API key never ends up in the access log) to the
  server's `config` endpoint and returns just the response body via `extractBody()` (empty on
  any failure, including a non-200 status -- unlike `log()`/`provision()`, which just
  substring-search the full raw response, this caller needs clean content with no stray header
  bytes). A plain fetch for a sketch that needs its own server-hosted config beyond
  `provision()`'s fixed fields, e.g. `DS18B20GridNode.ino` fetching its per-deployment ROM-ID
  grid layout from `sensors/config.php` (`larsi-org/html` repo) -- that endpoint resolves `key`
  to a prefix server-side the same way `log.php`/`provision.php` do, so the device itself never
  needs to know its own prefix. Doesn't touch NVS or any persisted state, unlike `log()`/
  `provision()`. `sanitizeHostname()` derives the
  network hostname from `deviceName` (letters/digits/hyphens only,
  runs of anything else collapsed to one `-`) -- `deviceName` stays
  free-text and unsanitized everywhere else (`provision()`'s `name=`,
  reports), since `WiFi.setHostname()` is the only consumer that
  actually needs the restricted shape. `openPortal()` is a thin
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
  `delay(1000)` and the `provision()` call (now gated on
  `needsProvisioning()`) are left in each sketch on purpose (baud rate
  and channel list are legitimately per-sketch).
  `checkPendingCommand()`/`applyPendingCommand()` (added 2026-08-25) are the client half of
  the server's one-shot `device.pending_command` test mailbox
  (`larsi.org/sensors/api.php`'s `Command:`/`pending_command` docs): `log()` parses
  a confirmed response's optional `Command: ...` line and persists it (free functions
  `pendingCommand()`/`setPendingCommand()`/`clearPendingCommand()` in
  `SensorNodeConfig.*`, deliberately not routed through the `SensorNodeConfig` struct/
  `config_` -- same reason `firmwareVersionChanged()` is a free function, see below);
  `applyPendingCommand()` (called from `loop()`, right after `log()`) is a small allowlist
  of commands that don't need `begin()` to not have connected yet -- currently just
  `"scan_i2c"` (added 2026-08-25: sweeps every I2C address, `Serial.printf`s whatever
  ACKed, `clearPendingCommand()`s itself, returns -- see `scanI2CBus()`, an anonymous-
  namespace helper needing `SensorNode.cpp`'s only `#include <Wire.h>`, since `Wire.begin()`
  had until now always been the sketch's own job, e.g. `BME280Node.ino`). Deliberately
  Serial-only, never reported back to the server: it's a bench-testing aid for whoever's
  physically at the device with a serial connection already open (see
  [[bench-test-serial-only]]), so there's no reason to round-trip the result anywhere.
  `"open_portal"` restarts the device instead (still inside `applyPendingCommand()`),
  without clearing the flag first; `checkPendingCommand()` (called from `setup()`,
  alongside `checkFirmwareVersion()`, before `begin()`) is what that restart actually
  reaches -- it's the only value *it* recognizes too, and it clears the flag and calls
  `openPortal()`. This split -- an allowlist of immediate commands in
  `applyPendingCommand()`, one deferred command in `checkPendingCommand()` at the next
  boot -- is deliberate: `checkPendingCommand()` mirrors `checkFirmwareVersion()`'s
  defer-to-next-boot shape because a command needing to run *before* `begin()` connects
  (`open_portal` today; simulating a network outage is the standing example of a future
  one) has no other way to do that, while a command like `scan_i2c` that doesn't share
  that constraint gets to skip the reboot round-trip entirely.

  **Anything neither method recognizes is left completely untouched** (2026-08-25 revision
  -- both used to have a catch-all: `applyPendingCommand()` defaulted unrecognized commands
  to a restart, `checkPendingCommand()` defaulted to clearing-and-dropping them). That
  catch-all was actively wrong for a real case that came up while scoping 1-Wire/DS18B20
  support: a sketch-specific command like `scan_1wire` needs the third-party `OneWire`
  library, and since Arduino compiles every `.cpp` under `src/` regardless of what a sketch
  `#include`s (the same reason `SensorNodeBattery` already applies to every sketch, see
  above), baking `scan_1wire` into this library the same way as `scan_i2c` would force
  `OneWire` onto every sketch using `SensorNode`, not just ones with a 1-Wire bus --
  unlike `Wire`/I2C, `OneWire` doesn't ship with the ESP32 core, so this is a real added
  dependency, not a free one (worth separately noting: `OneWire`'s current *published*
  release, 2.3.8, doesn't even compile on ESP32-C6 at all -- the fix landed on GitHub `main`
  in June 2025 but isn't in a tagged release yet, so using it here means installing from
  GitHub directly, not Library Manager, until that changes). Leaving unrecognized commands
  alone instead makes `pendingCommand()`/`setPendingCommand()`/`clearPendingCommand()`
  (`SensorNodeConfig.h`) a general extension point: a sketch owns its own command name
  entirely, checking/clearing it itself (typically right after `applyPendingCommand()` in
  `loop()`), so a sketch-specific dependency never has to touch this library at all. The
  trade-off: a command nothing ever claims (a typo, or one written for firmware that
  predates it) no longer self-clears -- it sits in `pending_command` until someone notices
  and clears it by hand. Acceptable since this mailbox is a manually-triggered testing aid,
  not high-volume traffic.

  **RTC ring buffer (added 2026-08-26)** -- `log()`'s actual buffering implementation,
  the piece `reportEveryCycles` existed for since 2026-08-25 but nothing read yet. A
  fixed-size ring (`RTCRingSlot slots[64]` -- `{uint32_t epoch; float values[16];}`, sized
  to the worst case regardless of what a given sketch actually reports) lives in an
  anonymous-namespace `RTC_DATA_ATTR` struct in `SensorNode.cpp`, alongside `head`/`tail`
  (ever-increasing `uint32_t` counters, not pre-masked -- a slot's real index is
  `counter & 63`) and a `magic` canary. `RTC_DATA_ATTR` survives deep sleep and
  `ESP.restart()`, which is what makes this the piece that lets a later deep-sleep sketch
  be a drop-in rather than a rework -- but it does *not* survive a power-on/EN reset,
  which is exactly what flashing new firmware over USB does. `ensureRingInitialized()`
  is the guard: if `magic` doesn't match the expected constant, `head`/`tail` reset to 0
  before anything reads them, so a cold boot never mistakes undefined RTC memory content
  for real ring state -- the flip side is that a physical reflash silently drops whatever
  was still queued, an accepted trade-off rather than something to engineer around.
  Every `log()` call pushes the current reading at `slots[head & 63]` and increments
  `head` unconditionally, *before* deciding whether to attempt a network flush -- so a
  reading is durably queued even on a wake that doesn't flush, or where the flush fails.
  If the push leaves the ring over capacity (`head - tail > 64`), the same write path also
  increments `tail` -- there's deliberately no separate consumer-side bookkeeping to keep
  `tail` valid; the one place that produces an overflow is the one place that corrects it.
  Flush cadence reuses `head` itself as the wake counter (`head % reportEveryCycles == 0`)
  instead of a second counter, since exactly one push happens per call; `reportEveryCycles
  == 1` (the default, and every device's setting until re-provisioned otherwise) flushes
  every single call, identical to `log()`'s pre-buffering behavior. A flush walks every
  slot from `tail` to `head`, building one request with repeated `data[]=`/`t[]=` pairs
  (`t[]` = seconds before `now`, matching the batched wire form `sensors/log.php`/
  `sensors/api.php` already document and accept) -- `tail` only advances to `head`
  once the response confirms `"Data logged"`; a failed or skipped flush leaves `tail`
  alone, so the next flush attempt naturally resends the whole backlog combined with
  whatever's accumulated since, with no separate retry path. A `log()` call that pushes
  but doesn't attempt a flush that wake now returns `true` (queued, not sent) rather than
  the pre-buffering "server confirmed" meaning -- checked that no example sketch reads
  `log()`'s return value before making that change (they all discard it: `node.log(...)`
  as a bare statement in every `loop()`).

  **Sleep -- not yet implemented (open design question as of 2026-08-26).** The ring
  buffer above exists specifically to make this a drop-in later, but "add sleep" isn't
  one swap-in -- there's a real choice of *how much* to sleep, not yet resolved:
  (1) WiFi modem-sleep only (`WiFi.setSleep(true)`) -- radio naps between beacons, CPU
  keeps running, no reboot, smallest win since the CPU's own idle draw still dominates;
  (2) light sleep (`esp_light_sleep_start()`) -- CPU halts between wakes, RAM retained,
  no reboot, `loop()` just resumes where it left off, WiFi association can often survive
  it; (3) deep sleep -- everything but the RTC domain powers off, lowest power by far, but
  the chip *reboots* on every wake (no persistent `loop()` -- every cycle is a fresh pass
  through `setup()`, which is exactly why the ring buffer lives in `RTC_DATA_ATTR` and not
  a plain global) and pays full Wi-Fi reassociation (scan/connect/DHCP/NTP) every wake,
  amortizing fine at 5+ minute intervals but wasteful at 1-minute ones. Deep sleep is the
  most likely real target given this is battery-powered (the whole reason
  `SensorNodeBattery`'s SOC channel exists) and light/modem sleep leave the CPU's own draw
  on the table, but nothing is decided -- don't assume deep sleep without checking back.
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
  hardware rather than an optional peripheral. `readSOC()` clamps to
  100% -- the MAX17048's own SOC estimate regularly overshoots past
  100% while charging (a known quirk of its ModelGauge algorithm, not
  a bug in this wrapper or upstream `SFE_MAX1704X`), and every sketch
  logs the raw return value straight to the `sensors/log` API, so
  clamping here is the one place that fixes it for every device.

## Networking

- **DNS** (`SensorNode::resolveServerIp()`) parses `config_.serverUrl` into `serverHost_`/
  `serverBasePath_` (see `parseServerUrl()`, `SensorNodePortal.h`) every call -- cheap, pure
  string work, no I/O -- but only actually resolves `serverHost_` via `WiFi.hostByName()` and
  caches the result in `serverIp_` once, re-resolving only on failure -- deliberately not every
  `log()` call, since `NetworkManager::hostByName()` doesn't take the TCPIP core lock lwIP
  requires (`arduino-esp32` issue #10526, still unpatched as of core 3.3.11) and can hang or
  crash under load. If DNS starts failing/crashing again, fall back to hardcoding the resolved
  IP and passing `serverHost_` to `connect()` only for TLS SNI/the HTTP Host header.
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

`SensorNode.cpp` verifies against a curated 5-root CA bundle
(`src/SensorNodeCertBundle.h`, via `WiFiClientSecure::setCACertBundle()`
+ `connect()` with a null `CA_cert` so it falls through to the bundle
path -- see `ssl_client.cpp`'s `rootCABuff != NULL` / `useRootCABundle`
branching if that ever needs re-verifying against a core update), not
a single pinned cert or a generic trust store or `setInsecure()`. That
header has the full reasoning, the five CAs picked and why, and --
important if flash pressure ever forces trimming the bundle back
down -- the order to remove them in. Don't hand-edit the byte array
there; regenerate it per that header's instructions.

A single pinned cert (what this used to do) breaks the instant
larsi.org's certificate provider changes; a full public-CA-store
bundle was tried and ruled out by actually compiling it -- ~55KB of
cert data plus ~71KB of extra linked code (mbedTLS's bundle search/
verify path isn't linked at all for a single pinned cert) overflows
this device's flash, which was already at 92% before any bundle. The
curated 5 -- ISRG Root X1 (Let's Encrypt), GlobalSign Root R46,
USERTrust RSA Certification Authority (Sectigo), Go Daddy Root
Certificate Authority - G2 (current), DigiCert Global Root G2 --
cover ~94% of the web by W3Techs' market-share survey at a cost of
~2.8KB data + the ~71KB fixed bundle-support overhead, landing at 97%
flash on `BME280Node`.

## Conventions

Same discipline as the `larsi-org/html` repo this device reports to:
MIT license, no `Co-Authored-By` in commits, README covers usage and
API, this file covers architecture/non-obvious decisions for future
sessions.
