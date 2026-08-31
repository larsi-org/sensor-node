// DS18B20 (1-Wire) temperature node -- any number of probes (up to kMaxProbes) on one bit-banged
// bus, GPIO3. OLED (device name + battery icon, same treatment duplicated from BME280Node.ino) is
// on the I2C Qwiic bus, independent of the 1-Wire probe bus, so there's no conflict between the
// two. Per-reading text (printReadings() in BME280Node.ino) is intentionally NOT duplicated here
// yet -- this sketch's measurements will get their own, different on-screen layout later.
//
// PROBE CONFIG (2026-08-30): which probes exist comes from a per-deployment text file fetched at
// boot via SensorNode::fetchConfig() -- same mechanism and file format as DS18B20GridNode.ino's
// grid config (a POST of this device's key/device id to https://larsi.org/sensors/config, which
// resolves to sensors/config/<prefix>-<deviceId>.txt's raw content). This sketch flattens that
// file's per-line/per-column layout into one list, read combined off the single physical bus on
// kOneWirePin -- there's no DS2482-800 I2C-to-1-Wire bridge on this station yet (see the library's
// CLAUDE.md), so there's no way to address each line as an independent bus the way GridNode does.
// Once the bridge arrives, the same config file works unchanged for DS18B20GridNode -- this
// sketch is the bridge-less stopgap, not a different config format. No more hardcoded ROM codes
// (the old batcave_temperature5.pde-derived kKnownProbes list) -- channel ids are assigned in
// file order, same convention as GridNode (first ID in the file is channel 0, etc.), so which
// physical probe is which channel is now a server-side config edit, not a reflash.
//
// Each probe also gets an "A1"/"B3"/etc position label from its row/order in the file (see
// probeLabel below) -- local-only for now, not sent to the server (SensorNodeChannel::label
// never is), since there's no OLED space to show it yet. Wired up early so it's ready once the
// on-screen layout has room.
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
#include <SensorNodePortal.h>
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
// Bumped for the switch to config-driven channels (2026-08-30): the channel list this sketch
// provisions is no longer the old fixed 5-probe "Temp1".."Temp5" shape, so every existing
// deployment needs to reprovision.
const uint32_t kFirmwareVersion = 3;

// GPIO3 -- the only other pin free of every strapping/peripheral reservation on this board
// family, alongside GPIO2 (kResetPin above) -- see the library's CLAUDE.md. 1-Wire is
// bit-banged (an external ~4.7k pull-up to 3.3V on the data line), not a hardware peripheral
// like I2C, so there's no dedicated/default pin the way Wire has one -- any free GPIO works.
// Every probe from the fetched config is assumed wired to this single pin -- see the file header
// comment on why this sketch flattens what DS18B20GridNode.ino would otherwise split across
// multiple DS2482-800 channels/physical buses.
const int kOneWirePin = 3;
OneWire oneWire(kOneWirePin);
DallasTemperature dsSensors(&oneWire);

// Channel budget: 0-14 for probes (channel 15 is always the battery, reserved sitewide -- see
// SensorNodeBattery::kSocChannel), same ceiling as DS18B20GridNode.ino's kMaxTotalProbes, since
// both sketches can read the same config file.
const uint8_t kMaxProbes = 15;

uint8_t probeCount = 0;
uint8_t romId[kMaxProbes][8];
bool probeFound[kMaxProbes];

// Each probe's position in the config file, as "A1"/"A2"/"B1"/etc -- row letter is the file's
// line number (line 1 = A, line 2 = B, ...; a blank line doesn't consume a letter, same as it
// doesn't consume a probe slot -- see parseOneWireConfig() below), position is 1-based order
// within that line. Local-only for now (no space left on the OLED -- see the file header
// comment): held here as SensorNodeChannel::label so it's already wired up for whenever the
// on-screen layout has room, not sent to provision() (label is never sent server-side, see
// SensorNode.h). Needs static storage duration since label is a raw `const char *` -- a global
// buffer array (rather than a stack/temporary String) gives each entry a stable address for the
// life of the program.
char probeLabel[kMaxProbes][4];  // "A" + up to 2 digits + '\0', e.g. "A1" or "A15"

// Built once in setup() from the parsed config.
std::vector<SensorNodeChannel> channels;

// One hex digit -> its 0-15 value, or -1 if not a hex digit -- used by parseRomId() below instead
// of strtoul() per-nibble to avoid churning a temporary String per digit. Same helper as
// DS18B20GridNode.ino -- duplicated rather than shared since it's a single-line utility with no
// other state to couple the two sketches through.
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

