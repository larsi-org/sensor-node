// Minimal example: reports two placeholder channels on the configured
// log frequency. Replace temperatureC/humidityPct below with real
// sensor reads.
//
// On first boot (or whenever it can't connect), the node opens an
// access point named "SensorNode-Setup-XXXX" -- join it and visit
// http://192.168.4.1/ to pick a Wi-Fi network and enter this device's
// name, id, write key, and log frequency. It saves and reboots
// automatically.

#include <SensorNode.h>

// Hold this pin low at boot to force reconfiguration (e.g. wire a
// button to GND). Change to match your board, or delete the check
// below entirely if you don't need it.
const int kResetPin = 9;

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
    Serial.println("Reset pin held low -- clearing saved config.");
    node.resetConfig();
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
