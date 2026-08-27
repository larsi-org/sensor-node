// TheCoop temperature grid node: revives the 2012-era 3-node "Temperature Grid A/B/C" setup
// (thecoop device_id 4/5/6 in the sensors DB, 15 DS18B20 channels each -- 3 nodes x 3 columns x
// 5 vertical slices = the 45-sensor grid) as ONE board instead of three, using 3 of the
// DS2482-800's 8 channels as the 3 columns (5 probes/column on its own physical 1-Wire line) and
// this board's onboard MAX17048 for channel 15 -- exactly the slot the old 3 boards never used.
//
// Requires the "Adafruit DS248x" library (Arduino Library Manager) for the DS2482-800/DS2484
// I2C-to-1-Wire bridge. Deliberately NOT DallasTemperature/OneWire -- Adafruit_DS248x's
// OneWireReset/OneWireWriteByte/OneWireReadByte/OneWireSearch are the only primitives it
// provides, so the DS18B20 command sequence (skip-ROM broadcast Convert T, match-ROM Read
// Scratchpad, CRC8) is hand-rolled below, same shape as the library's own DS2484_DS18B20
// example. This also means the ESP32-C6 OneWire compile problem (see OneWireNode.ino's header)
// never applies here -- there's no OneWire/DallasTemperature dependency to hit it.
//
// See BasicNode.ino for the setup-portal walkthrough (first boot, the SensorNode-Setup-XXXXXX
// access point, and the reset-button hold behavior) -- identical here.
//
// ROM ADDRESSING (2026-08-26): whether thecoop's original 3-board grid is even still physically
// intact is unknown, and its ROM codes were never captured anywhere -- so this doesn't try to
// revive that specific hardware. Column A instead reuses OneWireNode.ino's 5 known-good probes
// (the old batcave_temperature5.pde strand) as a bench-testing set with IDs already on hand --
// see kKnownProbesA below, verified present at boot the same way OneWireNode.ino does (a probe
// not found there reads NAN every cycle instead of a stale/garbage value). Columns B and C have
// no known IDs at all yet, so they're still searched fresh at boot and assigned grid position by
// SEARCH ORDER, which is NOT physical position along the bus (it's driven by each ROM's address
// bits) -- fine for now since "up to 15 sensors" is a ceiling this node supports, not a
// requirement it's actually wired for. If B/C ever get real probes whose vertical position
// matters, read the "column X probe Y: ROM ..." lines this prints at boot, confirm/relabel which
// ROM is actually at which slice by hand (e.g. warm one probe at a time and watch which reading
// jumps), and hardcode a kKnownProbes table for them the same way column A already has one.

#include <Adafruit_DS248x.h>
#include <Adafruit_SH110X.h>
#include <SensorNode.h>
#include <SensorNodeBattery.h>
#include <Wire.h>

// Same reset pin and rationale as BasicNode.ino.
const int kResetPin = 2;

// Bump this to force the setup portal open once on the next boot -- see checkFirmwareVersion().
const uint32_t kFirmwareVersion = 1;

// DS2482-800 defaults to 0x18 -- checked clear of this board's other I2C devices (MAX17048
// 0x36, OLED 0x3C) regardless of how the address pins end up strapped (whole range is
// 0x18-0x1F), so there's no need to move off the default.
Adafruit_DS248x bridge;

// The 3 columns (A/B/C, matching the old 3 separate boards) each get their own DS2482-800
// channel and their own physical 1-Wire line of up to 5 probes (vertical slices, top to
// bottom). Channels 3-7 go unused -- see the library's CLAUDE.md note on this project's line
// layout ruling out one-probe-per-channel.
const uint8_t kNumColumns = 3;
const uint8_t kProbesPerColumn = 5;
const uint8_t kBridgeChannel[kNumColumns] = {0, 1, 2};
const char *kColumnLetter[kNumColumns] = {"A", "B", "C"};