// Parses the config file's text (same format as DS18B20GridNode.ino's grid config -- one line per
// column, each a comma-separated list of 16-hex-char ROM IDs) into a single flat probeCount/romId
// list, in file order (column A's IDs first, then B's, then C's) -- there's no bridge here to
// keep the columns on separate physical buses, so which "column" an ID nominally belongs to
// doesn't matter for reading, only for channel numbering, which this preserves so the same file
// assigns the same channel ids under both sketches. Also fills probeLabel[] with each probe's
// "A1"/"B3"/etc position (row letter = line number, a blank line consumes neither a letter nor a
// probe slot; number = 1-based order within that line) -- see probeLabel's own comment above.
// Returns false (leaving probeCount/romId/probeLabel untouched) if it yields zero usable probes,
// e.g. an empty fetch (see SensorNode::fetchConfig()'s "empty on any failure" behavior) or a file
// that's nothing but blank lines. A single bad ID or an over-long line doesn't abort the whole
// parse -- a typo in one entry doesn't cost every other probe in the file.
bool parseOneWireConfig(const String &text) {
  uint8_t newProbeCount = 0;
  uint8_t newRomId[kMaxProbes][8];
  char newProbeLabel[kMaxProbes][4];

  int lineStart = 0;
  char rowLetter = 'A';
  while (lineStart < (int)text.length() && newProbeCount < kMaxProbes) {
    int lineEnd = text.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = text.length();
    String line = text.substring(lineStart, lineEnd);
    line.trim();
    lineStart = lineEnd + 1;
    if (line.length() == 0) continue;  // a blank line contributes no probes, consumes no letter

    uint8_t position = 1;
    int idStart = 0;
    while (idStart < (int)line.length() && newProbeCount < kMaxProbes) {
      int idEnd = line.indexOf(',', idStart);
      if (idEnd < 0) idEnd = line.length();
      String idText = line.substring(idStart, idEnd);
      idText.trim();
      idStart = idEnd + 1;
      if (idText.length() == 0) continue;  // tolerate a trailing/doubled comma
      if (!parseRomId(idText, newRomId[newProbeCount])) {
        Serial.printf("[OneWireConfig] Bad ROM ID \"%s\" -- skipped\n", idText.c_str());
        continue;
      }
      snprintf(newProbeLabel[newProbeCount], sizeof(newProbeLabel[newProbeCount]), "%c%u", rowLetter,
               position);
      newProbeCount++;
      position++;
    }
    rowLetter++;
  }

  if (newProbeCount == 0) return false;

  probeCount = newProbeCount;
  memcpy(romId, newRomId, sizeof(romId));
  memcpy(probeLabel, newProbeLabel, sizeof(probeLabel));
  return true;
}

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
// what it is. Independent of the fetched config above -- this talks to the raw OneWire bus
// directly, so it still works as a bench-testing aid even with no config loaded.
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
  node.begin();  // connects Wi-Fi -- fetchConfig() below needs that

  const SensorNodeConfig &config = node.config();
  oledTitle = config.deviceName.length() > 0 ? config.deviceName : "OneWireNode " + macSuffix();

  bool probesConfigured = parseOneWireConfig(node.fetchConfig());
  if (!probesConfigured) {
    Serial.println("[OneWireConfig] Failed to load/parse the probe config -- every channel will read NAN.");
  }

  channels.clear();
  for (uint8_t i = 0; i < probeCount; i++) {
    channels.push_back({i, "DS18B20", "Temperature", "C", probeLabel[i]});
  }
  channels.push_back({SensorNodeBattery::kSocChannel, "MAX17048", "State of Charge", "%"});

  // Provisioning needs the real channel list, which only exists once the probe config has been
  // fetched -- skip it entirely on a boot where that failed rather than registering an empty/
  // wrong shape; needsProvisioning() stays set, so a later boot (once the config is reachable)
  // gets another attempt.
  if (node.needsProvisioning()) {
    if (probesConfigured) {
      node.provision(channels);
    } else {
      Serial.println("[SensorNode] Skipping provision() -- no probe config yet.");
    }
  }

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

  // isConnected() (not getAddress-by-index) so detection is keyed on the parsed ROM codes
  // themselves, not on bus enumeration order -- a probe found here is confirmed to be one of
  // romId specifically, and any other device answering on the bus is silently ignored.
  dsSensors.begin();
  // Max resolution (12-bit, 0.0625C steps, ~750ms conversion) rather than trusting whatever's
  // already stored in each chip's EEPROM config byte.
  dsSensors.setResolution(12);
  for (uint8_t i = 0; i < probeCount; i++) {
    probeFound[i] = dsSensors.isConnected(romId[i]);
    if (probeFound[i]) {
      Serial.printf("DS18B20 probe %s (channel %d) found at %s\n", probeLabel[i], i,
                    formatAddress(romId[i]).c_str());
    } else {
      Serial.printf("DS18B20 probe %s (channel %d) not found -- will read NAN.\n", probeLabel[i], i);
    }
  }

  delay(60UL * 1000);
}

void loop() {
  std::vector<float> values;
  values.reserve(probeCount + 1);

  // One broadcast conversion request covers every probe on the bus, rather than a separate
  // request per probe.
  dsSensors.requestTemperatures();
  for (uint8_t i = 0; i < probeCount; i++) {
    float value = NAN;
    if (probeFound[i]) {
      float reading = dsSensors.getTempC(romId[i]);
      // DEVICE_DISCONNECTED_C (-127) is DallasTemperature's own "read failed" sentinel -- e.g.
      // a probe found at boot got unplugged since -- treated the same as never-found: NAN,
      // which log() then skips rather than reporting a wildly wrong temperature.
      if (reading != DEVICE_DISCONNECTED_C) value = reading;
    }
    values.push_back(value);
  }
  values.push_back(battery.readSOC());  // battery state of charge %

  node.log(channels, values);
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
      // icon until exactly 100%. soc is already 0-100 here (readSOC() clamps the fuel gauge's
      // occasional overshoot); constrain() is just defensive bounds.
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
