// Minimal example: reports two placeholder channels every 60s. Wire
// your real sensor reads in where readChannel0()/readChannel1() are
// used below.
//
// On first boot (or whenever it can't connect), the node opens an
// access point named "SensorNode-Setup-XXXX" -- join it and visit
// http://192.168.4.1/ to pick a Wi-Fi network and enter this node's
// name, device id, and write key. It saves and reboots automatically.

#include <SensorNode.h>

// Hold this pin low at boot to force reconfiguration (e.g. wire a
// button to GND). Change to match your board, or delete the check
// below entirely if you don't need it.
const int kResetPin = 9;

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
}

void loop() {
  float temperatureC = 21.5;  // TODO: replace with a real sensor read
  float humidityPct = NAN;    // NAN skips this channel entirely

  node.log({temperatureC, humidityPct});
  delay(60000);
}
