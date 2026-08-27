// DS18B20 (1-Wire) temperature node -- 5 probes on one bus, from an old strand originally wired
// up for CCubes_DataLogger/batcave_temperature5.pde (see kKnownProbes below; same ROM codes,
// carried over so the probes keep their physical identity). OLED (device name + battery icon,
// same treatment duplicated from BME280Node.ino) is back as of 2026-08-26 -- it's on the I2C
// Qwiic bus, independent of the bit-banged 1-Wire probe bus, so there's no conflict between the
// two. Per-reading text (printReadings() in BME280Node.ino) is intentionally NOT duplicated here
// yet -- this sketch's measurements will get their own, different on-screen layout later.
//
// Requires the "DallasTemperature" library (Arduino Library Manager: search
// "DallasTemperature", by Miles Burton) -- built on top of OneWire, handles the actual DS18B20
// conversion/scratchpad protocol rather than this sketch talking 1-Wire bytes directly.
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
#include <DallasTemperature.h>
#include <OneWire.h>
#include <SensorNode.h>
#include <SensorNodeBattery.h>
#include <Wire.h>

// Optional Adafruit FeatherWing OLED (128x64, SH1107, I2C address 0x3C -- shares the Qwiic bus
// with the MAX17048; independent of the bit-banged 1-Wire probe bus). Detected at boot via
// oled.begin()'s return value, same pattern as battery.begin() below -- if it's not physically
// connected, oledPresent stays false and every display call in loop() is skipped entirely.
// SH1107's native orientation is portrait (tall x wide) -- constructor takes height then width,
// and setRotation(1) below rotates it back to landscape.
const int kScreenWidth = 128;
const int kScreenHeight = 64;
const uint8_t kOledAddress = 0x3C;
Adafruit_SH1107 oled(kScreenHeight, kScreenWidth, &Wire);
bool oledPresent = false;
String oledTitle;  // deviceName, else a hardcoded fallback -- set in setup()

// Top-line battery gauge, right-aligned -- same layout as BME280Node.ino. kTitleWidth (6px/char
// at text size 1) leaves the last 2 characters' worth of pixels overlapping the icon's
// kBatteryIconWidth-pixel corner -- fine, since that area gets cleared and redrawn whenever the
// icon actually draws (see loop()); when it doesn't (no fuel gauge), those 2 extra characters of
// device name show instead of being reserved-and-wasted. No per-reading text yet -- see the file
// header comment.
const size_t kTitleWidth = 21;
const int16_t kBatteryIconWidth = 14;

// A 13x7 outline + 1px nub, matching ~/Pictures/bat.png's hand-drawn reference icon -- same
// duplicated-by-design copy as BME280Node.ino's drawBatteryIcon() (simple enough that copying it
// is fewer lines than sharing it across examples).
void drawBatteryIcon(int16_t x, int16_t y, uint8_t barsLit) {
  oled.drawRect(x, y, 13, 7, SH110X_WHITE);
  oled.drawFastVLine(x + 13, y + 2, 3, SH110X_WHITE);
  for (uint8_t i = 0; i < barsLit && i < 5; i++) {
    oled.fillRect(x + 2 + 2 * i, y + 2, 1, 3, SH110X_WHITE);
  }
}

// Same reset pin and rationale as BasicNode.ino.
const int kResetPin = 2;

// Bump this to force the setup portal open once on the next boot -- see checkFirmwareVersion().
const uint32_t kFirmwareVersion = 2;

// GPIO3 -- the only other pin free of every strapping/peripheral reservation on this board
// family, alongside GPIO2 (kResetPin above) -- see the library's CLAUDE.md. 1-Wire is
// bit-banged (an external ~4.7k pull-up to 3.3V on the data line), not a hardware peripheral
// like I2C, so there's no dedicated/default pin the way Wire has one -- any free GPIO works.
const int kOneWirePin = 3;
OneWire oneWire(kOneWirePin);
DallasTemperature dsSensors(&oneWire);

// The 5 probes' known 64-bit ROM addresses, in a fixed order -- same codes as
// CCubes_DataLogger/batcave_temperature5.pde's temp01..temp05, so each probe keeps the same
// identity/channel it's had historically regardless of which order the bus happens to enumerate
// them in at boot. Index into this array is the channel offset (0-4) in kChannels below.
const uint8_t kNumProbes = 5;
const DeviceAddress kKnownProbes[kNumProbes] = {
    {0x28, 0x4A, 0x5C, 0xD7, 0x03, 0x00, 0x00, 0x73},  // temp01
    {0x28, 0xDB, 0x70, 0xD7, 0x03, 0x00, 0x00, 0x2C},  // temp02
    {0x28, 0xCC, 0x7E, 0xD7, 0x03, 0x00, 0x00, 0x50},  // temp03
    {0x28, 0x87, 0x86, 0xD7, 0x03, 0x00, 0x00, 0x45},  // temp04
    {0x28, 0x5D, 0xAA, 0xD7, 0x03, 0x00, 0x00, 0x97},  // temp05
};

// Whether each of kKnownProbes actually answered on the bus at boot (see setup()) -- a probe
// not found here reads NAN every cycle instead of a stale/garbage value. Any ROM code found on
// the bus that ISN'T in kKnownProbes is silently ignored, not logged.
bool probeFound[kNumProbes] = {false, false, false, false, false};

// Channels 0-4: DS18B20 temperature, one per kKnownProbes entry, addressed by ROM code (not
// bus index) -- see kKnownProbes above. Channel 15: onboard battery state of charge
// (SensorNodeBattery::kSocChannel -- reserved sitewide, see the library's README.md), same as
// BME280Node.ino. log() zips this positionally against a values list, so the order here has to
// match loop()'s values vector exactly.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "DS18B20", "Temperature", "C", "Temp1", 1},
    {1, "DS18B20", "Temperature", "C", "Temp2", 1},
    {2, "DS18B20", "Temperature", "C", "Temp3", 1},
    {3, "DS18B20", "Temperature", "C", "Temp4", 1},
    {4, "DS18B20", "Temperature", "C", "Temp5", 1},
    {SensorNodeBattery::kSocChannel, "MAX17048", "State of Charge", "%"},
};

