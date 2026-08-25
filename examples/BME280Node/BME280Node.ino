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

#include <Adafruit_SH110X.h>
#include <SensorNode.h>
#include <SensorNodeBattery.h>
#include <SparkFunBME280.h>
#include <Wire.h>

// Optional Adafruit FeatherWing OLED (128x64, SH1107, I2C address 0x3C --
// shares the Qwiic bus with the BME280/MAX17048). Detected at boot via
// oled.begin()'s return value, same pattern as bme.beginI2C()/battery.begin()
// below -- if it's not physically connected, oledPresent stays false and
// every display call in loop() is skipped entirely. SH1107's native
// orientation is portrait (tall x wide) -- constructor takes height then
// width, and setRotation(1) below rotates it back to landscape.
const int kScreenWidth = 128;
const int kScreenHeight = 64;
const uint8_t kOledAddress = 0x3C;
Adafruit_SH1107 oled(kScreenHeight, kScreenWidth, &Wire);
bool oledPresent = false;
String oledTitle;  // deviceName, else a hardcoded fallback -- set in setup()

// Same reset pin and rationale as BasicNode.ino.
const int kResetPin = 2;

// Bump to force the setup portal open once on the next boot, without touching the reset
// button/jumper -- see checkFirmwareVersion(). Currently 5 to force one portal visit on
// this board once it's repurposed as device 1 (a new board is taking over as device 0),
// so its device id can be retyped from 0 to 1 without a factory reset.
const uint32_t kFirmwareVersion = 5;

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

// Top-line battery gauge, right-aligned. kTitleWidth (6px/char at text size 1) leaves the last
// 2 characters' worth of pixels overlapping the icon's kBatteryIconWidth-pixel corner -- fine,
// since that area gets cleared and redrawn whenever the icon actually draws (see loop()); when
// it doesn't (no fuel gauge), those 2 extra characters of device name show instead of being
// reserved-and-wasted.
const size_t kTitleWidth = 21;
const int16_t kBatteryIconWidth = 14;

// A 13x7 outline + 1px nub, matching ~/Pictures/bat.png's hand-drawn reference icon (decoded
// pixel-by-pixel rather than redrawn from memory) -- 5 fill bars at x=2,4,6,8,10, gaps at
// x=3,5,7,9 always empty, drawn instead of blitted since the shape is simple enough that this
// is fewer lines than embedding a bitmap.
void drawBatteryIcon(int16_t x, int16_t y, uint8_t barsLit) {
  oled.drawRect(x, y, 13, 7, SH110X_WHITE);
  oled.drawFastVLine(x + 13, y + 2, 3, SH110X_WHITE);
  for (uint8_t i = 0; i < barsLit && i < 5; i++) {
    oled.fillRect(x + 2 + 2 * i, y + 2, 1, 3, SH110X_WHITE);
  }
}

SensorNode node;
BME280 bme;
SensorNodeBattery battery;

void setup() {
  Serial.begin(115200);
  delay(1000);

  node.checkFirmwareVersion(kFirmwareVersion);
  node.checkPendingCommand();
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
  oledPresent = oled.begin(kOledAddress, true);
  if (!oledPresent) {
    Serial.println("OLED not detected -- skipping display.");
  } else {
    oled.setRotation(1);
    oled.setTextColor(SH110X_WHITE);
    oled.setTextSize(1);
    // Show the device name right away rather than leaving the display in whatever it powered
    // on with for the full settle delay below -- loop() (and its clearDisplay()) doesn't run
    // until after that delay, up to 3 minutes. No battery icon yet -- readSOC() isn't called
    // until loop() either.
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print(oledTitle.substring(0, kTitleWidth));
    oled.display();
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
  float ch0  = bme.readTempC();                 // temperature C
  float ch2  = bme.readFloatHumidity();         // humidity %
  float ch3  = bme.readFloatPressure() / 100.0; // pressure hPa
  float ch1  = bme.dewPointC();                 // dew point C
  float ch15 = battery.readSOC();               // battery state of charge %

  std::vector<float> readings = {ch0, ch1, ch2, ch3, ch15};

  // Zipped positionally against kChannels -- picks up each entry's id/decimalPlaces from there.
  // No need to pad channels 4-14 by hand; log() fills the gap up to channel 15 itself.
  node.log(kChannels, readings);
  node.applyPendingCommand();

  // "Current values" text screen -- entirely skipped if the OLED wasn't
  // detected at boot, so this is a no-op on a board with none wired up.
  if (oledPresent) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print(oledTitle.substring(0, kTitleWidth));
    // No icon at all (not an empty/0-bar one) when the fuel gauge isn't present -- (int)NAN
    // isn't safe to feed into the bars-lit math below, and a 0-bar icon would misleadingly
    // read as "battery dead" instead of "no battery".
    if (!isnan(ch15)) {
      // (soc+10)/20 in integer math is round-to-nearest-20% (equivalent to
      // floor(soc/20 + 0.5)), not floor(soc/20) -- floor alone would read as empty until 20%
      // and never show a full icon until exactly 100%. constrain() clamps the fuel gauge's
      // occasional >100% overshoot.
      uint8_t barsLit = (uint8_t)constrain(((int)ch15 + 10) / 20, 0, 5);
      // kTitleWidth's last couple characters can land under this corner -- wipe it before
      // drawing, since drawBatteryIcon()'s outline/bars only ever set foreground pixels and
      // would otherwise leave stray text pixels showing through the icon's gaps.
      oled.fillRect(kScreenWidth - kBatteryIconWidth, 0, kBatteryIconWidth, 8, SH110X_BLACK);
      drawBatteryIcon(kScreenWidth - kBatteryIconWidth, 0, barsLit);
    }
    oled.setCursor(0, 8);
    // Reads label/decimalPlaces/unit straight from kChannels, so the OLED can't drift from
    // what log() above and provision() actually use -- all three read the same rows.
    printReadings(readings);
    oled.display();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
