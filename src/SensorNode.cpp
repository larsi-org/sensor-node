#include "SensorNode.h"

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <sys/time.h>
#include <time.h>

#include <cctype>
#include <cstring>

#include "SensorNodePortal.h"
#include "CertBundle.h"

namespace {

// Recognized pending command values -- see checkPendingCommand() (defers to next boot) and
// applyPendingCommand() (runs immediately) for which is which.
const char *kOpenPortalCommand = "open_portal";
const char *kScanI2CCommand = "scan_i2c";

// log()'s buffer: a fixed-size ring living in RTC slow memory, which survives deep sleep and
// ESP.restart() (but not a power-on/EN reset -- see ensureRingInitialized() below). Sized to the
// worst case (kMaxChannels, matching log()'s own byId[] below), not whatever a given sketch
// actually reports, so this struct's shape never depends on which sketch includes the library.
// head/tail are ever-increasing counters (not pre-masked) so a full-buffer eviction is just
// "advance both" with no separate wraparound bookkeeping; head also doubles as the flush-cadence
// counter in log() below, since exactly one push happens per call.
const uint8_t kRingCapacity = 64;
const uint8_t kMaxChannels = 16;
const uint32_t kRingMagic = 0x53454e31;  // "SEN1" -- canary distinguishing real ring state from
                                          // undefined RTC memory content on a cold power-on boot

struct RTCRingSlot {
  uint32_t epoch;
  float values[kMaxChannels];  // byId, NAN = channel not reported that cycle
};

RTC_DATA_ATTR struct {
  uint32_t magic;
  uint32_t head;  // next write index; slot = head & (kRingCapacity - 1)
  uint32_t tail;  // oldest not-yet-flushed index; slot = tail & (kRingCapacity - 1)
  RTCRingSlot slots[kRingCapacity];
} rtcRing;

// A power-on/EN reset leaves RTC memory content undefined -- deep-sleep wake and
// ESP.restart() both preserve it, but this guard is what tells the two cases apart, so a
// physical reflash (which resets via EN) starts with an empty buffer rather than reading
// garbage as ring state. That also means a reflash silently drops whatever was still
// queued -- an accepted trade-off, not a bug.
void ensureRingInitialized() {
  if (rtcRing.magic == kRingMagic) return;
  rtcRing.magic = kRingMagic;
  rtcRing.head = 0;
  rtcRing.tail = 0;
}

// Any epoch below this means the clock was never synced (it runs from ~0 at boot until NTP
// answers): readings queued in that state carry seconds-since-boot, and syncTime() below
// shifts them onto the real timeline once the clock is set. Also gates TLS, whose certificate
// check needs a real date.
const time_t kValidEpoch = 1600000000;  // Sept 2020

bool clockIsSet() { return time(nullptr) > kValidEpoch; }

// Reconnect policy (SensorNode::service()). A failed round over every saved network is
// followed by a pause that doubles from kBackoffStartMs to kBackoffMaxMs (plus up to 25%
// jitter, so several nodes on one router do not all retry in lockstep after a power cut).
// The setup portal comes up in the background after kPortalAfterMs without a connection; a
// device that has been offline kRestartAfterMs (and has nobody on its portal) restarts, which
// clears a wedged radio and keeps the RTC ring buffer.
const unsigned long kBackoffStartMs = 5000;
const unsigned long kBackoffMaxMs = 5UL * 60 * 1000;
const unsigned long kPortalAfterMs = 60UL * 1000;
const unsigned long kRestartAfterMs = 30UL * 60 * 1000;
const unsigned long kClockRetryMs = 60UL * 1000;

// Sweeps every I2C address on whatever Wire is already using (defensive Wire.begin() -- this
// may run on a sketch that never touched Wire itself, e.g. BasicNode) and returns a comma-joined
// list of hex addresses that ACKed, or "none". Bench-testing aid only (see
// applyPendingCommand()): prints to Serial, never sent to the server -- whoever triggers this
// is physically at the device with a serial connection already open.
String scanI2CBus() {
  Wire.begin();
  String result;
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      if (result.length() > 0) result += ",";
      result += "0x" + String(address, HEX);
    }
  }
  return result.length() > 0 ? result : "none";
}

