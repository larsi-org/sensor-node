// Generic DS18B20 grid node: up to 3 columns (A/B/C), each its own physical 1-Wire line on one
// of the DS2482-800's 8 channels, up to 7 probes (vertical slices) per column -- channel budget
// is 15 total across all columns (server channel ids 0-14) plus channel 15 for the onboard
// MAX17048 battery, so a single column can use all 7 while two others sit unused, or all three
// can be populated as long as they don't collectively exceed 15 (e.g. 5/5/5, or 7/7/1, or
// 7/6/2 -- any split works, not just an even one).
//
// Requires the "Adafruit DS248x" library (Arduino Library Manager) for the DS2482-800/DS2484
// I2C-to-1-Wire bridge. Deliberately NOT DallasTemperature/OneWire -- Adafruit_DS248x's
// OneWireReset/OneWireWriteByte/OneWireReadByte are the only primitives it provides, so the
// DS18B20 command sequence (skip-ROM broadcast Convert T, match-ROM Read Scratchpad, CRC8) is
// hand-rolled below, same shape as the library's own DS2484_DS18B20 example. This also means
// the ESP32-C6 OneWire compile problem (see OneWireNode.ino's header) never applies here --
// there's no OneWire/DallasTemperature dependency to hit it.
//
// See BasicNode.ino for the setup-portal walkthrough (first boot, the SensorNode-Setup-XXXXXX
// access point, and the reset-button hold behavior) -- identical here.
//
// GRID CONFIG (2026-08-27): which probes exist and where they sit in the grid comes from a
// per-deployment text file fetched at boot via SensorNode::fetchConfig() -- a POST of this
// device's key/device id to https://larsi.org/sensors/config (same auth as log()/provision()),
// which the server resolves to this device's location.prefix and returns
// sensors/config/<prefix>-<deviceId>.txt's raw content. That file isn't directly web-accessible
// (Require-all-denied) -- config.php gates every read behind the API key, both so a stray
// prefix/deviceId guess can't read another station's layout and for consistency with every other
// sensor-node endpoint. This replaces hardcoding known ROM codes or discovering them via bus
// search (both tried in earlier revisions of this sketch) -- the real per-deployment layout now
// lives server-side as a small hand-authored file, editable without reflashing.
//
// File format: one line per column, in order (line 1 = column A, line 2 = B, line 3 = C -- a
// file with only 1 or 2 lines just leaves the rest unconfigured), each a comma-separated list
// of that column's probes' 64-bit ROM codes as 16 hex chars (no "0x", no separators within one
// ID -- e.g. "284A5CD703000073"), top-to-bottom/first-to-last. A column can list 0-7 IDs
// (kMaxProbesPerColumn); the columns don't all need the same count -- an asymmetric grid (e.g.
// 7 on A, 3 on B, nothing on C) is exactly as valid as a uniform one, which is the whole reason
// this is free-form CSV-per-line rather than a fixed "columns,rows" header. The only other
// limit is the 15-channel budget shared across every column combined (see the top comment) --
// parsing stops accepting new IDs once that's hit, wherever in the file that happens to be.
// Channel ids are assigned consecutively in file order (column A's IDs first, then B's, then
// C's) -- e.g. a 2-line file "a1,a2,a3\nb1,b2,b3" assigns channel 0=a1, 1=a2, 2=a3, 3=b1, 4=b2,
// 5=b3 (channel 15 is always the battery, never part of this count). Blank lines and blank/
// malformed individual IDs are skipped with a Serial warning rather than aborting the whole
// parse -- a typo in one ID shouldn't cost every other probe in the file.

#include <Adafruit_DS248x.h>
#include <Adafruit_SH110X.h>
#include <SensorNode.h>
#include <SensorNodeBattery.h>
#include <SensorNodePortal.h>
#include <Wire.h>

// Same reset pin and rationale as BasicNode.ino.
const int kResetPin = 2;

// Bump this to force the setup portal open once on the next boot -- see checkFirmwareVersion().
const uint32_t kFirmwareVersion = 1;

// DS2482-800 defaults to 0x18 -- checked clear of this board's other I2C devices (MAX17048
// 0x36, OLED 0x3C) regardless of how the address pins end up strapped (whole range is
// 0x18-0x1F), so there's no need to move off the default.
Adafruit_DS248x bridge;

// Up to 3 columns (A/B/C), each its own DS2482-800 channel and its own physical 1-Wire line.
// Channels 3-7 go unused -- see the library's CLAUDE.md note on this project's line layout
// ruling out one-probe-per-channel. kMaxProbesPerColumn/kMaxTotalProbes are ceilings the parser
// enforces (see the file header comment); columnCount/probeCountPerColumn are this boot's
// actual parsed shape, which can be smaller in every dimension.
const uint8_t kMaxColumns = 3;
const uint8_t kMaxProbesPerColumn = 7;
const uint8_t kMaxTotalProbes = 15;  // channel ids 0-14; 15 is always the battery
const uint8_t kBridgeChannel[kMaxColumns] = {0, 1, 2};

