#include "SensorNode.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "SensorNodePortal.h"

namespace {

const char *kServer = "larsi.org";

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

// WiFiClientSecure checks the pinned cert's validity dates against the
// device clock, which boots near the epoch -- without this, every TLS
// connection fails with HTTPC_ERROR_CONNECTION_REFUSED before a single
// byte is sent, since the cert looks "not yet valid" until synced.
void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[SensorNode] Waiting for NTP time sync");
  time_t now = time(nullptr);
  unsigned long start = millis();
  while (now < 1700000000 && millis() - start < 10000) {
    delay(250);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();
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
  client.setCACert(kServerRootCA);

  HTTPClient http;
  String url = String("https://") + kServer + "/log.php";
  if (!http.begin(client, url)) {
    Serial.println("[SensorNode] http.begin() failed");
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "key=" + config_.writeKey + "&data=" + data;
  int status = http.POST(body);
  String response = http.getString();
  http.end();

  Serial.printf("[SensorNode] POST %s (%s) -> %d\n", url.c_str(), data.c_str(), status);
  Serial.println(response);

  return status == 200 && response.indexOf("Data logged") >= 0;
}
