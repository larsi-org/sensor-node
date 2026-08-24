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
  uint8_t logIntervalMinutes = 5;

  bool isComplete() const { return ssids[0].length() > 0 && writeKey.length() == 16; }
};

// Reads saved settings from NVS. Returns config.isComplete().
bool loadSensorNodeConfig(SensorNodeConfig &config);

void saveSensorNodeConfig(const SensorNodeConfig &config);

// Erases saved settings so the next loadSensorNodeConfig() call reports
// incomplete, sending begin() back to the setup portal.
void clearSensorNodeConfig();

// Compares `version` against the value stored from the last boot -- read-only, doesn't write
// anything. Returns true if they differ, including the very first boot of a version-checking
// sketch, since "nothing stored yet" (0) counts as different from any real version.
bool firmwareVersionChanged(uint32_t version);

// Stores `version` as the last-seen firmware version. Deliberately separate from
// firmwareVersionChanged() (a pure comparison) -- call this only once the portal's settings
// have actually been submitted (see SensorNodePortal.h's pendingFirmwareVersion), not just
// because a mismatch was detected, so a reset while the portal sits open unconfigured doesn't
// silently consume the pending visit.
void markFirmwareVersionSeen(uint32_t version);
