#pragma once

#include <Arduino.h>

struct SensorNodeConfig {
  String ssid;
  String password;
  String nodeName;
  uint8_t deviceId = 0;
  String writeKey;

  bool isComplete() const { return ssid.length() > 0 && writeKey.length() > 0; }
};

// Reads saved settings from NVS. Returns config.isComplete().
bool loadSensorNodeConfig(SensorNodeConfig &config);

void saveSensorNodeConfig(const SensorNodeConfig &config);

// Erases saved settings so the next loadSensorNodeConfig() call reports
// incomplete, sending begin() back to the setup portal.
void clearSensorNodeConfig();