uint8_t columnCount = 0;
uint8_t probeCountPerColumn[kMaxColumns] = {0, 0, 0};
uint8_t romId[kMaxColumns][kMaxProbesPerColumn][8];
bool probeFound[kMaxColumns][kMaxProbesPerColumn];

// Built once in setup() from the parsed grid config -- SensorNodeChannel::label is left at its
// default ("") rather than a generated "A1"/"B3"/etc: it's a raw `const char *`, and with a
// runtime-variable channel count there's no way to give each one a string literal with static
// storage duration the way the old fixed-shape kChannels table could. Nothing in this sketch
// reads label anyway (the OLED grid below is purely positional, and provision() never sends it).
std::vector<SensorNodeChannel> channels;

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
// Rows 1-7: one vertical slice per row, columnCount columns per row (up to kMaxProbesPerColumn
// rows total, filling the screen's remaining 7 rows exactly -- no blank spacer row anymore,
// since 7 rows is the whole point of the budget above). Each column is a 5-char right-justified
// numeric field plus a literal "C" unit suffix (kValueNumericWidth + 1 = 6 chars), joined by
// single "|" dividers between columns (not after the last one) -- e.g.
// " 28.3C|-10.5C|  0.3C" for a 3-column row, 3 x 6 + 2 = 20 characters. Only the columns/rows
// this boot's parsed grid actually has are drawn (not always 3x7) -- a shorter column within a
// taller grid still gets its empty cells rendered as "--" so the grid stays rectangular, but a
// column that doesn't exist at all isn't given a phantom slot.
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

// Match-ROM + Read Scratchpad against the channel currently selected (caller's responsibility)
// -- assumes requestConvert() below already ran and its 750ms wait has elapsed (or, at boot,
// that a prior conversion left a valid scratchpad -- see the presence check in setup()). Returns
// NAN on a bad CRC (e.g. a probe that dropped off mid-cycle), skipped by log() rather than
// reported as a wildly wrong temperature.
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
// responsibility) -- one request covers every probe on that column's line, same as
// OneWireNode.ino's dsSensors.requestTemperatures(). Does NOT wait for the 750ms conversion
// itself -- see loop(), which triggers every column back-to-back before waiting once, since
// each column's DS18B20s convert on their own physical line independently of the I2C bridge
// selection that triggered them.
void requestConvert(uint8_t col) {
  bridge.selectChannel(kBridgeChannel[col]);
  bridge.OneWireReset();
  bridge.OneWireWriteByte(0xCC);  // Skip ROM
  bridge.OneWireWriteByte(0x44);  // Convert T
}