// Reduces a free-text device name to characters safe for a network
// hostname (RFC 1123: letters, digits, hyphens; can't start/end with
// a hyphen). WiFi.setHostname() itself doesn't validate or reject
// anything -- it just truncates to 31 chars -- but routers' DHCP/mDNS
// handling of a raw name outside that shape (spaces especially) can
// be unpredictable. Runs of disallowed characters collapse to a
// single hyphen; leading ones are dropped rather than producing a
// leading hyphen. The un-sanitized name is still what's sent to the
// provision endpoint and shown in reports -- this only affects what's
// advertised on the network.
String sanitizeHostname(const String &name) {
  String result;
  result.reserve(name.length());
  bool lastWasHyphen = false;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if (isalnum((unsigned char)c)) {
      result += c;
      lastWasHyphen = false;
    } else if (!lastWasHyphen && result.length() > 0) {
      result += '-';
      lastWasHyphen = true;
    }
  }
  while (result.length() > 0 && result[result.length() - 1] == '-') {
    result.remove(result.length() - 1);
  }
  return result;
}

// Writes an already-built raw HTTP/1.1 request and reads the raw
// response (status line, headers, body) back as one string.
//
// Deliberately not HTTPClient: on this hardware, the extra time
// HTTPClient takes between connect() succeeding and actually writing
// the request (constructing the object, addHeader(), etc.) was enough
// for the connection to get closed before a byte went out. Writing a
// single pre-built request immediately after connect() avoids that gap.
String rawHttpRequest(WiFiClientSecure &client, const String &request, unsigned long timeoutMs) {
  client.print(request);
  String response;
  unsigned long start = millis();
  while (millis() - start < timeoutMs && (client.connected() || client.available())) {
    while (client.available()) response += (char)client.read();
  }
  return response;
}

// Opens a TLS connection to host (TLS SNI + HTTP Host header -- see
// SensorNode::resolveServerIp() for where host/serverIp come from), retrying once
// (WiFiClientSecure occasionally fails its first connect attempt right after Wi-Fi association)
// before giving up. Factored out of postToServer() so a future second caller doesn't have to
// duplicate the retry dance.
bool connectToServer(WiFiClientSecure &client, IPAddress serverIp, const String &host) {
  client.setCACertBundle(kServerCertBundle, kServerCertBundleLen);
  client.setHandshakeTimeout(15);  // seconds; default is 120
  // CA_cert/cert/private_key all null -- setCACertBundle() above is what actually verifies the
  // chain (see NetworkClientSecure's connect(): a null CA_cert falls through to the bundle
  // path rather than skipping verification, since useRootCABundle is a separate flag).
  if (client.connect(serverIp, 443, host.c_str(), nullptr, nullptr, nullptr)) return true;
  if (client.connect(serverIp, 443, host.c_str(), nullptr, nullptr, nullptr)) return true;
  char err[128];
  client.lastError(err, sizeof(err));
  Serial.printf("[SensorNode] TLS connect failed: %s\n", err);
  return false;
}

