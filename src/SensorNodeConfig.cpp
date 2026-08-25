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
  config.deviceId = prefs.getUChar("deviceId", 0);
  config.writeKey = prefs.getString("writeKey", "");
  config.logIntervalMinutes = prefs.getUChar("logInterval", 5);
  config.reportEveryCycles = prefs.getUChar("reportEvery", 1);
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
  prefs.putUChar("deviceId", config.deviceId);
  prefs.putString("writeKey", config.writeKey);
  prefs.putUChar("logInterval", config.logIntervalMinutes);
  prefs.putUChar("reportEvery", config.reportEveryCycles);
  // Purges the retired deviceLocation key on any device that still has one saved from before
  // it was merged back into deviceName -- a no-op (remove() on an absent key just returns
  // false) once a device has already saved since this change.
  prefs.remove("deviceLocation");
  prefs.end();
}

void clearSensorNodeConfig() {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.clear();
  prefs.end();
}

bool firmwareVersionChanged(uint32_t version) {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  uint32_t stored = prefs.getUInt("fwVersion", 0);
  prefs.end();
  return stored != version;
}

void markFirmwareVersionSeen(uint32_t version) {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putUInt("fwVersion", version);
  prefs.end();
}

bool provisionPending() {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  bool pending = prefs.getBool("provPending", false);
  prefs.end();
  return pending;
}

void markProvisionPending() {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putBool("provPending", true);
  prefs.end();
}

void clearProvisionPending() {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putBool("provPending", false);
  prefs.end();
}

String pendingCommand() {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  String command = prefs.getString("cmd", "");
  prefs.end();
  return command;
}

void setPendingCommand(const String &command) {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putString("cmd", command);
  prefs.end();
}

void clearPendingCommand() {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putString("cmd", "");
  prefs.end();
}
