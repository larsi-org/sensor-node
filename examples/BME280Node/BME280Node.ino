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
String oledTitle;  // deviceLocation, else deviceName, else a hardcoded fallback -- set in setup()

// Same reset pin and rationale as BasicNode.ino.
const int kResetPin = 2;

// Bump to force the setup portal open once on the next boot, without touching the reset
// button/jumper -- see checkFirmwareVersion(). Currently 3 to force one portal visit on
// batcave's device 0 to fill in Location (added after this device was first provisioned).
const uint32_t kFirmwareVersion = 3;

// Channel 0: temperature C, 1: dew point C, 2: humidity %, 3: pressure
// hPa, 15: battery state of charge % (SensorNodeBattery::kSocChannel --
// reserved sitewide, see the library's README.md). See BasicNode.ino
// for why this list is safe to leave in place permanently (provision()
// semantics).
const std::vector<SensorNodeChannel> kChannels = {
    {0, "BME280", "Temperature", "C"},
    {1, "BME280", "Dew Point Temperature", "C"},
    {2, "BME280", "Relative Humidity", "%"},
    {3, "BME280", "Pressure", "hPa"},
    {SensorNodeBattery::kSocChannel, "MAX17048", "State of Charge", "%"},
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
  node.provision(kChannels);

  const SensorNodeConfig &config = node.config();
  oledTitle = config.deviceLocation.length() > 0 ? config.deviceLocation
              : config.deviceName.length() > 0   ? config.deviceName
                                                  : "sensor-node: BME280";

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
  node.log({ch0, ch1, ch2, ch3,
            NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN,
            ch15});

  // "Current values" text screen -- entirely skipped if the OLED wasn't
  // detected at boot, so this is a no-op on a board with none wired up.
  if (oledPresent) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.println(oledTitle);
    oled.printf("Temp:  %.1f C\n", ch0);
    oled.printf("Dew:   %.1f C\n", ch1);
    oled.printf("Humid: %.1f %%\n", ch2);
    oled.printf("Press: %.1f hPa\n", ch3);
    oled.printf("Batt:  %.1f %%\n", ch15);
    oled.display();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