// Shared by log() and provision(): writes body as a form-encoded POST to path, and returns the
// raw response (empty string on connect failure).
String postToServer(IPAddress serverIp, const String &host, const String &path, const String &body) {
  WiFiClientSecure client;
  if (!connectToServer(client, serverIp, host)) return String();

  String request = "POST " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + "\r\n";
  request += "Content-Type: application/x-www-form-urlencoded\r\n";
  request += "Content-Length: " + String(body.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += body;

  String response = rawHttpRequest(client, request, 5000);
  client.stop();
  return response;
}

// Backs SensorNode::fetchConfig(): strips a raw postToServer() response down to just its body,
// or an empty string if the status line isn't 200 -- unlike log()/provision(), which just
// substring-search the full raw response and don't care about stray header bytes mixed in,
// fetchConfig()'s caller needs exactly the server-hosted content with nothing else attached.
String extractBody(const String &response) {
  if (response.indexOf(" 200 ") < 0) return String();
  int bodyStart = response.indexOf("\r\n\r\n");
  return bodyStart >= 0 ? response.substring(bodyStart + 4) : String();
}

// A single NTP query over WiFiUDP, setting the system clock on a valid
// reply. Literal IPs, not hostnames -- Cloudflare's and Google's
// long-stable public NTP anycast addresses, so no DNS lookup needed.
bool ntpQuery(const char *serverIp, unsigned long timeoutMs) {
  WiFiUDP udp;
  if (!udp.begin(2390)) return false;

  uint8_t packet[48] = {0};
  packet[0] = 0b00100011;  // LI=0, VN=4, Mode=3 (client)
  if (!udp.beginPacket(serverIp, 123)) {
    udp.stop();
    return false;
  }
  udp.write(packet, sizeof(packet));
  udp.endPacket();

  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (udp.parsePacket() >= (int)sizeof(packet)) {
      udp.read(packet, sizeof(packet));
      udp.stop();
      uint32_t secsSince1900 = ((uint32_t)packet[40] << 24) | ((uint32_t)packet[41] << 16) |
                                ((uint32_t)packet[42] << 8) | (uint32_t)packet[43];
      const uint32_t kSecondsFrom1900To1970 = 2208988800UL;
      struct timeval tv = {};
      tv.tv_sec = secsSince1900 - kSecondsFrom1900To1970;
      settimeofday(&tv, nullptr);
      return true;
    }
    delay(50);
  }
  udp.stop();
  return false;
}

// WiFiClientSecure checks the pinned cert's validity dates against the
// device clock, which boots near the epoch -- without this, every TLS
// connection fails since the cert looks "not yet valid" until synced.
void syncTime() {
  time_t before = time(nullptr);
  bool synced = ntpQuery("162.159.200.1", 5000) || ntpQuery("216.239.35.0", 5000);
  time_t after = time(nullptr);
  Serial.printf("[SensorNode] Time sync %s (epoch %ld)\n", synced ? "OK" : "FAILED", (long)after);

  if (synced && before < kValidEpoch && after >= kValidEpoch) {
    // Readings queued while the clock was unset hold seconds-since-boot: move them onto the
    // real timeline by the same jump the clock just made.
    ensureRingInitialized();
    uint32_t shift = (uint32_t)(after - before);
    for (uint32_t i = rtcRing.tail; i != rtcRing.head; i++) {
      RTCRingSlot &entry = rtcRing.slots[i & (kRingCapacity - 1)];
      if (entry.epoch < (uint32_t)kValidEpoch) entry.epoch += shift;
    }
  }
}

}  // namespace

