#pragma once

#include <Wire.h>

#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>

// Wraps the SparkFun MAX1704x library for the MAX17048 fuel gauge built
// into the board itself on the hardware this library targets (e.g. the
// SparkFun ESP32-C6 Thing Plus's onboard battery-gauge Qwiic connection --
// see CLAUDE.md's strapping-pin note). Unlike a plug-in sensor such as
// BME280, this chip is part of every board in the family rather than
// something wired up per-sketch, so it lives here instead of being
// copy-pasted into each example.
//
// Channel 15 -- the last of the 16 a device gets -- is reserved sitewide
// for battery state of charge (see README.md), leaving 0-14 free for
// whatever the sketch actually measures.
class SensorNodeBattery {
 public:
  static const uint8_t kSocChannel = 15;

  // Connects to the onboard MAX17048 and runs quickStart() (SparkFun's
  // library recommends this once per boot for a better initial SOC guess).
  // Returns false if the chip doesn't respond, in which case readSOC()
  // always returns NAN -- a board without this chip just silently skips
  // channel 15 rather than needing separate handling in the sketch.
  bool begin(TwoWire &wirePort = Wire);

  // State of charge, 0-100% (clamped -- the MAX17048 itself can report
  // slightly over 100%, especially while charging). NAN if begin() wasn't
  // called or failed.
  float readSOC();

 private:
  SFE_MAX1704X gauge_{MAX1704X_MAX17048};
  bool ready_ = false;
};
