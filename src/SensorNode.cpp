#include "SensorNode.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "SensorNodePortal.h"

namespace {

const char *kServer = "larsi.org";

// Hardcoded rather than resolved at runtime: WiFi.hostByName() (used
// for any hostname-based connect()) reliably hung or crashed on this
// board/core during development, tracing back to
// NetworkManager::hostByName() not taking the TCPIP core lock lwIP
// requires (github.com/espressif/arduino-esp32 issue #10526). Update
// this if larsi.org's IP ever changes (symptom: connect() fails) --
// check with `dig +short larsi.org` or `getent hosts larsi.org`. The
// hostname is still passed to connect() below for TLS SNI/certificate
// verification and the HTTP Host header, so this doesn't weaken either
// of those checks.
const IPAddress kServerIp(107, 180, 118, 157);

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
// Deliberately not using HTTPClient here: constructing an HTTPClient,
// calling addHeader(), etc. between connect() and actually writing the
// request took long enough that the connection was reliably closed out
// from under us before a single byte went out -- confirmed on hardware
// by writing the request immediately after connect() instead, which
// works every time. Writing the request as one pre-built String
// immediately after connect() keeps that gap as small as possible.
String rawHttpRequest(WiFiClientSecure &client, const String &request, unsigned long timeoutMs) {
  client.print(request);
  String response;
  unsigned long start = millis();
  while (millis() - start < timeoutMs && (client.connected() || client.available())) {
    while (client.available()) response += (char)client.read();
  }
  return response;
}

String extractHeader(const String &response, const char *name) {
  String marker = String("\r\n") + name + ": ";
  int start = response.indexOf(marker);
  if (start < 0) return "";
  start += marker.length();
  int end = response.indexOf("\r\n", start);
  return end < 0 ? "" : response.substring(start, end);
}

// Sets the system clock from an ordinary HTTPS response's Date header,
// rather than NTP: raw NTP over WiFiUDP reliably never received a
// reply to anything it sent on this board/core (esp32:esp32, SparkFun
// ESP32-C6 Thing Plus), confirmed down to a same-subnet LAN round trip
// with no DNS involved -- while TCP has been reliable. This first
// request's response isn't authenticated (setInsecure()) -- but
// nothing secret changes hands here, only a timestamp used so the
// *next* connection (the real one, in log() below) can validate
// certificate dates correctly. Worst case for a forged Date response
// is that log()'s real, fully-validated HTTPS connection fails the
// same way it would with no clock set at all -- it can't leak the
// write key or accept a forged larsi.org cert, since that connection
// still checks the pinned CA and hostname independent of whatever this
// step set the clock to.
bool syncTimeFromHttpDate() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);
  // One retry: ordinary WiFi/AP flakiness is common enough on battery/
  // roaming-free embedded nodes to be worth one immediate retry before
  // giving up for this boot.
  if (!client.connect(kServerIp, 443) && !client.connect(kServerIp, 443)) {
    Serial.println("[SensorNode] Time sync: connect failed");
    return false;
  }

  String request = String("GET / HTTP/1.1\r\nHost: ") + kServer + "\r\nConnection: close\r\n\r\n";
  String response = rawHttpRequest(client, request, 5000);
  client.stop();

  String dateHeader = extractHeader(response, "Date");
  if (dateHeader.length() == 0) {
    Serial.println("[SensorNode] Time sync: no Date header in response");
    return false;
  }

  struct tm tm = {};
  // RFC 7231 preferred HTTP-date format, e.g. "Sun, 06 Nov 1994 08:49:37 GMT"
  if (strptime(dateHeader.c_str(), "%a, %d %b %Y %H:%M:%S %Z", &tm) == nullptr) {
    Serial.println("[SensorNode] Time sync: couldn't parse Date header");
    return false;
  }

  // No timegm() in this toolchain; force UTC so mktime() doesn't apply
  // a local-time offset -- HTTP dates are always GMT.
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t epoch = mktime(&tm);

  struct timeval tv = {};
  tv.tv_sec = epoch;
  settimeofday(&tv, nullptr);
  return true;
}

// WiFiClientSecure checks the pinned cert's validity dates against the
// device clock, which boots near the epoch -- without this, every TLS
// connection fails since the cert looks "not yet valid" until synced.
void syncTime() {
  bool synced = syncTimeFromHttpDate();
  Serial.printf("[SensorNode] Time sync %s (epoch %ld)\n", synced ? "OK" : "FAILED", (long)time(nullptr));
}

}  // namespace

void SensorNode::begin(unsigned long connectTimeoutMs) {
  bool connected = loadSensorNodeConfig(config_);

  if (connected) {
    if (config_.nodeName.length() > 0) {
      WiFi.setHostname(config_.nodeName.c_str());
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(config_.ssid.c_str(), config_.password.c_str());

    Serial.printf("[SensorNode] Connecting to \"%s\"...\n", config_.ssid.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < connectTimeoutMs) {
      delay(250);
    }
    connected = (WiFi.status() == WL_CONNECTED);
  }

  if (!connected) {
    Serial.println("[SensorNode] Not connected -- starting setup portal.");
    runSensorNodeSetupPortal();  // never returns; device restarts on success
  }

  Serial.printf("[SensorNode] Connected, IP %s\n", WiFi.localIP().toString().c_str());
  syncTime();
}

void SensorNode::resetConfig() { clearSensorNodeConfig(); }

bool SensorNode::log(const std::vector<float> &values, int decimalPlaces) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String data = String(config_.deviceId) + "|";
  for (size_t i = 0; i < values.size(); i++) {
    if (i > 0) data += ",";
    if (!isnan(values[i])) data += String(values[i], decimalPlaces);
  }

  WiFiClientSecure client;
  client.setHandshakeTimeout(15);  // seconds; default is 120
  if (!client.connect(kServerIp, 443, kServer, kServerRootCA, nullptr, nullptr) &&
      !client.connect(kServerIp, 443, kServer, kServerRootCA, nullptr, nullptr)) {
    char err[128];
    client.lastError(err, sizeof(err));
    Serial.printf("[SensorNode] TLS connect failed: %s\n", err);
    return false;
  }

  String body = "key=" + config_.writeKey + "&data=" + data;
  String request = "POST /log.php HTTP/1.1\r\n";
  request += String("Host: ") + kServer + "\r\n";
  request += "Content-Type: application/x-www-form-urlencoded\r\n";
  request += "Content-Length: " + String(body.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += body;

  String response = rawHttpRequest(client, request, 5000);
  client.stop();

  Serial.printf("[SensorNode] POST /log.php (%s):\n%s\n", data.c_str(), response.c_str());

  return response.indexOf("Data logged") >= 0;
}