void SensorNode::begin(unsigned long connectTimeoutMs) {
  connectTimeoutMs_ = connectTimeoutMs;

  if (!loadSensorNodeConfig(config_)) {
    // Nothing saved yet (first boot, or a wipe): there is nothing to retry, so the portal is
    // the only way forward.
    Serial.println("[SensorNode] No saved settings -- starting setup portal.");
    runSensorNodeSetupPortal();  // never returns; device restarts on success
  }

  String hostname = sanitizeHostname(config_.deviceName);
  if (hostname.length() > 0) {
    WiFi.setHostname(hostname.c_str());
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // service() owns every reconnect, so two never race

  bool connected = false;
  for (uint8_t i = 0; i < SensorNodeConfig::kMaxNetworks && !connected; i++) {
    if (config_.ssids[i].length() == 0) continue;

    Serial.printf("[SensorNode] Connecting to \"%s\"...\n", config_.ssids[i].c_str());
    WiFi.begin(config_.ssids[i].c_str(), config_.passwords[i].c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < connectTimeoutMs) {
      delay(250);
    }
    connected = (WiFi.status() == WL_CONNECTED);
    if (!connected) WiFi.disconnect();
  }

  if (connected) {
    onConnected();
  } else {
    // Saved networks exist, so this is an outage (or a moved device) rather than a fresh one:
    // carry on. service() keeps retrying with backoff and opens the portal in the background
    // if the outage lasts (see SensorNode.h).
    Serial.println("[SensorNode] Not connected -- will keep retrying; setup portal opens if this lasts.");
    downSince_ = millis();
  }
  publishStatus();
}

void SensorNode::onConnected() {
  wasConnected_ = true;
  downSince_ = 0;
  attemptActive_ = false;
  attemptIndex_ = 0;
  backoffMs_ = kBackoffStartMs;
  roundPause_ = false;
  Serial.printf("[SensorNode] Connected, IP %s\n", WiFi.localIP().toString().c_str());
  resolveServerIp();  // best-effort here; log() retries later if this fails
  syncTime();
  lastClockTry_ = millis();
}

bool SensorNode::connected() const { return WiFi.status() == WL_CONNECTED; }

SensorNodeStatus SensorNode::status() const {
  SensorNodeStatus st;
  st.connected = WiFi.status() == WL_CONNECTED;
  st.portal = sensorNodePortalActive();
  if (st.portal) {
    st.ip = WiFi.softAPIP();
  } else if (st.connected) {
    st.ip = WiFi.localIP();
  }
  if (st.connected) {
    st.rssi = (int8_t)WiFi.RSSI();
  } else if (downSince_ != 0) {
    st.downSeconds = (millis() - downSince_) / 1000;
  }
  return st;
}

void SensorNode::publishStatus() { publishSensorNodeStatus(status()); }

// Moves attemptIndex_ forward to the next saved network at or after it; false once past the
// last one (the round is over).
bool SensorNode::pickNetwork() {
  while (attemptIndex_ < SensorNodeConfig::kMaxNetworks) {
    if (config_.ssids[attemptIndex_].length() > 0) return true;
    attemptIndex_++;
  }
  return false;
}

void SensorNode::service() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasConnected_) {
      Serial.printf("[SensorNode] Wi-Fi back after %lu s.\n", downSince_ ? (now - downSince_) / 1000 : 0UL);
      onConnected();
    } else if (!clockIsSet() && now - lastClockTry_ >= kClockRetryMs) {
      lastClockTry_ = now;
      syncTime();
    }
    // Close the background portal once online, but not under someone's hands.
    if (sensorNodePortalActive() && !sensorNodePortalBusy()) stopSensorNodePortal();
    serviceSensorNodePortal();
    publishStatus();
    return;
  }

  // Not connected.
  if (wasConnected_) {
    wasConnected_ = false;
    downSince_ = now;
    attemptActive_ = false;
    attemptIndex_ = 0;
    roundPause_ = false;
    backoffMs_ = kBackoffStartMs;
    Serial.println("[SensorNode] Wi-Fi lost -- reconnecting.");
  }
  if (downSince_ == 0) downSince_ = now;

  if (now - downSince_ >= kRestartAfterMs && !sensorNodePortalBusy()) {
    Serial.println("[SensorNode] Offline too long -- restarting.");
    delay(100);
    ESP.restart();
  }

  // Reconnect attempts pause while someone is using the portal: a scan or association attempt
  // would drop their page.
  if (!sensorNodePortalBusy()) {
    if (attemptActive_) {
      if (now - attemptStart_ >= connectTimeoutMs_) {
        WiFi.disconnect();
        attemptActive_ = false;
        attemptIndex_++;
      }
    }
    if (!attemptActive_) {
      if (roundPause_ && now - roundEnd_ < pauseMs_) {
        // still backing off
      } else {
        roundPause_ = false;
        if (pickNetwork()) {
          Serial.printf("[SensorNode] Reconnecting to \"%s\"...\n", config_.ssids[attemptIndex_].c_str());
          WiFi.begin(config_.ssids[attemptIndex_].c_str(), config_.passwords[attemptIndex_].c_str());
          attemptActive_ = true;
          attemptStart_ = now;
        } else {
          // Every saved network failed this round: back off before the next one.
          attemptIndex_ = 0;
          roundPause_ = true;
          roundEnd_ = now;
          pauseMs_ = backoffMs_ + random(backoffMs_ / 4 + 1);
          backoffMs_ = min(backoffMs_ * 2, kBackoffMaxMs);
        }
      }
    }
  }

  if (!sensorNodePortalActive() && now - downSince_ >= kPortalAfterMs) {
    Serial.println("[SensorNode] Still offline -- opening setup portal in the background.");
    startSensorNodePortal();
  }
  serviceSensorNodePortal();
  publishStatus();
}

