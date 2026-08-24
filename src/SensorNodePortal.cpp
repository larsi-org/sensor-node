#include "SensorNodePortal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "SensorNodeConfig.h"

namespace {

const byte kDnsPort = 53;
DNSServer dnsServer;
WebServer server(80);
bool saved = false;

// Set once, at the top of runSensorNodeSetupPortal(); only actually persisted (see
// handleSave()) once the user submits, not just because the portal was entered -- see
// SensorNodePortal.h.
uint32_t pendingFirmwareVersion = 0;

// Last 6 hex digits of the MAC (its full non-OUI address space), not the first -- the first 3
// octets are the vendor OUI, shared across a huge range of Espressif chips, so every device from
// the same manufacturing batch would otherwise collide. The last 3 octets are the actual
// per-device-unique part. Shared by the AP name and the default device name below.
String macSuffix() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return mac.substring(mac.length() - 6);
}

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

// Matches the shape of larsi.org's own key generator (id.php's
// generateID(16)): exactly 16 chars, first a letter, the rest base64url
// (A-Z, a-z, 0-9, -, _). Not just a length check -- a key that's the
// right length but wrong shape (e.g. copy-pasted with a stray space,
// or missing the leading-letter constraint) would otherwise only fail
// once talking to the real server, with a generic "Key not found".
bool isValidWriteKey(const String &key) {
  if (key.length() != 16) return false;
  if (!isalpha((unsigned char)key[0])) return false;
  for (size_t i = 0; i < key.length(); i++) {
    char c = key[i];
    if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false;
  }
  return true;
}

// Allowed log-endpoint reporting intervals, in minutes -- the only values
// the portal's dropdown offers, and the only ones handleSave() accepts.
const uint8_t kLogIntervals[] = {1, 2, 3, 5, 10, 15, 20, 30, 60};
const size_t kLogIntervalCount = sizeof(kLogIntervals) / sizeof(kLogIntervals[0]);

bool isValidLogInterval(uint8_t minutes) {
  for (size_t i = 0; i < kLogIntervalCount; i++) {
    if (kLogIntervals[i] == minutes) return true;
  }
  return false;
}

String buildLogIntervalOptions(uint8_t selected) {
  String options;
  for (size_t i = 0; i < kLogIntervalCount; i++) {
    uint8_t minutes = kLogIntervals[i];
    options += "<option value=\"" + String(minutes) + "\"";
    if (minutes == selected) options += " selected";
    options += ">Every " + String(minutes) + (minutes == 1 ? " minute" : " minutes") + "</option>";
  }
  return options;
}

// Device IDs are 0-15 (larsi.org's flat sensor addressing is
// 16*deviceId+channel) -- small enough that a dropdown rules out an
// invalid value entirely, instead of relying on handleSave()'s
// constrain() to silently clamp a stray typed-in one.
String buildDeviceIdOptions(uint8_t selected) {
  String options;
  for (uint8_t id = 0; id <= 15; id++) {
    options += "<option value=\"" + String(id) + "\"";
    if (id == selected) options += " selected";
    options += ">" + String(id) + "</option>";
  }
  return options;
}

// Adds a new network at the front of config's known-network list
// (most-recently-added first), shifting the rest down and dropping
// the oldest if full. If ssid already matches an existing slot, just
// updates that slot's password in place instead of creating a
// duplicate entry -- handles "same network, password changed" without
// wasting a slot. The password field is never pre-filled in the form
// (see buildFormPage()), so a blank incoming password on an already-known
// SSID is treated as "didn't mean to change it" and leaves the saved one
// alone -- otherwise re-submitting the form just to tweak the device
// name would silently wipe a working Wi-Fi password. A blank
// password on a genuinely new SSID is still saved as-is (a real open
// network is a legitimate case there).
void addOrUpdateNetwork(SensorNodeConfig &config, const String &ssid, const String &password) {
  for (uint8_t i = 0; i < SensorNodeConfig::kMaxNetworks; i++) {
    if (config.ssids[i] == ssid) {
      if (password.length() > 0) config.passwords[i] = password;
      return;
    }
  }
  for (uint8_t i = SensorNodeConfig::kMaxNetworks - 1; i > 0; i--) {
    config.ssids[i] = config.ssids[i - 1];
    config.passwords[i] = config.passwords[i - 1];
  }
  config.ssids[0] = ssid;
  config.passwords[0] = password;
}

// Scanned networks, strongest signal first, de-duplicated by SSID (an
// AP with multiple radios/bands otherwise shows up more than once).
std::vector<String> scanNetworkNames() {
  int count = WiFi.scanNetworks();
  std::vector<std::pair<int32_t, String>> found;
  for (int i = 0; i < count; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool duplicate = false;
    for (auto &entry : found) {
      if (entry.second == ssid) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) found.push_back({WiFi.RSSI(i), ssid});
  }
  std::sort(found.begin(), found.end(),
            [](const std::pair<int32_t, String> &a, const std::pair<int32_t, String> &b) {
              return a.first > b.first;
            });

  std::vector<String> names;
  names.reserve(found.size());
  for (auto &entry : found) names.push_back(entry.second);
  return names;
}

