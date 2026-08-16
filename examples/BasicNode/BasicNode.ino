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
// field, e.g. the device name); a hold past kWipeHoldMs wipes the
// saved config first, so the portal comes up blank instead.

#include <SensorNode.h>

// Hold this pin low at boot to reach the setup portal on demand (e.g.
// wire a button to GND). Change to match your board, or delete the
// check below entirely if you don't need it.
const int kResetPin = 9;

// How long the pin must stay held low to trigger a full wipe instead
// of just opening the portal (see setup() below).
const unsigned long kWipeHoldMs = 5000;

// Matches the log() call below. Sent once at boot; provision.php only
// fills in rows that don't exist yet, so this is safe to leave in
// place permanently. Replace with the real channels for your sketch.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "Temperature", "C"},
    {1, "Humidity", "%"},
};

SensorNode node;

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(kResetPin, INPUT_PULLUP);
  if (digitalRead(kResetPin) == LOW) {
    unsigned long heldStart = millis();
    while (digitalRead(kResetPin) == LOW && millis() - heldStart < kWipeHoldMs) {
      delay(50);
    }
    if (millis() - heldStart >= kWipeHoldMs) {
      Serial.println("Held long -- wiping saved config.");
      node.resetConfig();  // begin() below falls straight into the portal
    } else {
      Serial.println("Held short -- opening portal (nothing erased).");
      node.openPortal();  // never returns
    }
  }

  node.begin();
  node.provision(kChannels);
}

void loop() {
  float temperatureC = 21.5;  // TODO: replace with a real sensor read
  float humidityPct = NAN;    // NAN skips this channel entirely

  node.log({temperatureC, humidityPct});
  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
