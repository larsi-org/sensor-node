// BME280 sensor node: temperature, dew point, humidity, and pressure
// on channels 0-3; the onboard battery is shown on the OLED and logged on channel 15 only with kLogBattery and a meter present.
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
#include <SensorNodePortal.h>
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
// button/jumper -- see checkFirmwareVersion(). 6: the default server URL moved to
// https://larsi.org/api/sensors/ (2026-11-24); the portal opens once so a node saved with the
// old https://larsi.org/sensors/ can be changed.
const uint32_t kFirmwareVersion = 6;

// Log the battery's state of charge on channel 15? The OLED's battery icon shows it either way;
// logging it is off by default (on a node powered over USB it barely moves). See
// SensorNodeBattery.h.
const bool kLogBattery = false;

// kLogBattery and a battery meter that answered at boot (setup()): without the meter the battery
// is neither provisioned nor logged.
bool logBattery = false;

// Channel 0: temperature C, 1: dew point C, 2: humidity %, 3: pressure
// hPa, and with logBattery 15: battery state of charge % (SensorNodeBattery::channel()). See
// BasicNode.ino for when this actually gets posted (needsProvisioning()). Both log() and
// printReadings() (below, for the OLED) zip this positionally against a values list, so it has
// to stay temp/dew point/humidity/pressure(/SOC) in this order -- reordering it without updating loop()'s
// calls to match would silently misfile a reading onto the wrong channel, not just mislabel it
// on screen.
//
// label (short enough for the OLED -- property is meant for the server/reports and runs long,
// e.g. "Dew Point Temperature") and decimalPlaces (matching each sensor's actual accuracy
// rather than its raw register resolution -- both chips report far more digits than their
// datasheet accuracy backs up: BME280 is +-0.5C / +-3%RH / +-1hPa temp/dew point/humidity/
// pressure, MAX17048 is roughly +-1% SOC) are never read by provision(); they're here purely
// for printReadings()/log() below to share.
// Built in setup(), once the battery meter has been looked for.
std::vector<SensorNodeChannel> channelList() {
  std::vector<SensorNodeChannel> list = {
      {0, "BME280", "Temperature", "C", "Temp", 1},
      {1, "BME280", "Dew Point Temperature", "C", "Dew", 1},
      {2, "BME280", "Relative Humidity", "%", "Humid", 0},
      {3, "BME280", "Pressure", "hPa", "Press", 1},
  };
  if (logBattery) list.push_back(SensorNodeBattery::channel());
  return list;
}
std::vector<SensorNodeChannel> channels;

// Prints one OLED line per value, e.g. "Temp:  21.5 C", zipped positionally against channels
// the same way log() is (values[i] labeled/rounded/unit-ed from channels[i]) -- pass fewer
// values than channels to show only the first several rather than slicing channels itself.
const size_t kLabelColumnWidth = 7;

