// Minimal example: reports two placeholder channels on the configured
// log frequency. Replace temperatureC/humidityPct below with real
// sensor reads.
//
// On first boot (or whenever it can't connect), the node opens an
// access point named "SensorNode-Setup-XXXX" -- join it and visit
// http://192.168.4.1/ to pick a Wi-Fi network and enter this device's
// name, id, write key, and log frequency. It saves and reboots
// automatically.
//
// Hold the button at boot to reach the portal on demand: a short hold
// opens it pre-filled, without erasing anything (for tweaking one
// field, e.g. the device name); a longer hold wipes the saved config
// first, so the portal comes up blank instead (see
// checkPortalButton()'s wipeHoldMs).

#include <SensorNode.h>

// Hold this pin low at boot to reach the setup portal on demand (e.g.
// wire a button, or briefly jumper it to GND) -- short hold opens it
// pre-filled, long hold wipes first (see checkPortalButton()'s default
// wipeHoldMs). GPIO0 avoids this board's reserved pins: 4/5/8/9/15
// are chip strapping pins (9 is also the onboard BOOT button -- see
// CLAUDE.md), 12/13 are USB D-/D+ (same USB-JTAG used for flashing/
// serial), 6/7/11/18-23 are tied to onboard Qwiic/battery-gauge/
// microSD/RGB LED. Change to match your board's free pins, or delete
// the check below entirely if you don't need it.
const int kResetPin = 0;

// Matches the log() call below. Sent once at boot; the provision
// endpoint only fills in rows that don't exist yet, so this is safe to
// leave in place permanently. Replace with the real channels for your sketch.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "YourSensor", "Temperature", "C"},
    {1, "YourSensor", "Humidity", "%"},
};

SensorNode node;

void setup() {
  Serial.begin(115200);
  delay(1000);

  node.checkPortalButton(kResetPin);
  node.begin();
  node.provision(kChannels);
}

void loop() {
  float temperatureC = 21.5;  // TODO: replace with a real sensor read
  float humidityPct = NAN;    // NAN skips this channel entirely

  node.log({temperatureC, humidityPct});
  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