// Formats an 8-byte 1-Wire ROM code as 16 hex digits, e.g. "2800000000000000" -- shared by
// scanOneWireBus() below and the startup probe-discovery log line.
String formatAddress(const uint8_t addr[8]) {
  String result;
  for (uint8_t i = 0; i < 8; i++) {
    if (addr[i] < 0x10) result += "0";
    result += String(addr[i], HEX);
  }
  return result;
}

// Sweeps every device on the bus, returning a comma-joined list of ROM codes (see
// formatAddress()) or "none". DS18B20s have family code 0x28 (the ROM code's first byte) --
// not filtered on here, since this is meant to show whatever's actually answering, not assume
// what it is. Independent of kKnownProbes/DallasTemperature above -- this talks to the raw
// OneWire bus directly, so it still works as a bench-testing aid even with no probes found.
String scanOneWireBus() {
  String result;
  byte addr[8];
  oneWire.reset_search();
  while (oneWire.search(addr)) {
    if (result.length() > 0) result += ",";
    result += formatAddress(addr);
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
  oledTitle = config.deviceName.length() > 0 ? config.deviceName : "sensor-node: OneWireNode";

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
    // Show the device name right away rather than leaving the display in whatever it powered on
    // with for the full settle delay below -- loop() (and its clearDisplay()) doesn't run until
    // after that delay, up to 3 minutes. No battery icon yet -- readSOC() isn't called until
    // loop() either.
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print(oledTitle.substring(0, kTitleWidth));
    oled.display();
  }

  // isConnected() (not getAddress-by-index) so detection is keyed on the known ROM codes
  // themselves, not on bus enumeration order -- a probe found here is confirmed to be one of
  // kKnownProbes specifically, and any other device answering on the bus is silently ignored.
  dsSensors.begin();
  // Max resolution (12-bit, 0.0625C steps, ~750ms conversion) rather than trusting whatever's
  // already stored in each chip's EEPROM config byte -- these probes are an old strand reused
  // from a previous project (batcave_temperature5.pde) that never set this either, so it was
  // whatever the factory default happened to be.
  dsSensors.setResolution(12);
  for (uint8_t i = 0; i < kNumProbes; i++) {
    probeFound[i] = dsSensors.isConnected(kKnownProbes[i]);
    if (probeFound[i]) {
      Serial.printf("DS18B20 probe %d found at %s\n", i + 1, formatAddress(kKnownProbes[i]).c_str());
    } else {
      Serial.printf("DS18B20 probe %d not found -- channel %d will read NAN.\n", i + 1, i);
    }
  }

  delay(60UL * 1000);
}

void loop() {
  std::vector<float> values;
  values.reserve(kNumProbes + 1);

  // One broadcast conversion request covers every probe on the bus, rather than a separate
  // request per probe.
  dsSensors.requestTemperatures();
  for (uint8_t i = 0; i < kNumProbes; i++) {
    float value = NAN;
    if (probeFound[i]) {
      float reading = dsSensors.getTempC(kKnownProbes[i]);
      // DEVICE_DISCONNECTED_C (-127) is DallasTemperature's own "read failed" sentinel -- e.g.
      // a probe found at boot got unplugged since -- treated the same as never-found: NAN,
      // which log() then skips rather than reporting a wildly wrong temperature.
      if (reading != DEVICE_DISCONNECTED_C) value = reading;
    }
    values.push_back(value);
  }
  values.push_back(battery.readSOC());  // battery state of charge %

  node.log(kChannels, values);
  node.applyPendingCommand();  // handles open_portal/scan_i2c if either ever shows up here too

  // Title + battery icon only, same as BME280Node.ino -- no per-reading text yet, see the file
  // header comment. Skipped entirely if the OLED wasn't detected at boot.
  if (oledPresent) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print(oledTitle.substring(0, kTitleWidth));
    float soc = values.back();
    // No icon at all (not an empty/0-bar one) when the fuel gauge isn't present -- (int)NAN
    // isn't safe to feed into the bars-lit math below, and a 0-bar icon would misleadingly read
    // as "battery dead" instead of "no battery".
    if (!isnan(soc)) {
      // (soc+10)/20 in integer math is round-to-nearest-20% (equivalent to floor(soc/20 + 0.5)),
      // not floor(soc/20) -- floor alone would read as empty until 20% and never show a full
      // icon until exactly 100%. constrain() clamps the fuel gauge's occasional >100% overshoot.
      uint8_t barsLit = (uint8_t)constrain(((int)soc + 10) / 20, 0, 5);
      // kTitleWidth's last couple characters can land under this corner -- wipe it before
      // drawing, since drawBatteryIcon()'s outline/bars only ever set foreground pixels and
      // would otherwise leave stray text pixels showing through the icon's gaps.
      oled.fillRect(kScreenWidth - kBatteryIconWidth, 0, kBatteryIconWidth, 8, SH110X_BLACK);
      drawBatteryIcon(kScreenWidth - kBatteryIconWidth, 0, barsLit);
    }
    oled.display();
  }

  // "scan_1wire" isn't something SensorNode knows about -- see the file header comment --
  // so it's checked and cleared right here instead.
  if (pendingCommand() == "scan_1wire") {
    Serial.printf("1-Wire scan (pin %d): %s\n", kOneWirePin, scanOneWireBus().c_str());
    clearPendingCommand();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