// Column A's 5 probes -- same physical strand and ROM codes as OneWireNode.ino's kKnownProbes
// (originally CCubes_DataLogger/batcave_temperature5.pde's temp01..temp05), reused here as a
// bench-testing set with known-good IDs on hand. See the ROM ADDRESSING note above.
const uint8_t kKnownProbesA[kProbesPerColumn][8] = {
    {0x28, 0x4A, 0x5C, 0xD7, 0x03, 0x00, 0x00, 0x73},  // temp01
    {0x28, 0xDB, 0x70, 0xD7, 0x03, 0x00, 0x00, 0x2C},  // temp02
    {0x28, 0xCC, 0x7E, 0xD7, 0x03, 0x00, 0x00, 0x50},  // temp03
    {0x28, 0x87, 0x86, 0xD7, 0x03, 0x00, 0x00, 0x45},  // temp04
    {0x28, 0x5D, 0xAA, 0xD7, 0x03, 0x00, 0x00, 0x97},  // temp05
};

// Column A copied from kKnownProbesA at boot (verified present, see setup()); columns B/C filled
// by searchColumn() -- see the ROM ADDRESSING note above.
uint8_t discoveredRom[kNumColumns][kProbesPerColumn][8];
bool probeFound[kNumColumns][kProbesPerColumn];

// Channel ids 0-14 (column-major: column*5 + vertical slice), matching the shape of the old
// thecoop device_id 4/5/6 rows in the sensors DB (15 DS18B20 Temperature channels each) --
// consolidated onto one device here instead of split across 3. Channel 15: onboard battery
// state of charge (SensorNodeBattery::kSocChannel -- reserved sitewide). log() zips this
// positionally against a values list built the same column-major way in loop(). Written out as
// a flat literal table (not generated from kColumnLetter) since SensorNodeChannel::label is a
// raw `const char *`, not a String -- a loop-built String's buffer wouldn't outlive the loop
// iteration that created it.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "DS18B20", "Temperature", "C", "A1", 1},
    {1, "DS18B20", "Temperature", "C", "A2", 1},
    {2, "DS18B20", "Temperature", "C", "A3", 1},
    {3, "DS18B20", "Temperature", "C", "A4", 1},
    {4, "DS18B20", "Temperature", "C", "A5", 1},
    {5, "DS18B20", "Temperature", "C", "B1", 1},
    {6, "DS18B20", "Temperature", "C", "B2", 1},
    {7, "DS18B20", "Temperature", "C", "B3", 1},
    {8, "DS18B20", "Temperature", "C", "B4", 1},
    {9, "DS18B20", "Temperature", "C", "B5", 1},
    {10, "DS18B20", "Temperature", "C", "C1", 1},
    {11, "DS18B20", "Temperature", "C", "C2", 1},
    {12, "DS18B20", "Temperature", "C", "C3", 1},
    {13, "DS18B20", "Temperature", "C", "C4", 1},
    {14, "DS18B20", "Temperature", "C", "C5", 1},
    {SensorNodeBattery::kSocChannel, "MAX17048", "State of Charge", "%"},
};

// Optional Adafruit FeatherWing OLED (128x64, SH1107, I2C address 0x3C -- shares the Qwiic bus
// with the bridge/MAX17048; independent of the bridge's 1-Wire channels). Detected at boot via
// oled.begin()'s return value -- if it's not physically connected, oledPresent stays false and
// every display call in loop() is skipped entirely. SH1107's native orientation is portrait
// (tall x wide) -- constructor takes height then width, and setRotation(1) below rotates it
// back to landscape.
const int kScreenWidth = 128;
const int kScreenHeight = 64;
const uint8_t kOledAddress = 0x3C;
Adafruit_SH1107 oled(kScreenHeight, kScreenWidth, &Wire);
bool oledPresent = false;
String oledTitle;  // deviceName, else a hardcoded fallback -- set in setup()

// Row 0: title + battery icon (kTitleWidth/kBatteryIconWidth, same layout as BME280Node.ino).
// Row 1: left blank on purpose -- a visual gap before the grid, spending one of the 8 rows this
// screen has to spare on readability. Rows 2-6: one vertical slice per row, 3 columns per row
// (row 7 unused). Each column is a 5-char right-justified numeric field plus a literal "C" unit
// suffix (kValueNumericWidth + 1 = 6 chars), joined by single "|" dividers between columns (not
// after the last one) -- e.g. " 28.3C|-10.5C|  0.3C", 3 x 6 + 2 = 20 characters, tight against
// this display's 21-char width. All 3 columns and both dividers are always drawn regardless of
// how many probes actually answered (a missing one just shows "--", no "C") -- deliberately, so
// the display always shows the full 15-sensor layout this node supports, not just however many
// are wired up today.
const size_t kTitleWidth = 21;
const int16_t kBatteryIconWidth = 14;
const size_t kValueNumericWidth = 5;
const size_t kValueFieldWidth = kValueNumericWidth + 1;  // + 1 for the "C" unit suffix

