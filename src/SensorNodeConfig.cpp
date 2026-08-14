#include "SensorNodeConfig.h"

#include <Preferences.h>

namespace {
const char *kNamespace = "sensornode";
}

bool loadSensorNodeConfig(SensorNodeConfig &config) {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  config.ssid = prefs.getString("ssid", "");
  config.password = prefs.getString("password", "");
  config.nodeName = prefs.getString("nodeName", "");
  config.deviceId = prefs.getUChar("deviceId", 0);
  config.writeKey = prefs.getString("writeKey", "");
  prefs.end();
  return config.isComplete();
}

void saveSensorNodeConfig(const SensorNodeConfig &config) {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putString("ssid", config.ssid);
  prefs.putString("password", config.password);
  prefs.putString("nodeName", config.nodeName);
  prefs.putUChar("deviceId", config.deviceId);
  prefs.putString("writeKey", config.writeKey);
  prefs.end();
}

void clearSensorNodeConfig() {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.clear();
  prefs.end();
}