void SensorNode::idle(unsigned long ms) {
  unsigned long start = millis();
  while (true) {
    service();
    unsigned long elapsed = millis() - start;
    if (elapsed >= ms) break;
    unsigned long tick = sensorNodePortalActive() ? 5 : 200;  // the portal wants quick polls
    delay(min(ms - elapsed, tick));
  }
}

void SensorNode::waitForWifi() {
  while (WiFi.status() != WL_CONNECTED) idle(1000);
  service();  // runs onConnected() if the connection came in between ticks
}

void SensorNode::resetConfig() { clearSensorNodeConfig(); }

void SensorNode::openPortal() { runSensorNodeSetupPortal(); }

void SensorNode::checkFirmwareVersion(uint32_t version) {
  if (!firmwareVersionChanged(version)) return;
  Serial.printf("[SensorNode] Firmware version changed (now %lu) -- opening setup portal.\n",
                (unsigned long)version);
  runSensorNodeSetupPortal(version);  // never returns; reboots on save
}

void SensorNode::checkPendingCommand() {
  String command = pendingCommand();
  if (command != kOpenPortalCommand) return;  // not ours -- see the .h comment

  clearPendingCommand();
  Serial.println("[SensorNode] Pending command \"open_portal\" -- opening setup portal.");
  openPortal();  // never returns; reboots on save
}

void SensorNode::checkPortalButton(uint8_t pin, unsigned long wipeHoldMs) {
  pinMode(pin, INPUT_PULLUP);
  if (digitalRead(pin) != LOW) return;

  unsigned long heldStart = millis();
  while (digitalRead(pin) == LOW && millis() - heldStart < wipeHoldMs) {
    delay(50);
  }
  if (millis() - heldStart >= wipeHoldMs) {
    Serial.println("[SensorNode] Portal button held long -- wiping saved config.");
    resetConfig();  // begin() below falls straight into the portal
  } else {
    Serial.println("[SensorNode] Portal button held short -- opening portal (nothing erased).");
    openPortal();  // never returns
  }
}

bool SensorNode::resolveServerIp() {
  if (!parseServerUrl(config_.serverUrl, serverHost_, serverBasePath_)) {
    Serial.printf("[SensorNode] Invalid server URL \"%s\"\n", config_.serverUrl.c_str());
    return false;
  }

  if (serverIp_ != IPAddress()) return true;
  Serial.printf("[SensorNode] Resolving %s...\n", serverHost_.c_str());
  if (!WiFi.hostByName(serverHost_.c_str(), serverIp_)) {
    Serial.printf("[SensorNode] Could not resolve %s\n", serverHost_.c_str());
    serverIp_ = IPAddress();
    return false;
  }
  Serial.printf("[SensorNode] Resolved %s -> %s\n", serverHost_.c_str(), serverIp_.toString().c_str());
  return true;
}

