#pragma once

#include <Arduino.h>

#include <vector>

#include "SensorNodeConfig.h"

// One channel's identity for provision() -- id is the 0-15 offset
// within this device (matches the position log() addresses that
// channel at), sensor is the physical hardware's model name (e.g.
// "BME280", "DS18B20"), property/unit are short human labels (e.g.
// "Temperature", "C") -- all stored server-side the first time this
// channel is seen, then left alone.
struct SensorNodeChannel {
  uint8_t id;
  const char *sensor;
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

  // Compares version against what was stored on the last boot that actually submitted the
  // setup portal. If it's different -- including the very first boot of a version-checking
  // sketch -- opens the setup portal (pre-filled, nothing erased, same as a short
  // checkPortalButton() press), never returning, so a firmware update always gets one chance
  // to revisit settings (e.g. a newly added config field) before falling through to normal
  // logging. version itself is only stored once the portal is actually submitted, not just
  // because this opened it -- a reset or power loss while it's sitting open unconfigured
  // leaves the stored value untouched, so the next boot offers the portal again instead of
  // wrongly assuming it already happened. A no-op if version already matches. Call once at the
  // top of setup(), before checkPortalButton()/begin() -- see examples/BasicNode/examples/
  // BME280Node.
  void checkFirmwareVersion(uint32_t version);

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
  // configured device id (channel deviceId*16 + index). A NAN entry
  // is omitted from the request, matching the log endpoint's "blank value"
  // skip semantics. Returns true once the server confirms the data
  // was logged.
  bool log(const std::vector<float> &values, int decimalPlaces = 2);

  // Registers this device and its channels with the server -- safe to call every boot: the
  // provision endpoint is a non-empty upsert, so an empty field here never overwrites a value
  // set by hand server-side, but a real one does update the row. Call once after begin(),
  // before the first log(). Returns true once the server confirms.
  bool provision(const std::vector<SensorNodeChannel> &channels);

  const SensorNodeConfig &config() const { return config_; }

 private:
  // Resolves and caches the server's IP; re-resolves on demand only if
  // never successful yet (e.g. DNS wasn't up at boot).
  bool resolveServerIp();

  SensorNodeConfig config_;
  IPAddress serverIp_;
};
