#pragma once

#include <Arduino.h>

#include <vector>

#include "SensorNodeConfig.h"

// One channel's identity for provision() -- id is the 0-255 offset
// within this device (matches the position log() addresses that
// channel at), property/unit are short human labels (e.g.
// "Temperature", "C") stored server-side the first time this channel
// is seen, then left alone.
struct SensorNodeChannel {
  uint8_t id;
  const char *property;
  const char *unit;
};

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

  // Opens the setup portal directly, without erasing anything --
  // pre-filled from whatever's already saved, same as begin()'s
  // automatic fallback when no known network connects. Use this for an
  // on-demand "edit config" trigger (e.g. a short button hold) where
  // resetConfig()'s full wipe would be overkill -- see
  // examples/BasicNode. Never returns; the device restarts once the
  // user submits.
  void openPortal();

  // Configures pin as INPUT_PULLUP and, if held low at call time,
  // blocks measuring how long: held past wipeHoldMs calls
  // resetConfig() (so begin() below falls into the portal blank);
  // held less than that calls openPortal() directly (pre-filled,
  // nothing erased) and never returns. A no-op if pin reads high. Call
  // once at the top of setup(), before begin() -- see examples/
  // BasicNode and examples/BME280Node for the wiring (a button to
  // GND).
  void checkPortalButton(uint8_t pin, unsigned long wipeHoldMs = 5000);

  // Posts one reading per channel, addressed starting at this node's
  // configured device id (channel deviceId*256 + index). A NAN entry
  // is omitted from the request, matching log.php's "blank value"
  // skip semantics. Returns true once the server confirms the data
  // was logged.
  bool log(const std::vector<float> &values, int decimalPlaces = 2);

  // Registers this device and its channels with the server -- safe to
  // call every boot, since provision.php only creates rows that don't
  // already exist yet (anything renamed by hand afterward is left
  // alone). Call once after begin(), before the first log(). Returns
  // true once the server confirms.
  bool provision(const std::vector<SensorNodeChannel> &channels);

  const SensorNodeConfig &config() const { return config_; }

 private:
  // Resolves and caches the server's IP; re-resolves on demand only if
  // never successful yet (e.g. DNS wasn't up at boot).
  bool resolveServerIp();

  SensorNodeConfig config_;
  IPAddress serverIp_;
};