bool SensorNode::log(const std::vector<SensorNodeChannel> &channels, const std::vector<float> &values) {
  // Channels 0-15 -- see SensorNodeChannel's id comment. Laid out sparsely by id here (not by
  // position within values/channels) so the ring push and the serialization loop below can
  // both walk them in wire order regardless of what order the caller listed channels in.
  float byId[kMaxChannels];
  uint8_t decimalPlacesById[kMaxChannels];
  for (uint8_t id = 0; id < kMaxChannels; id++) {
    byId[id] = NAN;
    decimalPlacesById[id] = 1;  // a buffered older slot's id not in this call's channels (the
                                 // channel list changing between calls, which no real sketch
                                 // does) would otherwise read this uninitialized
  }

  // Zipped, not id-keyed: values[i] is channels[i]'s reading -- see log()'s header comment on
  // why that means channels' order can't change without every values list built against it
  // changing to match.
  for (size_t i = 0; i < values.size() && i < channels.size(); i++) {
    uint8_t id = channels[i].id;
    if (id >= kMaxChannels) continue;  // shouldn't happen; guards a bad id
    byId[id] = values[i];
    decimalPlacesById[id] = channels[i].decimalPlaces;
  }

  // A cycle whose only reading is the battery (channel 15, reserved sitewide for it) logs nothing
  // at all: a node whose real sensors are all dead (e.g. a disconnected 1-Wire bus) must go
  // silent on the server so staleness checks notice, instead of looking alive on battery alone.
  // The same rule makes an all-NAN cycle log nothing, rather than an empty row that would keep
  // device.last_epoch fresh.
  bool anyNonBatteryReading = false;
  for (uint8_t id = 0; id < kMaxChannels - 1; id++) {
    if (!isnan(byId[id])) {
      anyNonBatteryReading = true;
      break;
    }
  }
  if (!anyNonBatteryReading) {
    Serial.println("[SensorNode] No sensor readings this cycle (battery alone doesn't count) -- not logging.");
    return false;
  }

  // Buffer first, unconditionally -- every wake's reading is durably queued in the RTC ring
  // regardless of whether this wake also attempts a flush below, so a failed/skipped flush
  // never loses a reading, just defers it.
  ensureRingInitialized();
  RTCRingSlot &slot = rtcRing.slots[rtcRing.head & (kRingCapacity - 1)];
  slot.epoch = (uint32_t)time(nullptr);
  memcpy(slot.values, byId, sizeof(byId));
  rtcRing.head++;
  if (rtcRing.head - rtcRing.tail > kRingCapacity) {
    rtcRing.tail++;  // buffer's full -- evict the oldest unflushed entry to keep tail valid
  }

  // Flush cadence: every reportEveryCycles-th wake, reusing head itself as the wake counter
  // (exactly one push happens per call, so no separate counter is needed). reportEveryCycles
  // == 1 (the default, and every device's setting until re-provisioned) flushes every call --
  // identical to this method's behavior before buffering existed.
  if (rtcRing.head % config_.reportEveryCycles != 0) return true;  // queued, nothing to send yet

  service();  // a flush wake is a good moment to notice a drop (or a recovery)
  if (WiFi.status() != WL_CONNECTED) return false;  // still queued -- next flush wake retries
  if (!clockIsSet()) return false;  // TLS and the readings' offsets both need the real time
  if (!resolveServerIp()) return false;

  uint32_t now = (uint32_t)time(nullptr);
  String body = "key=" + config_.writeKey + "&device=" + String(config_.deviceId);
  for (uint32_t i = rtcRing.tail; i != rtcRing.head; i++) {
    const RTCRingSlot &entry = rtcRing.slots[i & (kRingCapacity - 1)];
    String data;
    for (uint8_t id = 0; id < kMaxChannels; id++) {
      if (id > 0) data += ",";
      // (unsigned int) cast: ESP32 core's String(float, unsigned int) is otherwise ambiguous
      // against its other explicit String(..., unsigned char) overloads when given a uint8_t
      // directly -- neither is a strictly better match across both arguments.
      if (!isnan(entry.values[id])) data += String(entry.values[id], (unsigned int)decimalPlacesById[id]);
    }
    uint32_t offset = now > entry.epoch ? now - entry.epoch : 0;
    body += "&data[]=" + data + "&t[]=" + String(offset);
  }

  String response = postToServer(serverIp_, serverHost_, serverBasePath_ + "log", body);

  Serial.printf("[SensorNode] POST /sensors/log (device=%d, body=%s):\n%s\n", config_.deviceId, body.c_str(), response.c_str());

  bool confirmed = response.indexOf("Data logged") >= 0;
  if (confirmed) {
    rtcRing.tail = rtcRing.head;  // everything just sent is flushed

    // Only trust a "Command: ..." line alongside a confirmed log -- the server only ever sends
    // one there, but a malformed/garbled response shouldn't be trusted to carry a command either.
    int cmdIdx = response.indexOf("Command: ");
    if (cmdIdx >= 0) {
      int lineEnd = response.indexOf('\n', cmdIdx);
      String command = response.substring(cmdIdx + 9, lineEnd >= 0 ? lineEnd : response.length());
      command.trim();
      if (command.length() > 0) setPendingCommand(command);
    }
  }

  return confirmed;
}

