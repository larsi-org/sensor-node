// BME280 sensor node: temperature, dew point, humidity, and pressure
// on channels 0-3.
//
// Requires the "SparkFun BME280" library (Arduino Library Manager:
// search "SparkFun BME280"). Wire the breakout to the board's
// Qwiic/I2C connector (SDA/SCL) -- default I2C address 0x77.
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
#include <SparkFunBME280.h>
#include <Wire.h>

// Hold this pin low at boot to reach the setup portal on demand (e.g.
// wire a button to GND). Change to match your board, or delete the
// check below entirely if you don't need it.
const int kResetPin = 9;

// How long the pin must stay held low to trigger a full wipe instead
// of just opening the portal (see setup() below).
const unsigned long kWipeHoldMs = 5000;

// Matches the log() call below -- channel 0: temperature C, 1: dew
// point C, 2: humidity %, 3: pressure hPa. Sent once at boot;
// provision.php only fills in rows that don't exist yet, so this is
// safe to leave in place permanently.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "Temperature", "C"},
    {1, "Dew Point Temperature", "C"},
    {2, "Relative Humidity", "%"},
    {3, "Pressure", "hPa"},
};

SensorNode node;
BME280 bme;

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

  Wire.begin();
  if (!bme.beginI2C()) {
    Serial.println("BME280 not detected -- check wiring.");
  }
}

void loop() {
  float tempC = bme.readTempC();
  float humidityPct = bme.readFloatHumidity();
  float pressureHpa = bme.readFloatPressure() / 100.0;
  float dewPointTempC = bme.dewPointC();

  // channel 0: temperature C, 1: dew point C, 2: humidity %, 3: pressure hPa
  node.log({tempC, dewPointTempC, humidityPct, pressureHpa});

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
