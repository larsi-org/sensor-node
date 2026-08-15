#pragma once

#include <Arduino.h>

#include <vector>

#include "SensorNodeConfig.h"

class SensorNode {
 public:
  // Loads saved settings and tries each known network in turn
  // (SensorNodeConfig::kMaxNetworks, most-recently-added first) --
  // connectTimeoutMs applies per network, so the worst-case time
  // before falling back is that times the number of known networks.
  // If nothing is saved, or none connect, this instead runs the AP
  // setup portal and never returns -- the device restarts once the
  // user submits valid settings, then reconnects on the next boot.
  void begin(unsigned long connectTimeoutMs = 15000);

  // Erases saved settings so the next begin() call falls back to the
  // setup portal. Call before begin(), e.g. when a button is held at
  // boot -- see examples/BasicNode.
  void resetConfig();

  // Posts one reading per channel, addressed starting at this node's
  // configured device id (channel deviceId*256 + index). A NAN entry
  // is omitted from the request, matching log.php's "blank value"
  // skip semantics. Returns true once the server confirms the data
  // was logged.
  bool log(const std::vector<float> &values, int decimalPlaces = 2);

  const SensorNodeConfig &config() const { return config_; }

 private:
  // Resolves and caches the server's IP; re-resolves on demand if
  // never successful yet (e.g. DNS wasn't up at boot). Cached rather
  // than re-resolved every call so a still-flaky network doesn't hit
  // the DNS resolution path -- historically the least reliable part of
  // this stack -- any more often than necessary.
  bool resolveServerIp();

  SensorNodeConfig config_;
  IPAddress serverIp_;
};
