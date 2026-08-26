// 1-Wire bus bench-testing node -- no 1-Wire sensor reading logic yet, just enough to confirm
// a device is actually answering on the bus before building real DS18B20 support. OLED +
// battery gauge wiring below is duplicated from BME280Node.ino rather than shared (see the
// library's CLAUDE.md/README on why) -- keep the two in sync by hand if either changes.
//
// See BasicNode.ino for the setup-portal walkthrough (first boot, the SensorNode-Setup-XXXXXX
// access point, and the reset-button hold behavior) -- identical here.
//
// The "scan_1wire" pending command (see the library's Notes section on
// pendingCommand()/clearPendingCommand()) sweeps the bus and prints whatever ROM codes answer,
// straight to Serial -- a bench-testing aid only, same treatment as the base library's own
// "scan_i2c", but implemented here rather than in SensorNode itself: 1-Wire needs the
// third-party OneWire library, and unlike Wire/I2C that doesn't ship with the ESP32 core, so
// baking it into the shared library would force that dependency onto every sketch using
// SensorNode, not just ones with a 1-Wire bus wired up. Set it with, e.g.:
//   UPDATE device SET pending_command='scan_1wire' WHERE prefix='...' AND device_id=...;
//
// NOTE: the Arduino Library Manager's published OneWire (2.3.8) doesn't compile on ESP32-C6 at
// all -- its fast-GPIO path has no C6 case. The fix landed on GitHub's main branch in June 2025
// but isn't in a tagged release yet, so install it from GitHub directly:
//   git clone https://github.com/PaulStoffregen/OneWire.git ~/Arduino/libraries/OneWire
// (re-check whether a new release exists before repeating this workaround later).

#include <Adafruit_SH110X.h>
#include <OneWire.h>
#include <SensorNode.h>
#include <SensorNodeBattery.h>
#include <Wire.h>

// Optional Adafruit FeatherWing OLED -- see BME280Node.ino for the full rationale (same
// hardware, same detection-at-boot treatment: oledPresent stays false and every display call
// in loop() is skipped if it's not physically connected).
const int kScreenWidth = 128;
const int kScreenHeight = 64;
const uint8_t kOledAddress = 0x3C;
Adafruit_SH1107 oled(kScreenHeight, kScreenWidth, &Wire);
bool oledPresent = false;
String oledTitle;  // deviceName, else a hardcoded fallback -- set in setup()

// Same reset pin and rationale as BasicNode.ino.
const int kResetPin = 2;

// Bump this to force the setup portal open once on the next boot -- see checkFirmwareVersion().
const uint32_t kFirmwareVersion = 1;

// GPIO3 -- the only other pin free of every strapping/peripheral reservation on this board
// family, alongside GPIO2 (kResetPin above) -- see the library's CLAUDE.md. 1-Wire is
// bit-banged (an external ~4.7k pull-up to 3.3V on the data line), not a hardware peripheral
// like I2C, so there's no dedicated/default pin the way Wire has one -- any free GPIO works.
const int kOneWirePin = 3;
OneWire oneWire(kOneWirePin);

// Channel 0: a placeholder, not a real reading -- just enough to keep log() actually logging
// something each cycle. Without at least one non-NAN value, the server never considers the
// request successful, and a pending command only ever gets delivered on a successful log()
// response -- see sensors/log.php's Command:/pending_command docs. Channel 15: onboard battery
// state of charge (SensorNodeBattery::kSocChannel -- reserved sitewide, see the library's
// README.md), same as BME280Node.ino. Both log() and printReadings() below zip this
// positionally against a values list, so it has to stay placeholder/SOC in this order.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "Placeholder", "Heartbeat", "", "HB", 0},
    {SensorNodeBattery::kSocChannel, "MAX17048", "State of Charge", "%", "Batt", 0},
};

// Prints one OLED line per value -- see BME280Node.ino, identical treatment.
const size_t kLabelColumnWidth = 7;

void printReadings(const std::vector<float> &values) {
  for (size_t i = 0; i < values.size() && i < kChannels.size(); i++) {
    const SensorNodeChannel &channel = kChannels[i];
    String label = String(channel.label) + ":";
    while (label.length() < kLabelColumnWidth) label += " ";
    oled.println(label + String(values[i], (unsigned int)channel.decimalPlaces) + " " + channel.unit);
  }
}

// Top-line battery gauge -- see BME280Node.ino, identical treatment.
const size_t kTitleWidth = 21;
const int16_t kBatteryIconWidth = 14;

void drawBatteryIcon(int16_t x, int16_t y, uint8_t barsLit) {
  oled.drawRect(x, y, 13, 7, SH110X_WHITE);
  oled.drawFastVLine(x + 13, y + 2, 3, SH110X_WHITE);
  for (uint8_t i = 0; i < barsLit && i < 5; i++) {
    oled.fillRect(x + 2 + 2 * i, y + 2, 1, 3, SH110X_WHITE);
  }
}

// Sweeps every device on the bus, returning a comma-joined list of 8-byte ROM codes (each as
// 16 hex digits, e.g. "2800000000000000") or "none". DS18B20s have family code 0x28 (the ROM
// code's first byte) -- not filtered on here, since this is meant to show whatever's actually
// answering, not assume what it is.
String scanOneWireBus() {
  String result;
  byte addr[8];
  oneWire.reset_search();
  while (oneWire.search(addr)) {
    if (result.length() > 0) result += ",";
    for (uint8_t i = 0; i < 8; i++) {
      if (addr[i] < 0x10) result += "0";
      result += String(addr[i], HEX);
    }
  }
  return result.length() > 0 ? result : "none";
}

SensorNode node;
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
  oledTitle = config.deviceName.length() > 0 ? config.deviceName : "sensor-node: 1-Wire";

  Wire.begin();
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
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print(oledTitle.substring(0, kTitleWidth));
    oled.display();
  }

  delay(60UL * 1000);
}

void loop() {
  float ch0 = 1;                    // placeholder -- see kChannels above
  float ch15 = battery.readSOC();   // battery state of charge %

  std::vector<float> readings = {ch0, ch15};

  node.log(kChannels, readings);
  node.applyPendingCommand();  // handles open_portal/scan_i2c if either ever shows up here too

  // "scan_1wire" isn't something SensorNode knows about -- see the file header comment --
  // so it's checked and cleared right here instead.
  if (pendingCommand() == "scan_1wire") {
    Serial.printf("1-Wire scan (pin %d): %s\n", kOneWirePin, scanOneWireBus().c_str());
    clearPendingCommand();
  }

  if (oledPresent) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print(oledTitle.substring(0, kTitleWidth));
    if (!isnan(ch15)) {
      uint8_t barsLit = (uint8_t)constrain(((int)ch15 + 10) / 20, 0, 5);
      oled.fillRect(kScreenWidth - kBatteryIconWidth, 0, kBatteryIconWidth, 8, SH110X_BLACK);
      drawBatteryIcon(kScreenWidth - kBatteryIconWidth, 0, barsLit);
    }
    oled.setCursor(0, 8);
    printReadings(readings);
    oled.display();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
