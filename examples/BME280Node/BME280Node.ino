// BME280 sensor node: temperature, dew point, humidity, and pressure
// on channels 0-3.
//
// Requires the "SparkFun BME280" library (Arduino Library Manager:
// search "SparkFun BME280"). Wire the breakout to the board's
// Qwiic/I2C connector (SDA/SCL) -- default I2C address 0x77.
//
// On first boot (or whenever it can't connect), the node opens an
// access point named "SensorNode-Setup-XXXX" -- join it and visit
// http://192.168.4.1/ to pick a Wi-Fi network and enter this node's
// name, device id, write key, and log frequency. It saves and reboots
// automatically.

#include <SensorNode.h>
#include <SparkFunBME280.h>
#include <Wire.h>

// Hold this pin low at boot to force reconfiguration (e.g. wire a
// button to GND). Change to match your board, or delete the check
// below entirely if you don't need it.
const int kResetPin = 9;

SensorNode node;
BME280 bme;

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(kResetPin, INPUT_PULLUP);
  if (digitalRead(kResetPin) == LOW) {
    Serial.println("Reset pin held low -- clearing saved config.");
    node.resetConfig();
  }

  node.begin();

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
