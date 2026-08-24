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
// for when this actually gets posted (needsProvisioning()). Both log() and printReadings()
// (below, for the OLED) zip this positionally against a values list, so it has to stay
// temp/dew point/humidity/pressure/SOC in this order -- reordering it without updating loop()'s
// calls to match would silently misfile a reading onto the wrong channel, not just mislabel it
// on screen.
//
// label (short enough for the OLED -- property is meant for the server/reports and runs long,
// e.g. "Dew Point Temperature") and decimalPlaces (matching each sensor's actual accuracy
// rather than its raw register resolution -- both chips report far more digits than their
// datasheet accuracy backs up: BME280 is +-0.5C / +-3%RH / +-1hPa temp/dew point/humidity/
// pressure, MAX17048 is roughly +-1% SOC) are never read by provision(); they're here purely
// for printReadings()/log() below to share.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "BME280", "Temperature", "C", "Temp", 1},
    {1, "BME280", "Dew Point Temperature", "C", "Dew", 1},
    {2, "BME280", "Relative Humidity", "%", "Humid", 0},
    {3, "BME280", "Pressure", "hPa", "Press", 1},
    {SensorNodeBattery::kSocChannel, "MAX17048", "State of Charge", "%", "Batt", 0},
};

// Prints one OLED line per value, e.g. "Temp:  21.5 C", zipped positionally against kChannels
// the same way log() is (values[i] labeled/rounded/unit-ed from kChannels[i]) -- pass fewer
// values than kChannels to show only the first several rather than slicing kChannels itself.
const size_t kLabelColumnWidth = 7;

void printReadings(const std::vector<float> &values) {
  for (size_t i = 0; i < values.size() && i < kChannels.size(); i++) {
    const SensorNodeChannel &channel = kChannels[i];
    String label = String(channel.label) + ":";
    while (label.length() < kLabelColumnWidth) label += " ";
    // (unsigned int) cast: ESP32 core's String(float, unsigned int) is otherwise ambiguous
    // against its other explicit String(..., unsigned char) overloads when given a uint8_t
    // directly -- neither is a strictly better match across both arguments.
    oled.println(label + String(values[i], (unsigned int)channel.decimalPlaces) + " " + channel.unit);
  }
}

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

  std::vector<float> readings = {ch0, ch1, ch2, ch3, ch15};

  // Zipped positionally against kChannels -- picks up each entry's id/decimalPlaces from there.
  // No need to pad channels 4-14 by hand; log() fills the gap up to channel 15 itself.
  node.log(kChannels, readings);

  // "Current values" text screen -- entirely skipped if the OLED wasn't
  // detected at boot, so this is a no-op on a board with none wired up.
  if (oledPresent) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.println(oledTitle);
    // Reads label/decimalPlaces/unit straight from kChannels, so the OLED can't drift from
    // what log() above and provision() actually use -- all three read the same rows.
    printReadings(readings);
    oled.display();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