// A 13x7 outline + 1px nub, matching ~/Pictures/bat.png's hand-drawn reference icon -- same
// duplicated-by-design copy as BME280Node.ino's drawBatteryIcon().
void drawBatteryIcon(int16_t x, int16_t y, uint8_t barsLit) {
  oled.drawRect(x, y, 13, 7, SH110X_WHITE);
  oled.drawFastVLine(x + 13, y + 2, 3, SH110X_WHITE);
  for (uint8_t i = 0; i < barsLit && i < 5; i++) {
    oled.fillRect(x + 2 + 2 * i, y + 2, 1, 3, SH110X_WHITE);
  }
}

// Right-justifies a temperature into kValueNumericWidth characters plus a literal "C" unit
// suffix, e.g. " 28.3C" / "-10.5C" / "  0.3C". A missing probe (NAN) has no unit to report, so
// it instead gets "--" centered within the same kValueFieldWidth, e.g. "  --  " -- kept visually
// distinct from a real 0.0C reading rather than reusing the numeric format for it.
String formatValue(float value) {
  char buf[16];
  if (isnan(value)) {
    size_t padLeft = (kValueFieldWidth - 2) / 2;
    size_t padRight = kValueFieldWidth - 2 - padLeft;
    snprintf(buf, sizeof(buf), "%*s%*s", (int)(padLeft + 2), "--", (int)padRight, "");
  } else {
    snprintf(buf, sizeof(buf), "%*.1fC", (int)kValueNumericWidth, value);
  }
  return String(buf);
}

// Standard Dallas/Maxim 8-bit CRC (polynomial 0x8C, reflected) -- Adafruit_DS248x has no
// built-in CRC helper (unlike DallasTemperature), so this covers the same "reject a corrupted
// scratchpad read" check OneWireNode.ino gets for free from that library.
uint8_t crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < len; i++) {
    uint8_t inbyte = data[i];
    for (uint8_t b = 0; b < 8; b++) {
      uint8_t mix = (crc ^ inbyte) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      inbyte >>= 1;
    }
  }
  return crc;
}

// Searches whichever channel is currently selected (caller's responsibility, matching
// selectChannel() being a bridge-wide call rather than a per-search argument), filling up to
// kProbesPerColumn ROM codes in search order. Any device beyond kProbesPerColumn is left
// unsearched-for, same "silently ignore extras" convention as OneWireNode.ino's bus scan.
void searchColumn(uint8_t col) {
  bridge.OneWireSearchReset();
  for (uint8_t row = 0; row < kProbesPerColumn; row++) {
    probeFound[col][row] = bridge.OneWireSearch(discoveredRom[col][row]);
    if (probeFound[col][row]) {
      Serial.printf("Column %s probe %d: ROM ", kColumnLetter[col], row + 1);
      for (uint8_t i = 0; i < 8; i++) Serial.printf("%02X", discoveredRom[col][row][i]);
      Serial.println();
    } else {
      Serial.printf("Column %s probe %d: not found -- channel %d will read NAN.\n",
                    kColumnLetter[col], row + 1, col * kProbesPerColumn + row);
    }
  }
}

// Match-ROM + Read Scratchpad against the channel currently selected (caller's responsibility)
// -- assumes requestConvert() below already ran and its 750ms wait has elapsed. Returns NAN on
// a bad CRC (e.g. a probe that dropped off mid-cycle) the same way OneWireNode.ino treats
// DEVICE_DISCONNECTED_C -- skipped by log() rather than reported as a wildly wrong temperature.
float readTemperature(const uint8_t rom[8]) {
  bridge.OneWireReset();
  bridge.OneWireWriteByte(0x55);  // Match ROM
  for (uint8_t i = 0; i < 8; i++) bridge.OneWireWriteByte(rom[i]);
  bridge.OneWireWriteByte(0xBE);  // Read Scratchpad

  uint8_t data[9];
  for (uint8_t i = 0; i < 9; i++) bridge.OneWireReadByte(&data[i]);
  if (crc8(data, 8) != data[8]) return NAN;

  int16_t raw = (data[1] << 8) | data[0];
  return raw / 16.0;  // 12-bit resolution (DS18B20 power-on default), 0.0625C/count
}

