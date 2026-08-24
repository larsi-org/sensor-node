#include "SensorNode.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <sys/time.h>
#include <time.h>

#include <cctype>

#include "SensorNodePortal.h"

namespace {

const char *kServer = "larsi.org";

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

// "Go Daddy Root Certificate Authority - G2", self-signed, valid to
// 2037-12-31 -- the root larsi.org's chain currently validates against
// (verified 2026-08-13: `openssl s_client -connect larsi.org:443
// -showcerts`, chain leaf -> "Go Daddy Secure Certificate Authority -
// G2" -> this root; fetched from
// https://certs.godaddy.com/repository/gdroot-g2.crt). If larsi.org
// ever switches certificate providers, TLS connections will start
// failing and this constant needs updating to match.
const char kServerRootCA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoT
EUdvRGFkZHkuY29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRp
ZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5MDkwMTAwMDAwMFoXDTM3MTIzMTIz
NTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6b25hMRMwEQYDVQQH
EwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8GA1UE
AxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIw
DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKD
E6bFIEMBO4Tx5oVJnyfq9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH
/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD+qK+ihVqf94Lw7YZFAXK6sOoBJQ7Rnwy
DfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutdfMh8+7ArU6SSYmlRJQVh
GkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMlNAJWJwGR
tDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEA
AaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE
FDqahQcQZyi27/a9BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmX
WWcDYfF+OwYxdS2hII5PZYe096acvNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu
9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r5N9ss4UXnT3ZJE95kTXWXwTr
gIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYVN8Gb5DKj7Tjo
2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO
LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI
4uJEvlz36hz1
-----END CERTIFICATE-----
)EOF";

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

// Shared by log() and provision(): connects, writes body as a
// form-encoded POST to path, and returns the raw response (empty
// string on connect failure).
String postToServer(IPAddress serverIp, const char *path, const String &body) {
  WiFiClientSecure client;
  client.setHandshakeTimeout(15);  // seconds; default is 120
  if (!client.connect(serverIp, 443, kServer, kServerRootCA, nullptr, nullptr) &&
      !client.connect(serverIp, 443, kServer, kServerRootCA, nullptr, nullptr)) {
    char err[128];
    client.lastError(err, sizeof(err));
    Serial.printf("[SensorNode] TLS connect failed: %s\n", err);
    return String();
  }

  String request = "POST " + String(path) + " HTTP/1.1\r\n";
  request += String("Host: ") + kServer + "\r\n";
  request += "Content-Type: application/x-www-form-urlencoded\r\n";
  request += "Content-Length: " + String(body.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += body;

  String response = rawHttpRequest(client, request, 5000);
  client.stop();
  return response;
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
  bool synced = ntpQuery("162.159.200.1", 5000) || ntpQuery("216.239.35.0", 5000);
  Serial.printf("[SensorNode] Time sync %s (epoch %ld)\n", synced ? "OK" : "FAILED", (long)time(nullptr));
}

}  // namespace

void SensorNode::begin(unsigned long connectTimeoutMs) {
  bool connected = loadSensorNodeConfig(config_);

  if (connected) {
    String hostname = sanitizeHostname(config_.deviceName);
    if (hostname.length() > 0) {
      WiFi.setHostname(hostname.c_str());
    }
    WiFi.mode(WIFI_STA);

    connected = false;
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
  }

  if (!connected) {
    Serial.println("[SensorNode] Not connected -- starting setup portal.");
    runSensorNodeSetupPortal();  // never returns; device restarts on success
  }

  Serial.printf("[SensorNode] Connected, IP %s\n", WiFi.localIP().toString().c_str());
  resolveServerIp();  // best-effort here; log() retries later if this fails
  syncTime();
}

void SensorNode::resetConfig() { clearSensorNodeConfig(); }

void SensorNode::openPortal() { runSensorNodeSetupPortal(); }

void SensorNode::checkFirmwareVersion(uint32_t version) {
  if (!firmwareVersionChanged(version)) return;
  Serial.printf("[SensorNode] Firmware version changed (now %lu) -- opening setup portal.\n",
                (unsigned long)version);
  runSensorNodeSetupPortal(version);  // never returns; reboots on save
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
  if (serverIp_ != IPAddress()) return true;
  Serial.printf("[SensorNode] Resolving %s...\n", kServer);
  if (!WiFi.hostByName(kServer, serverIp_)) {
    Serial.printf("[SensorNode] Could not resolve %s\n", kServer);
    serverIp_ = IPAddress();
    return false;
  }
  Serial.printf("[SensorNode] Resolved %s -> %s\n", kServer, serverIp_.toString().c_str());
  return true;
}

bool SensorNode::log(const std::vector<float> &values, int decimalPlaces) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!resolveServerIp()) return false;

  String data;
  for (size_t i = 0; i < values.size(); i++) {
    if (i > 0) data += ",";
    if (!isnan(values[i])) data += String(values[i], decimalPlaces);
  }

  String body = "key=" + config_.writeKey + "&device=" + String(config_.deviceId) + "&data=" + data;
  String response = postToServer(serverIp_, "/sensors/log", body);

  Serial.printf("[SensorNode] POST /sensors/log (device=%d, data=%s):\n%s\n", config_.deviceId, data.c_str(), response.c_str());

  return response.indexOf("Data logged") >= 0;
}

bool SensorNode::provision(const std::vector<SensorNodeChannel> &channels) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!resolveServerIp()) return false;

  String channelList;
  for (size_t i = 0; i < channels.size(); i++) {
    if (i > 0) channelList += "|";
    channelList += String(channels[i].id) + "," + channels[i].sensor + "," + channels[i].property + "," + channels[i].unit;
  }

  String body = "key=" + config_.writeKey + "&device=" + String(config_.deviceId) +
                "&name=" + config_.deviceName + "&channels=" + channelList;
  String response = postToServer(serverIp_, "/sensors/provision", body);

  Serial.printf("[SensorNode] POST /sensors/provision:\n%s\n", response.c_str());

  return response.indexOf("Provisioned") >= 0;
}
