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

  // How many log() calls SensorNode::log() batches into its RTC ring buffer before actually
  // flushing to the server -- sent to provision() so the server can size its own alerting
  // tolerance accordingly (see this repo's CLAUDE.md/README.md for the ring buffer design). 1
  // (the default) means every call flushes immediately, matching this library's original
  // unbuffered behavior.
  uint8_t reportEveryCycles = 1;

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

// Whether the setup portal has saved settings since the last confirmed provision() call --
// read-only, doesn't write anything. Set by markProvisionPending() (SensorNodePortal.cpp's
// handleSave()), cleared by clearProvisionPending() (SensorNode::provision(), only on a
// confirmed server response). A normal boot that just reconnects with already-known
// credentials never touches either, so this stays false across it.
bool provisionPending();

// Marks that the portal just saved settings, so the next successful provision() should
// actually run. Call this from handleSave(), after saveSensorNodeConfig() -- covers both a
// brand-new device's first-ever setup and any later reconfiguration (device name, id, etc. may
// have changed either way).
void markProvisionPending();

// Clears the pending flag. Call this only once provision() gets a confirmed response --
// leaving it set on failure (no connectivity, server error, etc.) means the next boot tries
// again instead of silently dropping the registration.
void clearProvisionPending();

// A one-shot test command delivered via a successful log() response's "Command: ..." line (see
// SensorNode::checkPendingCommand()/applyPendingCommand()) -- read-only, doesn't write anything.
// Free functions rather than a SensorNodeConfig field: checkPendingCommand() needs to read this
// at the top of setup(), before begin() has ever called loadSensorNodeConfig(), the same reason
// firmwareVersionChanged() above is a free function too.
//
// These three functions are deliberately public API, not internal to SensorNode -- both
// checkPendingCommand() and applyPendingCommand() only ever touch a value they specifically
// recognize (currently "open_portal" and "scan_i2c") and leave everything else alone, so a
// sketch is free to check pendingCommand() for its own command name and call
// clearPendingCommand() itself once handled. That's how a sketch-specific dependency (e.g. a
// 1-Wire scan needing the OneWire library) can use this same mailbox without forcing that
// dependency onto every sketch that uses SensorNode -- see CLAUDE.md.
String pendingCommand();

// Persists a newly-received command, overwriting any not-yet-applied one (last one delivered
// wins). Call this from SensorNode::log() once a response actually carries one.
void setPendingCommand(const String &command);

// Clears the pending command. checkPendingCommand()/applyPendingCommand() call this only once
// they've actually acted on a command they own; a sketch handling its own command name should
// do the same once it's done. A command nothing ever claims (a typo, or one from firmware that
// predates it) is left to accumulate rather than self-clearing -- see CLAUDE.md.
void clearPendingCommand();
