#pragma once

#include <Arduino.h>

// What a sketch with a display needs to know about the network, pushed to it whenever it
// changes (SensorNode::onStatus()). The library has no display dependency of its own; the
// sketch decides how to draw this (the example sketches' bottom OLED row: address, down time
// and a Wi-Fi icon).
struct SensorNodeStatus {
  bool connected = false;      // associated with a Wi-Fi network
  bool portal = false;         // the setup portal's access point is up
  IPAddress ip;                // portal ? the access point's address : the LAN address (0.0.0.0 if none)
  int8_t rssi = 0;             // dBm while connected
  uint32_t downSeconds = 0;    // time without a connection (0 while connected)

  // 1 (weak) to 3 (strong) while connected, 0 when not.
  uint8_t level() const {
    if (!connected) return 0;
    return rssi > -60 ? 3 : (rssi > -72 ? 2 : 1);
  }
};

typedef void (*SensorNodeStatusFn)(const SensorNodeStatus &status);

// Registers the callback (nullptr removes it). Register before checkFirmwareVersion()/begin()
// so the blocking setup portals report too.
void setSensorNodeStatusCallback(SensorNodeStatusFn fn);

// Library-internal: hands status to the callback, but only when something a display would show
// changed (connected, portal, address, signal level, or the down time at the coarseness it is
// shown: every 5 s for the first minute, then every minute).
void publishSensorNodeStatus(const SensorNodeStatus &status);