void printReadings(const std::vector<float> &values) {
  for (size_t i = 0; i < values.size() && i < channels.size(); i++) {
    const SensorNodeChannel &channel = channels[i];
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
// since that area gets cleared and redrawn whenever the icon actually draws (see
// drawTitleAndBattery()); when
// it doesn't (no fuel gauge), those 2 extra characters of device name show instead of being
// reserved-and-wasted.
const size_t kTitleWidth = 21;
const int16_t kBatteryIconWidth = 14;

// Draws the title bar: device name, plus a battery icon in the top-right corner when soc isn't
// NAN -- same duplicated-by-design copy across every OLED-equipped example (simple enough that
// copying it is fewer lines than sharing it). No icon at all (not an empty/0-bar one) when soc
// is NAN -- (int)NAN isn't safe to feed into the bars-lit math below, and a 0-bar icon would
// misleadingly read as "battery dead" instead of "no battery"; callers pass NAN before the fuel
// gauge has been read yet (e.g. setup()'s title-only draw) to get a title with no icon.
void drawTitleAndBattery(float soc) {
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.print(oledTitle.substring(0, kTitleWidth));
  if (!isnan(soc)) {
    // (soc+10)/20 in integer math is round-to-nearest-20% (equivalent to floor(soc/20 + 0.5)),
    // not floor(soc/20) -- floor alone would read as empty until 20% and never show a full icon
    // until exactly 100%. soc is already 0-100 here (readSOC() clamps the fuel gauge's
    // occasional overshoot); constrain() is just defensive bounds.
    uint8_t barsLit = (uint8_t)constrain(((int)soc + 10) / 20, 0, 5);
    // kTitleWidth's last couple characters can land under this corner -- wipe it before drawing
    // the icon, since its outline/bars only ever set foreground pixels and would otherwise
    // leave stray text pixels showing through its gaps.
    int16_t iconX = kScreenWidth - kBatteryIconWidth;
    oled.fillRect(iconX, 0, kBatteryIconWidth, 8, SH110X_BLACK);
    // A 13x7 outline + 1px nub, matching ~/Pictures/bat.png's hand-drawn reference icon (decoded
    // pixel-by-pixel rather than redrawn from memory) -- 5 fill bars at iconX+2,4,6,8,10, gaps
    // at iconX+3,5,7,9 always empty, drawn instead of blitted since the shape is simple enough
    // that this is fewer lines than embedding a bitmap.
    oled.drawRect(iconX, 0, 13, 7, SH110X_WHITE);
    oled.drawFastVLine(iconX + 13, 2, 3, SH110X_WHITE);
    for (uint8_t i = 0; i < barsLit && i < 5; i++) {
      oled.fillRect(iconX + 2 + 2 * i, 2, 1, 3, SH110X_WHITE);
    }
  }
}

// Bottom row (y 56-63) of the OLED: the address (at most 15 characters, 6 px each), a 3-character
// note at x 90 ("AP" while the setup portal is up, the time offline otherwise) and a 17 x 7
// Wi-Fi icon at x 110, crossed out when there is no connection. Fed by the library through
// setSensorNodeStatusCallback() below; the same duplicated-by-design copy in every
// OLED-equipped example, like the battery icon. The library only reports; what to draw is this
// sketch's business.
const int16_t kStatusY = 56;
const int16_t kStatusNoteX = 90;
const int16_t kWifiIconX = 110;
SensorNodeStatus oledStatus;  // last status the library pushed

// The icon, one string per row. A digit is the lowest signal level (1 weak .. 3 strong) at which
// that pixel is drawn, so one picture serves all three levels. Offline draws level 3 plus a slash.
const char *const kWifiIcon[7] = {
    "....333333333....",
    "..33.........33..",
    ".3...2222222...3.",
    "3..22.......22..3",
    "..2...11111...2..",
    ".....1.....1.....",
    ".......111.......",
};

void drawWifiIcon(int16_t x, int16_t y, uint8_t level) {
  uint8_t drawLevel = level == 0 ? 3 : level;
  for (uint8_t row = 0; row < 7; row++) {
    for (uint8_t col = 0; col < 17; col++) {
      char c = kWifiIcon[row][col];
      if (c != '.' && c - '0' <= drawLevel) oled.drawPixel(x + col, y + row, SH110X_WHITE);
    }
  }
  if (level == 0) {
    // Slash from top left to bottom right, 2 px wide with a 1 px gap cleared on both sides.
    for (uint8_t row = 0; row < 7; row++) {
      int16_t c = 2 + 2 * row;
      oled.drawPixel(x + c - 1, y + row, SH110X_BLACK);
      if (x + c + 2 < x + 17) oled.drawPixel(x + c + 2, y + row, SH110X_BLACK);
      oled.drawPixel(x + c, y + row, SH110X_WHITE);
      oled.drawPixel(x + c + 1, y + row, SH110X_WHITE);
    }
  }
}

// Draws the bottom row into the frame buffer (no oled.display()) -- called after every redraw
// that clears the screen, and by onStatus() when the state changes.
void paintStatus() {
  const SensorNodeStatus &s = oledStatus;
  oled.fillRect(0, kStatusY, kScreenWidth, 8, SH110X_BLACK);
  oled.setCursor(0, kStatusY);
  oled.print((s.portal || s.connected) ? s.ip.toString() : String("no Wi-Fi"));
  char note[8] = "";
  if (s.portal) {
    strcpy(note, " AP");
  } else if (!s.connected) {
    if (s.downSeconds < 60) snprintf(note, sizeof(note), "%lus", (unsigned long)s.downSeconds);
    else if (s.downSeconds < 3600) snprintf(note, sizeof(note), "%lum", (unsigned long)(s.downSeconds / 60));
    else snprintf(note, sizeof(note), "%luh", (unsigned long)min<uint32_t>(s.downSeconds / 3600, 99));
  }
  oled.setCursor(kStatusNoteX, kStatusY);
  oled.print(note);
  drawWifiIcon(kWifiIconX, kStatusY, s.level());
}

void onStatus(const SensorNodeStatus &s) {
  oledStatus = s;
  if (!oledPresent) return;
  paintStatus();
  oled.display();
}

SensorNode node;
BME280 bme;
SensorNodeBattery battery;

// A reading the BME280 cannot physically produce means the sensor has gone wrong, not the
// weather: seen in production (batcave basement, 2026-11-24) after a reboot, when the sensor kept
// returning humidity 0, a temperature that barely moved and pressure 178 hPa low (still following
// the real pressure) for 8 hours until a power cycle -- the signature of calibration constants
// read wrong at beginI2C(), which the library never reads again. Only the three measured values
// are checked: the dew point is computed from temperature and humidity. Hard physical limits, not
// unusual weather, so no real cold snap or storm can trip them:
// - humidity <= 0 or > 100: the compensation formula clamps to 0..100, so an exact 0 means it
//   computed a negative value (real air is never near 0%); 100 is real (fog, condensation).
// - pressure outside 870..1090 hPa: every sea-level pressure ever recorded, with room for a
//   node a few hundred meters up (station pressure, not reduced to sea level).
// - temperature outside -40..85 C: the chip's rated range.
// NAN fails every comparison, so it fails the check too.
bool readingsPlausible(float t, float rh, float p) {
  return t >= -40 && t <= 85 && rh > 0 && rh <= 100 && p >= 870 && p <= 1090;
}

// Reads temperature, humidity and pressure into t/rh/p; on an implausible reading soft-resets the
// sensor, runs beginI2C() again (chip ID check, calibration constants read again, settings
// written) and reads once more. Returns whether the readings are plausible; when false the
// caller logs nothing from the sensor this cycle and tries again next cycle.
bool readBme(float &t, float &rh, float &p) {
  t = bme.readTempC();
  rh = bme.readFloatHumidity();
  p = bme.readFloatPressure() / 100.0;
  if (readingsPlausible(t, rh, p)) return true;

  Serial.printf("BME280 implausible (T %.2f C, RH %.2f %%, P %.2f hPa) -- re-initializing.\n", t, rh, p);
  bme.reset();
  delay(10);  // the datasheet's 2 ms start-up time after a reset, with margin
  if (!bme.beginI2C()) {
    Serial.println("BME280 not detected after reset -- check wiring.");
    return false;
  }
  delay(100);  // let a full measurement cycle finish in normal mode before reading again
  t = bme.readTempC();
  rh = bme.readFloatHumidity();
  p = bme.readFloatPressure() / 100.0;
  if (readingsPlausible(t, rh, p)) {
    Serial.println("BME280 recovered.");
    return true;
  }
  Serial.printf("BME280 still implausible (T %.2f C, RH %.2f %%, P %.2f hPa) -- nothing logged.\n", t, rh, p);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Display first, so even the blocking setup portals (firmware-version change, button hold) can
  // show their address through the status callback. The title comes straight from NVS here:
  // node.config() is only filled in by begin().
  Wire.begin();
  {
    SensorNodeConfig saved;
    loadSensorNodeConfig(saved);
    oledTitle = saved.deviceName.length() > 0 ? saved.deviceName : "BME280 " + macSuffix();
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
    drawTitleAndBattery(NAN);
    paintStatus();
    oled.display();
  }
  setSensorNodeStatusCallback(onStatus);

  // The battery meter is looked for before anything is provisioned: no meter, no battery channel.
  bool batteryPresent = battery.begin();
  if (!batteryPresent) Serial.println("MAX17048 not detected -- no battery icon, battery not logged.");
  logBattery = kLogBattery && batteryPresent;
  channels = channelList();

  node.checkFirmwareVersion(kFirmwareVersion);
  node.checkPendingCommand();
  node.checkPortalButton(kResetPin);
  node.begin();
  if (node.needsProvisioning()) node.provision(channels);

  if (!bme.beginI2C()) {
    Serial.println("BME280 not detected -- check wiring.");
  }

  // The very first reading right after boot occasionally comes back as a
  // fixed garbage value (seen in production: identical bogus temp/humidity/
  // pressure on two separate boots) -- likely the sensor or I2C bus not yet
  // settled amid WiFi connect/provisioning right beforehand. Give it a
  // minute to settle -- fixed, not derived from logIntervalMinutes (whose
  // portal-enforced minimum is already 1), so there's no long-interval
  // device sitting dark for its whole first cycle to guard against.
  node.idle(60UL * 1000);
}

void loop() {
  float ch0, ch2, ch3;                           // temperature C, humidity %, pressure hPa
  bool bmeOk = readBme(ch0, ch2, ch3);
  float ch1  = bmeOk ? bme.dewPointC() : NAN;   // dew point C, only from readings that passed
  if (!bmeOk) ch0 = ch2 = ch3 = NAN;            // log() skips NAN values; all NAN logs nothing
  float soc  = battery.readSOC();               // battery state of charge %, for the OLED icon

  std::vector<float> readings = {ch0, ch1, ch2, ch3};
  if (logBattery) readings.push_back(soc);      // channel 15, see channelList()

  // Zipped positionally against channels -- picks up each entry's id/decimalPlaces from there.
  // No need to pad channels 4-14 by hand; log() fills the gap up to channel 15 itself.
  if (node.needsProvisioning()) node.provision(channels);  // retried here too: setup() may have been offline
  node.log(channels, readings);
  node.applyPendingCommand();

  // "Current values" text screen -- entirely skipped if the OLED wasn't
  // detected at boot, so this is a no-op on a board with none wired up.
  if (oledPresent) {
    drawTitleAndBattery(soc);
    oled.setCursor(0, 8);
    // Reads label/decimalPlaces/unit straight from channels, so the OLED can't drift from
    // what log() above and provision() actually use -- all three read the same rows.
    if (bmeOk) printReadings(readings);
    else oled.println("BME280 fault");
    paintStatus();
    oled.display();
  }

  node.idle(node.config().logIntervalMinutes * 60UL * 1000);  // not delay(): keeps Wi-Fi reconnecting
}
