// BME280 sensor node: temperature, dew point, humidity, and pressure
// on channels 0-3, plus onboard battery state of charge on channel 15.
//
// Requires the "SparkFun BME280" library (Arduino Library Manager:
// search "SparkFun BME280"). Wire the breakout to the board's
// Qwiic/I2C connector (SDA/SCL) -- default I2C address 0x77.
//
// See BasicNode.ino for the setup-portal walkthrough (first boot, the
// SensorNode-Setup-XXXXXX access point, and the reset-button hold
// behavior) -- identical here.

#include <Adafruit_SSD1306.h>
#include <SensorNode.h>
#include <SensorNodeBattery.h>
#include <SparkFunBME280.h>
#include <Wire.h>

// Optional Adafruit FeatherWing OLED (128x64, SSD1306, I2C address 0x3C --
// shares the Qwiic bus with the BME280/MAX17048). Detected at boot via
// oled.begin()'s return value, same pattern as bme.beginI2C()/battery.begin()
// below -- if it's not physically connected, oledPresent stays false and
// every display call in loop() is skipped entirely.
const int kScreenWidth = 128;
const int kScreenHeight = 64;
const uint8_t kOledAddress = 0x3C;
Adafruit_SSD1306 oled(kScreenWidth, kScreenHeight, &Wire, -1);
bool oledPresent = false;
String oledTitle;  // deviceName, else a hardcoded fallback -- set in setup()

// Same reset pin and rationale as BasicNode.ino.
const int kResetPin = 2;

// Bump to force the setup portal open once on the next boot, without touching the reset
// button/jumper -- see checkFirmwareVersion(). Currently 4 to force one portal visit on
// batcave's device 0 so its name can be retyped to include the location baked in (e.g.
// "sensor-node-basement") now that the separate Location field has been merged back into
// Device Name -- see CLAUDE.md/README.md.
const uint32_t kFirmwareVersion = 4;

// Channel 0: temperature C, 1: dew point C, 2: humidity %, 3: pressure
// hPa, 15: battery state of charge % (SensorNodeBattery::kSocChannel --
// reserved sitewide, see the library's README.md). See BasicNode.ino
// for when this actually gets posted (needsProvisioning()). Order matters beyond that: both
// log() and the OLED text below index into this by position (kChannels[0].decimalPlaces,
// kChannels[0].unit, etc.) instead of a second list, so it has to stay temp/dew point/humidity/
// pressure/SOC in this order.
//
// decimalPlaces (trailing field, defaults to 2 if omitted -- see SensorNodeChannel) matches
// each sensor's actual accuracy rather than its raw register resolution: both chips report far
// more digits than their datasheet accuracy backs up -- BME280 is +-0.5C / +-3%RH / +-1hPa
// (temp/dew point/humidity/pressure), MAX17048 is roughly +-1% SOC. provision() never reads
// this field; it's here purely for log()/the OLED below to share.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "BME280", "Temperature", "C", 1},
    {1, "BME280", "Dew Point Temperature", "C", 1},
    {2, "BME280", "Relative Humidity", "%", 0},
    {3, "BME280", "Pressure", "hPa", 1},
    {SensorNodeBattery::kSocChannel, "MAX17048", "State of Charge", "%", 0},
};

SensorNode node;
BME280 bme;
SensorNodeBattery battery;

void setup() {
  Serial.begin(115200);
  delay(1000);

  node.checkFirmwareVersion(kFirmwareVersion);
  node.checkPortalButton(kResetPin);
  node.begin();
  if (node.needsProvisioning()) node.provision(kChannels);

  const SensorNodeConfig &config = node.config();
  oledTitle = config.deviceName.length() > 0 ? config.deviceName : "sensor-node: BME280";

  Wire.begin();
  if (!bme.beginI2C()) {
    Serial.println("BME280 not detected -- check wiring.");
  }
  if (!battery.begin()) {
    Serial.println("MAX17048 not detected -- battery channel will read NAN.");
  }
  oledPresent = oled.begin(SSD1306_SWITCHCAPVCC, kOledAddress);
  if (!oledPresent) {
    Serial.println("OLED not detected -- skipping display.");
  } else {
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
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
  float ch0 = bme.readTempC();                 // temperature C
  float ch2 = bme.readFloatHumidity();         // humidity %
  float ch3 = bme.readFloatPressure() / 100.0; // pressure hPa
  float ch1 = bme.dewPointC();                 // dew point C
  float ch15 = battery.readSOC();               // battery state of charge %

  // Channels 4-14 are unused on this node -- log() addresses each entry by
  // position (see SensorNode::log()), so they still need to be present as
  // NAN to hold channel 15's slot rather than shifting it down to 4.
  node.log({{ch0, kChannels[0].decimalPlaces}, {ch1, kChannels[1].decimalPlaces},
            {ch2, kChannels[2].decimalPlaces}, {ch3, kChannels[3].decimalPlaces},
            NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN,
            {ch15, kChannels[4].decimalPlaces}});

  // "Current values" text screen -- entirely skipped if the OLED wasn't
  // detected at boot, so this is a no-op on a board with none wired up.
  if (oledPresent) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.println(oledTitle);
    // String(value, precision) + kChannels[i].unit here, not printf's %.Nf plus a hardcoded
    // "C"/"%"/"hPa", so the displayed precision/unit can't drift from what log() above and
    // provision() actually use -- all three read the same kChannels row. The (unsigned int)
    // cast matches ESP32 core's String(float, unsigned int) constructor exactly; a uint8_t
    // straight from kChannels[i].decimalPlaces is otherwise ambiguous against WString.h's other
    // explicit String(..., unsigned char) overloads.
    oled.println("Temp:  " + String(ch0, (unsigned int)kChannels[0].decimalPlaces) + " " + kChannels[0].unit);
    oled.println("Dew:   " + String(ch1, (unsigned int)kChannels[1].decimalPlaces) + " " + kChannels[1].unit);
    oled.println("Humid: " + String(ch2, (unsigned int)kChannels[2].decimalPlaces) + " " + kChannels[2].unit);
    oled.println("Press: " + String(ch3, (unsigned int)kChannels[3].decimalPlaces) + " " + kChannels[3].unit);
    oled.println("Batt:  " + String(ch15, (unsigned int)kChannels[4].decimalPlaces) + " " + kChannels[4].unit);
    oled.display();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