// One hex digit -> its 0-15 value, or -1 if not a hex digit -- used by parseRomId() below
// instead of strtoul() per-nibble to avoid churning a temporary String per digit.
int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parses exactly 16 hex chars into 8 bytes -- false (rom left untouched) on the wrong length or
// any non-hex character.
bool parseRomId(const String &text, uint8_t rom[8]) {
  if (text.length() != 16) return false;
  for (uint8_t i = 0; i < 8; i++) {
    int hi = hexNibble(text[i * 2]);
    int lo = hexNibble(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    rom[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// Parses the grid config file's text (see this file's header comment for the exact format) into
// columnCount/probeCountPerColumn/romId. Returns false (leaving all three untouched) if it
// yields zero usable columns, e.g. an empty fetch (see SensorNode::fetchConfig()'s "empty on any
// failure" behavior) or a file that's nothing but blank lines. A single bad ID or an over-long
// line doesn't abort the whole parse -- see the per-token handling below -- so a typo in one
// entry doesn't cost every other probe in the file.
bool parseGridConfig(const String &text) {
  uint8_t newColumnCount = 0;
  uint8_t newProbeCountPerColumn[kMaxColumns] = {0, 0, 0};
  uint8_t newRomId[kMaxColumns][kMaxProbesPerColumn][8];
  uint8_t totalProbes = 0;

  int lineStart = 0;
  while (lineStart < (int)text.length() && newColumnCount < kMaxColumns) {
    int lineEnd = text.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = text.length();
    String line = text.substring(lineStart, lineEnd);
    line.trim();
    lineStart = lineEnd + 1;
    if (line.length() == 0) continue;  // a blank line doesn't consume a column slot

    uint8_t col = newColumnCount;
    uint8_t count = 0;
    int idStart = 0;
    while (idStart < (int)line.length() && count < kMaxProbesPerColumn &&
           totalProbes < kMaxTotalProbes) {
      int idEnd = line.indexOf(',', idStart);
      if (idEnd < 0) idEnd = line.length();
      String idText = line.substring(idStart, idEnd);
      idText.trim();
      idStart = idEnd + 1;
      if (idText.length() == 0) continue;  // tolerate a trailing/doubled comma
      if (!parseRomId(idText, newRomId[col][count])) {
        Serial.printf("[GridConfig] Bad ROM ID \"%s\" on column %d -- skipped\n", idText.c_str(),
                      col);
        continue;
      }
      count++;
      totalProbes++;
    }
    newProbeCountPerColumn[col] = count;
    newColumnCount++;
  }

  if (newColumnCount == 0) return false;

  columnCount = newColumnCount;
  memcpy(probeCountPerColumn, newProbeCountPerColumn, sizeof(probeCountPerColumn));
  memcpy(romId, newRomId, sizeof(romId));
  return true;
}

SensorNode node;
SensorNodeBattery battery;

void setup() {
  Serial.begin(115200);
  delay(1000);

  node.checkFirmwareVersion(kFirmwareVersion);
  node.checkPendingCommand();
  node.checkPortalButton(kResetPin);
  node.begin();  // connects Wi-Fi -- fetchConfig() below needs that, not the I2C bus

  const SensorNodeConfig &config = node.config();
  oledTitle = config.deviceName.length() > 0 ? config.deviceName : "DS18B20Grid " + macSuffix();

  bool gridConfigured = parseGridConfig(node.fetchConfig());
  if (!gridConfigured) {
    Serial.println("[GridConfig] Failed to load/parse the grid config -- every channel will read NAN.");
  }

  uint8_t nextId = 0;
  for (uint8_t col = 0; col < columnCount; col++) {
    for (uint8_t row = 0; row < probeCountPerColumn[col]; row++) {
      channels.push_back({nextId++, "DS18B20", "Temperature", "C"});
    }
  }
  channels.push_back({SensorNodeBattery::kSocChannel, "MAX17048", "State of Charge", "%"});

  // Provisioning needs the real channel list, which only exists once the grid config has been
  // fetched -- skip it entirely on a boot where that failed rather than registering an empty/
  // wrong shape; needsProvisioning() stays set, so a later boot (once the config is reachable)
  // gets another attempt.
  if (node.needsProvisioning()) {
    if (gridConfigured) {
      node.provision(channels);
    } else {
      Serial.println("[SensorNode] Skipping provision() -- no grid config yet.");
    }
  }

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

  // Verify each parsed ROM actually answers (a stale scratchpad read still passes CRC if the
  // chip is there, no fresh conversion needed) rather than assuming presence just because the
  // config file lists it -- a probe not found here reads NAN every cycle instead of a stale/
  // garbage value, same treatment as every other example in this library.
  for (uint8_t col = 0; col < columnCount; col++) {
    bridge.selectChannel(kBridgeChannel[col]);
    for (uint8_t row = 0; row < probeCountPerColumn[col]; row++) {
      probeFound[col][row] = !isnan(readTemperature(romId[col][row]));
      Serial.printf("Column %d probe %d: %s\n", col, row + 1,
                    probeFound[col][row] ? "found" : "not found -- will read NAN");
    }
  }

  delay(60UL * 1000);
}

void loop() {
  // Trigger every column's broadcast Convert T back-to-back, then wait once -- every column's
  // probes convert in parallel on their own physical lines during that single 750ms, rather
  // than paying the conversion delay once per column.
  for (uint8_t col = 0; col < columnCount; col++) requestConvert(col);
  delay(750);

  float reading[kMaxColumns][kMaxProbesPerColumn];
  uint8_t maxRows = 0;
  std::vector<float> values;
  values.reserve(kMaxTotalProbes + 1);
  for (uint8_t col = 0; col < columnCount; col++) {
    bridge.selectChannel(kBridgeChannel[col]);
    if (probeCountPerColumn[col] > maxRows) maxRows = probeCountPerColumn[col];
    for (uint8_t row = 0; row < probeCountPerColumn[col]; row++) {
      float value = NAN;
      if (probeFound[col][row]) value = readTemperature(romId[col][row]);
      reading[col][row] = value;
      values.push_back(value);
    }
  }
  values.push_back(battery.readSOC());  // battery state of charge %

  node.log(channels, values);
  node.applyPendingCommand();

  // Title + battery icon, then the grid -- no per-reading label text (that's the "render
  // differently" layout still to come), just the fixed-width numeric columns, sized to
  // whatever shape this boot's grid config actually parsed to (not always 3 wide/7 tall).
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

    oled.setCursor(0, 8);  // no blank spacer row -- see the file header comment
    for (uint8_t row = 0; row < maxRows; row++) {
      String line;
      for (uint8_t col = 0; col < columnCount; col++) {
        if (col > 0) line += "|";
        float value = row < probeCountPerColumn[col] ? reading[col][row] : NAN;
        line += formatValue(value);
      }
      oled.println(line);
    }
    oled.display();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
