#include "SensorNodeConfig.h"

#include <Preferences.h>

namespace {
const char *kNamespace = "sensornode";
}

bool loadSensorNodeConfig(SensorNodeConfig &config) {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  for (uint8_t i = 0; i < SensorNodeConfig::kMaxNetworks; i++) {
    config.ssids[i] = prefs.getString(("ssid" + String(i)).c_str(), "");
    config.passwords[i] = prefs.getString(("password" + String(i)).c_str(), "");
  }
  config.deviceName = prefs.getString("deviceName", "");
  config.deviceLocation = prefs.getString("deviceLocation", "");
  config.deviceId = prefs.getUChar("deviceId", 0);
  config.writeKey = prefs.getString("writeKey", "");
  config.logIntervalMinutes = prefs.getUChar("logInterval", 3);
  prefs.end();
  return config.isComplete();
}

void saveSensorNodeConfig(const SensorNodeConfig &config) {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  for (uint8_t i = 0; i < SensorNodeConfig::kMaxNetworks; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), config.ssids[i]);
    prefs.putString(("password" + String(i)).c_str(), config.passwords[i]);
  }
  prefs.putString("deviceName", config.deviceName);
  prefs.putString("deviceLocation", config.deviceLocation);
  prefs.putUChar("deviceId", config.deviceId);
  prefs.putString("writeKey", config.writeKey);
  prefs.putUChar("logInterval", config.logIntervalMinutes);
  prefs.end();
}

void clearSensorNodeConfig() {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.clear();
  prefs.end();
}
