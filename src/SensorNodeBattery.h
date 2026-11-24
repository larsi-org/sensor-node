#pragma once

#include <Wire.h>

#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>

#include "SensorNode.h"

// Wraps the SparkFun MAX1704x library for the MAX17048 fuel gauge built
// into the board itself on the hardware this library targets (e.g. the
// SparkFun ESP32-C6 Thing Plus's onboard battery-gauge Qwiic connection --
// see CLAUDE.md's strapping-pin note). Unlike a plug-in sensor such as
// BME280, this chip is part of every board in the family rather than
// something wired up per-sketch, so it lives here instead of being
// copy-pasted into each example.
//
// Reading the battery and logging it are separate: a sketch with a display
// reads it for its battery icon either way, and logs it only if it chooses to
// (each example's kLogBattery, off by default: on a node powered over USB the
// value barely moves and was rarely worth a channel) and begin() found the
// meter. The examples call begin() before provision(), so a board without the
// meter never registers the battery channel either. When it is logged, it is
// channel 15, the last of the 16 a device gets; when it isn't, channel 15 is
// free for a real sensor.
class SensorNodeBattery {
 public:
  static const uint8_t kSocChannel = 15;

  // The channel entry to add to provision()'s and log()'s list when the
  // battery is logged: channel 15, MAX17048, "State of Charge" in %, shown as
  // "Batt" with no decimals, marked battery so log() doesn't count it as a
  // sensor reading.
  static SensorNodeChannel channel() {
    return {kSocChannel, "MAX17048", "State of Charge", "%", "Batt", 0, true};
  }

  // Connects to the onboard MAX17048 and runs quickStart() (SparkFun's
  // library recommends this once per boot for a better initial SOC guess).
  // Returns false if the chip doesn't respond, in which case readSOC()
  // always returns NAN: the examples then show no battery icon and leave
  // the battery channel out of provision() and log().
  bool begin(TwoWire &wirePort = Wire);

  // State of charge, 0-100% (clamped -- the MAX17048 itself can report
  // slightly over 100%, especially while charging). NAN if begin() wasn't
  // called or failed.
  float readSOC();

 private:
  SFE_MAX1704X gauge_{MAX1704X_MAX17048};
  bool ready_ = false;
};