// Skip-ROM broadcasts Convert T to every probe on the channel currently selected (caller's
// responsibility) -- one request covers all 5 probes on that column's line, same as
// OneWireNode.ino's dsSensors.requestTemperatures(). Does NOT wait for the 750ms conversion
// itself -- see loop(), which triggers all 3 columns back-to-back before waiting once, since
// each column's DS18B20s convert on their own physical line independently of the I2C bridge
// selection that triggered them.
void requestConvert(uint8_t col) {
  bridge.selectChannel(kBridgeChannel[col]);
  bridge.OneWireReset();
  bridge.OneWireWriteByte(0xCC);  // Skip ROM
  bridge.OneWireWriteByte(0x44);  // Convert T
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
  oledTitle = config.deviceName.length() > 0 ? config.deviceName : "sensor-node: CoopGrid";

  Wire.begin();
  if (!bridge.begin(&Wire)) {
    Serial.println("DS2482-800 not detected -- all grid channels will read NAN.");
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
    // Show the device name right away rather than leaving the display in whatever it powered on
    // with for the full settle delay below -- loop() (and its clearDisplay()) doesn't run until
    // after that delay, up to 3 minutes.
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print(oledTitle.substring(0, kTitleWidth));
    oled.display();
  }

  // Column A: verify each known probe actually answers (a stale scratchpad read still passes
  // CRC if the chip is there, no fresh conversion needed) rather than assuming presence just
  // because the ROM code is in kKnownProbesA -- same "not found -> NAN forever" treatment as
  // OneWireNode.ino. Columns B/C: no known IDs yet, so discover fresh via search instead.
  bridge.selectChannel(kBridgeChannel[0]);
  for (uint8_t row = 0; row < kProbesPerColumn; row++) {
    memcpy(discoveredRom[0][row], kKnownProbesA[row], 8);
    probeFound[0][row] = !isnan(readTemperature(kKnownProbesA[row]));
    Serial.printf("Column A probe %d: %s\n", row + 1,
                  probeFound[0][row] ? "found" : "not found -- will read NAN");
  }
  for (uint8_t col = 1; col < kNumColumns; col++) {
    bridge.selectChannel(kBridgeChannel[col]);
    searchColumn(col);
  }

  delay(60UL * 1000);
}

void loop() {
  // Trigger every column's broadcast Convert T back-to-back, then wait once -- the three
  // columns' probes convert in parallel on their own physical lines during that single 750ms,
  // rather than paying the conversion delay three times over.
  for (uint8_t col = 0; col < kNumColumns; col++) requestConvert(col);
  delay(750);

  float reading[kNumColumns][kProbesPerColumn];
  std::vector<float> values;
  values.reserve(kNumColumns * kProbesPerColumn + 1);
  for (uint8_t col = 0; col < kNumColumns; col++) {
    bridge.selectChannel(kBridgeChannel[col]);
    for (uint8_t row = 0; row < kProbesPerColumn; row++) {
      float value = NAN;
      if (probeFound[col][row]) value = readTemperature(discoveredRom[col][row]);
      reading[col][row] = value;
      values.push_back(value);
    }
  }
  values.push_back(battery.readSOC());  // battery state of charge %

  node.log(kChannels, values);
  node.applyPendingCommand();

  // Title + battery icon, a blank row, then the 3x5 grid -- no per-reading label text (that's
  // the "render differently" layout still to come), just the fixed-width numeric columns.
  if (oledPresent) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.print(oledTitle.substring(0, kTitleWidth));
    float soc = values.back();
    if (!isnan(soc)) {
      uint8_t barsLit = (uint8_t)constrain(((int)soc + 10) / 20, 0, 5);
      oled.fillRect(kScreenWidth - kBatteryIconWidth, 0, kBatteryIconWidth, 8, SH110X_BLACK);
      drawBatteryIcon(kScreenWidth - kBatteryIconWidth, 0, barsLit);
    }

    oled.setCursor(0, 16);  // row 1 (y=8) intentionally left blank -- see kTitleWidth's comment
    for (uint8_t row = 0; row < kProbesPerColumn; row++) {
      String line;
      for (uint8_t col = 0; col < kNumColumns; col++) {
        if (col > 0) line += "|";
        line += formatValue(reading[col][row]);
      }
      oled.println(line);
    }
    oled.display();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
