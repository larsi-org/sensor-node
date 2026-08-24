// Minimal example: reports two placeholder channels on the configured
// log frequency. Replace temperatureC/humidityPct below with real
// sensor reads.
//
// On first boot (or whenever it can't connect), the node opens an
// access point named "SensorNode-Setup-XXXXXX" -- join it and visit
// http://192.168.4.1/ to pick a Wi-Fi network and enter this device's
// name, id, write key, and log frequency. It saves and reboots
// automatically.
//
// Hold the button at boot to reach the portal on demand: a short hold
// opens it pre-filled, without erasing anything (for tweaking one
// field, e.g. the device name); a longer hold wipes the saved config
// first, so the portal comes up blank instead (see
// checkPortalButton()'s wipeHoldMs).
//
// The portal also opens automatically, without touching the button,
// whenever kFirmwareVersion below doesn't match what the device last
// booted with -- see checkFirmwareVersion().

#include <SensorNode.h>

// Hold this pin low at boot to reach the setup portal on demand (e.g.
// wire a button, or briefly jumper it to GND) -- short hold opens it
// pre-filled, long hold wipes first (see checkPortalButton()'s default
// wipeHoldMs). GPIO2 -- see CLAUDE.md for why this pin specifically.
// Change to match your board's free pins, or delete the check below
// entirely if you don't need it.
const int kResetPin = 2;

// Bump this to force the setup portal open once on the next boot -- e.g. after adding a new
// config field to the portal you want existing devices to fill in, without needing physical
// access to the reset button. See checkFirmwareVersion().
const uint32_t kFirmwareVersion = 1;

// Matches the log() call below. Only actually posted when
// needsProvisioning() is true (i.e. the setup portal just saved
// settings) -- see setup() below. Replace with the real channels for
// your sketch.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "YourSensor", "Temperature", "C"},
    {1, "YourSensor", "Humidity", "%"},
};

SensorNode node;

void setup() {
  Serial.begin(115200);
  delay(1000);

  node.checkFirmwareVersion(kFirmwareVersion);
  node.checkPortalButton(kResetPin);
  node.begin();
  if (node.needsProvisioning()) node.provision(kChannels);
}

void loop() {
  float temperatureC = 21.5;  // TODO: replace with a real sensor read
  float humidityPct = NAN;    // NAN skips this channel entirely

  // Each entry defaults to 2 decimal places (SensorNodeReading) -- override per entry, e.g.
  // {temperatureC, 1}, to match your real sensor's actual accuracy instead of over-reporting.
  // See examples/BME280Node for a worked example.
  node.log({temperatureC, humidityPct});
  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