void SensorNode::applyPendingCommand() {
  String command = pendingCommand();

  if (command == kScanI2CCommand) {
    // Doesn't need begin() to not have connected yet, unlike open_portal -- runs right here.
    Serial.printf("[SensorNode] I2C scan: %s\n", scanI2CBus().c_str());
    clearPendingCommand();
    return;
  }

  if (command == kOpenPortalCommand) {
    // Doesn't clear the command itself -- it's already persisted, and staying set is what lets
    // checkPendingCommand() pick it up after this restart, at the top of the next setup().
    Serial.println("[SensorNode] Pending command received -- restarting to apply.");
    ESP.restart();
    return;
  }

  // Anything else -- including empty -- isn't ours to interpret. A sketch with its own
  // commands (e.g. a 1-Wire scan, without forcing that dependency on every sketch using this
  // library) can check pendingCommand()/clearPendingCommand() itself, e.g. right after this
  // call in loop() -- see SensorNodeConfig.h.
}

bool SensorNode::needsProvisioning() const { return provisionPending(); }

bool SensorNode::provision(const std::vector<SensorNodeChannel> &channels) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!resolveServerIp()) return false;

  String channelList;
  for (size_t i = 0; i < channels.size(); i++) {
    if (i > 0) channelList += "|";
    channelList += String(channels[i].id) + "," + channels[i].sensor + "," + channels[i].property + "," + channels[i].unit;
  }

  // Full MAC, no colons -- the server can't see this itself (a remote HTTP request only carries
  // the client's IP, never its link-layer address), so it has to ride along in the body like
  // every other hardware fact provision() reports.
  String mac = WiFi.macAddress();
  mac.replace(":", "");

  String body = "key=" + config_.writeKey + "&device=" + String(config_.deviceId) +
                "&name=" + config_.deviceName + "&mac=" + mac +
                "&logInterval=" + String(config_.logIntervalMinutes) +
                "&reportEvery=" + String(config_.reportEveryCycles) + "&channels=" + channelList;
  String response = postToServer(serverIp_, serverHost_, serverBasePath_ + "provision", body);

  Serial.printf("[SensorNode] POST /sensors/provision:\n%s\n", response.c_str());

  bool confirmed = response.indexOf("Provisioned") >= 0;
  if (confirmed) clearProvisionPending();
  return confirmed;
}

String SensorNode::fetchConfig() {
  if (WiFi.status() != WL_CONNECTED) return String();
  if (!resolveServerIp()) return String();
  String body = "key=" + config_.writeKey + "&device=" + String(config_.deviceId);
  String response = postToServer(serverIp_, serverHost_, serverBasePath_ + "config", body);
  return extractBody(response);
}
