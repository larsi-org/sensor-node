#include "SensorNodePortal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "SensorNodeConfig.h"

namespace {

const byte kDnsPort = 53;
DNSServer dnsServer;
WebServer server(80);
bool saved = false;

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
  page += "<form method=\"POST\" action=\"/save\">";
  page += "<label>Wi-Fi Network</label><select name=\"ssid\">" + options + "</select>";
  page += "<label>Wi-Fi Password</label><input type=\"password\" name=\"password\">";
  page += "<label>Node Name</label><input type=\"text\" name=\"nodeName\" maxlength=\"32\">";
  page += "<label>Device ID (0-255)</label><input type=\"number\" name=\"deviceId\" min=\"0\" max=\"255\" value=\"0\" required>";
  page += "<label>Write Key (16 characters)</label><input type=\"text\" name=\"writeKey\" minlength=\"16\" maxlength=\"16\" required>";
  page += "<button type=\"submit\">Save &amp; Reboot</button>";
  page += "</form></body></html>";
  return page;
}

void handleRoot() { server.send(200, "text/html", buildFormPage()); }

void handleSave() {
  SensorNodeConfig config;
  config.ssid = server.arg("ssid");
  config.password = server.arg("password");
  config.nodeName = server.arg("nodeName");
  config.deviceId = (uint8_t)constrain(server.arg("deviceId").toInt(), 0, 255);
  config.writeKey = server.arg("writeKey");

  if (config.ssid.length() == 0) {
    server.send(400, "text/html", "<p>Wi-Fi network is required. <a href=\"/\">Back</a></p>");
    return;
  }
  if (config.writeKey.length() != 16) {
    server.send(400, "text/html", "<p>Write key must be exactly 16 characters. <a href=\"/\">Back</a></p>");
    return;
  }

  saveSensorNodeConfig(config);
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

void runSensorNodeSetupPortal() {
  WiFi.mode(WIFI_AP_STA);

  String apName = "SensorNode-Setup-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
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
