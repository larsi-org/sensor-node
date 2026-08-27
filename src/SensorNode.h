#pragma once

#include <Arduino.h>

#include <vector>

#include "SensorNodeConfig.h"

// One channel's identity, shared by provision() and log() -- id is the 0-15 offset within
// this device (channel deviceId*16 + id is what the server actually addresses), sensor is the
// physical hardware's model name (e.g. "BME280", "DS18B20"), property/unit are short human
// labels (e.g. "Temperature", "C") -- all stored server-side the first time this channel is
// seen, then left alone. label/decimalPlaces are never sent to provision() (which only reads
// id/sensor/property/unit) -- log() reads decimalPlaces to format each value it's given (see
// below); label is unused by this library at all, it's just there so a sketch with a display
// has one place per channel to hold a short form of property (which is meant for the
// server/reports and can run long, e.g. "Dew Point Temperature" vs. a 21-char OLED line).
// Both default (label to "", decimalPlaces to 1 -- most hobby-grade sensors' real accuracy
// backs up one decimal place, not two) so a sketch that doesn't care about either can leave
// them unset.
struct SensorNodeChannel {
  uint8_t id;
  const char *sensor;
  const char *property;
  const char *unit;
  const char *label = "";
  uint8_t decimalPlaces = 1;
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

  // Checks for a command left over from a previous log() call (see log()'s doc comment below)
  // and, if it's "open_portal" -- the only value this owns -- clears it and opens the setup
  // portal (never returning). A no-op for anything else, including a command this build simply
  // doesn't recognize: that's not this method's call to make, and clearing it here would erase
  // it before a sketch checking pendingCommand()/clearPendingCommand() itself (SensorNodeConfig.h
  // -- e.g. for its own command not built into this library) ever got a chance to see it. Call
  // this once at the top of setup(), alongside checkFirmwareVersion()/checkPortalButton() and
  // before begin() -- see examples/BasicNode.
  //
  // This is the "next boot" half of the mechanism: applyPendingCommand() (below) is what
  // actually triggers the reboot that brings execution back here with something to act on.
  void checkPendingCommand();

  // Buffers values, zipped positionally against channels -- values[i] is channels[i]'s reading,
  // using that entry's id (for wire position, within this node's configured device id: channel
  // deviceId*16 + id) and decimalPlaces (for rounding). channels is typically the same list
  // passed to provision(). A NAN entry skips that channel (matching the log endpoint's "blank
  // value" semantics). values may be shorter than channels to report only the first several
  // (e.g. skip a channel a display also truncates to) -- anything past values.size() just isn't
  // sent.
  //
  // Positional, not id-keyed, so values has to list channels[0]'s reading first, channels[1]'s
  // second, etc. -- reordering channels without updating every values list built against it
  // would silently misfile data onto the wrong channel.
  //
  // Every call queues its reading in a fixed 64-entry RTC-memory ring buffer first,
  // unconditionally -- so a call never loses data, even if this wake doesn't also flush (below)
  // or the flush fails. Only every config().reportEveryCycles-th call actually attempts to send
  // anything, batching everything queued since the last confirmed flush into one request (see
  // https://larsi.org/sensors/sensor-node.php's Batched Reports). reportEveryCycles == 1 (the
  // default) flushes every call, same as before buffering existed. A queued-but-not-this-wake
  // call returns true (queued, nothing was supposed to send yet); a flush wake returns true only
  // once the server confirms the batch was logged -- a false return leaves everything queued for
  // the next flush attempt to retry, combined with whatever's accumulated since. The ring holds
  // only the most recent 64 unflushed readings -- a longer outage silently evicts the oldest.
  //
  // A successful flush's response may also carry a one-shot test command (a "Command: ..." line
  // -- see https://larsi.org/sensors/sensor-node.php), which gets persisted for
  // applyPendingCommand()/checkPendingCommand() to act on -- log() itself never acts on it.
  bool log(const std::vector<SensorNodeChannel> &channels, const std::vector<float> &values);

  // Acts on a command log() persisted on this or an earlier call, if it's one this method
  // owns. "scan_i2c" runs immediately, right here (no restart needed -- see scanI2CBus()) and
  // clears the flag itself; "open_portal" restarts the device instead, without clearing the
  // flag, so checkPendingCommand() (at the top of the next setup()) is what actually consumes
  // it. Anything else -- including nothing pending, or a command this library doesn't
  // recognize at all -- is left completely untouched: a sketch with its own commands can check
  // pendingCommand()/clearPendingCommand() itself (SensorNodeConfig.h) right after this call,
  // e.g. to run a 1-Wire scan without forcing that library's dependency on every sketch that
  // uses SensorNode. Call this right after log() in loop() -- see examples/BasicNode.
  void applyPendingCommand();

  // True if the setup portal saved settings since the last confirmed provision() call -- gate
  // provision() on this rather than calling it every boot (see examples/BasicNode): the portal
  // is the only thing that changes what needs registering (device name/id, or a sketch update
  // adding a channel), so a normal reconnect-only boot has nothing new to report. Valid after
  // begin() returns.
  bool needsProvisioning() const;

  // Registers this device and its channels with the server. The provision endpoint is a
  // non-empty upsert (an empty field here never overwrites a value set by hand server-side, but
  // a real one does update the row), so it's harmless to call outside needsProvisioning() too --
  // just redundant once already confirmed. Call after begin(), guarded by needsProvisioning(),
  // before the first log(). Returns true once the server confirms, which also clears
  // needsProvisioning() for next boot; a false return (no connectivity, server error) leaves it
  // set so the next boot's begin() gets another attempt.
  bool provision(const std::vector<SensorNodeChannel> &channels);

  // POSTs key/device (the same auth every other endpoint uses) to the server's "config"
  // endpoint and returns just the response body -- empty on any failure (not connected, DNS,
  // TLS connect, or a non-200 status; there's no way to tell "empty file" from "request failed"
  // from the return value alone, but no caller has needed that distinction yet). POST, not GET,
  // so the API key never ends up in the web server's access log -- same reason log()/
  // provision() are POST-only. Unlike those two, this doesn't touch NVS or any persisted state
  // -- a plain fetch for a sketch that needs its own server-hosted config beyond what
  // provision()'s fixed fields cover (e.g. a probe ROM-ID layout too free-form to fit as channel
  // data). Call after begin() has connected.
  String fetchConfig();

  const SensorNodeConfig &config() const { return config_; }

 private:
  // Parses config_.serverUrl into serverHost_/serverBasePath_, then resolves and caches
  // serverHost_'s IP; re-resolves on demand only if never successful yet (e.g. DNS wasn't up
  // at boot). The parse itself is cheap and re-done every call (config_.serverUrl never
  // changes at runtime -- only a portal save + reboot can change it -- so this is just to keep
  // the two derived fields next to the resolution that depends on them).
  bool resolveServerIp();

  SensorNodeConfig config_;
  IPAddress serverIp_;
  String serverHost_;
  String serverBasePath_ = "/";  // e.g. "/sensors/" -- log()/provision() append "log"/"provision"
};
