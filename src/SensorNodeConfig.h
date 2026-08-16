#pragma once

#include <Arduino.h>

struct SensorNodeConfig {
  // A small history of known networks, most-recently-added first, so
  // a node that moves between a handful of locations (e.g. home and a
  // second home) reconnects immediately without reprovisioning every
  // time it moves back. Unused slots have an empty ssid.
  static const uint8_t kMaxNetworks = 3;
  String ssids[kMaxNetworks];
  String passwords[kMaxNetworks];

  String deviceName;
  uint8_t deviceId = 0;
  String writeKey;  // location.WriteKey is varchar(16) -- always exactly 16 chars

  // How often the sketch should call log(), in minutes. Not enforced
  // by this library -- sketches read it themselves (config().
  // logIntervalMinutes) to compute their own loop() delay. Portal
  // restricts entry to SensorNodePortal.cpp's kLogIntervals set.
  uint8_t logIntervalMinutes = 3;

  bool isComplete() const { return ssids[0].length() > 0 && writeKey.length() == 16; }
};

// Reads saved settings from NVS. Returns config.isComplete().
bool loadSensorNodeConfig(SensorNodeConfig &config);

void saveSensorNodeConfig(const SensorNodeConfig &config);

// Erases saved settings so the next loadSensorNodeConfig() call reports
// incomplete, sending begin() back to the setup portal.
void clearSensorNodeConfig();