String buildFormPage() {
  // Pre-fill everything from the existing config except the network
  // fields -- this portal only runs because none of the known
  // networks worked (moved to a new location, most likely), so the
  // common case is just adding one new network without retyping the
  // device name/id/write key.
  SensorNodeConfig existing;
  loadSensorNodeConfig(existing);

  std::vector<String> networks = scanNetworkNames();

  String options;
  if (networks.empty()) {
    options = "<option value=\"\">No networks found -- move closer and reset</option>";
  } else {
    for (auto &ssid : networks) {
      String escaped = htmlEscape(ssid);
      options += "<option value=\"" + escaped + "\">" + escaped + "</option>";
    }
  }

  String page;
  page += "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  page += "<title>Sensor Node Setup</title>";
  page += "<style>body{font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em}";
  page += "label{display:block;margin-top:1em;font-weight:bold}";
  page += "input,select{width:100%;padding:.4em;box-sizing:border-box;font-size:1em}";
  page += "button{margin-top:1.5em;padding:.6em 1.2em;font-size:1em}</style></head><body>";
  page += "<h1>Sensor Node Setup</h1>";
  if (existing.ssids[0].length() > 0) {
    page += "<p>Device name/ID/write key are pre-filled from the existing setup -- pick a new "
            "Wi-Fi network below. Up to " +
            String(SensorNodeConfig::kMaxNetworks) +
            " networks are remembered (oldest is replaced), so moving back later should "
            "reconnect automatically.</p>";
  }
  page += "<form method=\"POST\" action=\"/save\">";
  page += "<label>Wi-Fi Network</label><select name=\"ssid\">" + options + "</select>";
  page += "<label>Wi-Fi Password</label><input type=\"password\" name=\"password\" "
          "placeholder=\"Leave blank to keep the saved password for a known network\">";
  // Defaults to "Weather <last 6 MAC hex digits>" when nothing's saved yet, so a brand-new
  // device never ships with a blank/generic name -- required below forces this default (or
  // whatever the user replaces it with) to actually get submitted.
  String defaultDeviceName = existing.deviceName.length() > 0 ? existing.deviceName : "Weather " + macSuffix();
  page += "<label>Device Name (and Location)</label><input type=\"text\" name=\"deviceName\" maxlength=\"32\" required value=\"" +
          htmlEscape(defaultDeviceName) + "\">";
  page += "<label>Device ID</label><select name=\"deviceId\">" +
          buildDeviceIdOptions(existing.deviceId) + "</select>";
  page += "<label>Write Key (16 characters, starts with a letter)</label>";
  // maxlength deliberately generous (not 16): a pasted key with
  // accidental leading/trailing whitespace is longer than 16 chars
  // until trimmed server-side (handleSave()) -- a strict maxlength="16"
  // would silently truncate the paste first and clip real key
  // characters instead of the whitespace. The pattern likewise allows
  // surrounding whitespace so a legitimate padded paste still passes
  // client-side validation instead of just failing to submit.
  page += "<input type=\"text\" name=\"writeKey\" maxlength=\"32\" required "
          "pattern=\"\\s*[A-Za-z][A-Za-z0-9_-]{15}\\s*\" value=\"" +
          htmlEscape(existing.writeKey) +
          "\" title=\"16 characters: a letter, then letters/digits/-/_ (surrounding spaces OK)\">";
  page += "<label>Log Frequency</label><select name=\"logInterval\">" +
          buildLogIntervalOptions(existing.logIntervalMinutes) + "</select>";
  page += "<button type=\"submit\">Save &amp; Reboot</button>";
  page += "</form></body></html>";
  return page;
}

void handleRoot() { server.send(200, "text/html", buildFormPage()); }

void handleSave() {
  // Start from the existing config, not a blank one, so the other
  // known-network slots (and anything the user didn't touch) survive.
  SensorNodeConfig config;
  loadSensorNodeConfig(config);

  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");
  config.deviceName = server.arg("deviceName");
  config.deviceName.trim();  // strip accidental leading/trailing whitespace from copy-paste
  config.deviceId = (uint8_t)constrain(server.arg("deviceId").toInt(), 0, 15);
  config.writeKey = server.arg("writeKey");
  config.writeKey.trim();  // strip accidental leading/trailing whitespace from copy-paste
  uint8_t logInterval = (uint8_t)server.arg("logInterval").toInt();
  config.logIntervalMinutes = isValidLogInterval(logInterval) ? logInterval : 5;

  if (newSsid.length() == 0) {
    server.send(400, "text/html", "<p>Wi-Fi network is required. <a href=\"/\">Back</a></p>");
    return;
  }
  if (config.deviceName.length() == 0) {
    server.send(400, "text/html", "<p>Device Name is required. <a href=\"/\">Back</a></p>");
    return;
  }
  if (!isValidWriteKey(config.writeKey)) {
    server.send(400, "text/html",
                "<p>Write key must be 16 characters: a letter, then letters/digits/-/_ only. "
                "<a href=\"/\">Back</a></p>");
    return;
  }

  addOrUpdateNetwork(config, newSsid, newPassword);

  saveSensorNodeConfig(config);
  if (pendingFirmwareVersion != 0) markFirmwareVersionSeen(pendingFirmwareVersion);
  markProvisionPending();  // next begin()'s reconnect gets one provision() attempt -- see SensorNode.h
  server.send(200, "text/html", "<p>Saved. Rebooting...</p>");
  saved = true;
}

// Any unrecognized path bounces back to the form -- this is what makes
// phones/laptops auto-pop the captive portal page on connect.
void handleNotFound() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

}  // namespace

void runSensorNodeSetupPortal(uint32_t firmwareVersion) {
  pendingFirmwareVersion = firmwareVersion;

  WiFi.mode(WIFI_AP_STA);

  String apName = "SensorNode-Setup-" + macSuffix();
  WiFi.softAP(apName.c_str());
  IPAddress apIP = WiFi.softAPIP();

  Serial.printf("[SensorNode] Setup portal: join Wi-Fi \"%s\", then visit http://%s/\n",
                apName.c_str(), apIP.toString().c_str());

  dnsServer.start(kDnsPort, "*", apIP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  saved = false;
  unsigned long savedAt = 0;
  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (saved && savedAt == 0) savedAt = millis();
    if (savedAt != 0 && millis() - savedAt > 1000) ESP.restart();
  }
}
