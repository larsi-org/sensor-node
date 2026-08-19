// BME280 sensor node: temperature, dew point, humidity, and pressure
// on channels 0-3.
//
// Requires the "SparkFun BME280" library (Arduino Library Manager:
// search "SparkFun BME280"). Wire the breakout to the board's
// Qwiic/I2C connector (SDA/SCL) -- default I2C address 0x77.
//
// See BasicNode.ino for the setup-portal walkthrough (first boot, the
// SensorNode-Setup-XXXXXX access point, and the reset-button hold
// behavior) -- identical here.

#include <SensorNode.h>
#include <SparkFunBME280.h>
#include <Wire.h>

// Same reset pin and rationale as BasicNode.ino.
const int kResetPin = 2;

// Channel 0: temperature C, 1: dew point C, 2: humidity %, 3: pressure
// hPa. See BasicNode.ino for why this list is safe to leave in place
// permanently (provision() semantics).
const std::vector<SensorNodeChannel> kChannels = {
    {0, "BME280", "Temperature", "C"},
    {1, "BME280", "Dew Point Temperature", "C"},
    {2, "BME280", "Relative Humidity", "%"},
    {3, "BME280", "Pressure", "hPa"},
};

SensorNode node;
BME280 bme;

void setup() {
  Serial.begin(115200);
  delay(1000);

  node.checkPortalButton(kResetPin);
  node.begin();
  node.provision(kChannels);

  Wire.begin();
  if (!bme.beginI2C()) {
    Serial.println("BME280 not detected -- check wiring.");
  }

  // The very first reading right after boot occasionally comes back as a
  // fixed garbage value (seen in production: identical bogus temp/humidity/
  // pressure on two separate boots) -- likely the sensor or I2C bus not yet
  // settled amid WiFi connect/provisioning right beforehand. Give it one
  // full log interval to settle, capped at 3 minutes so a long-interval
  // device doesn't sit dark for its whole first cycle.
  delay(min(node.config().logIntervalMinutes, (uint8_t)3) * 60UL * 1000);
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
