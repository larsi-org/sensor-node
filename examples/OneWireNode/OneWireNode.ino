// DS18B20 (1-Wire) temperature node. No OLED for now (2026-08-25: pulled to free up the board
// while wiring the actual probe) -- see BME280Node.ino if it needs to come back, same treatment
// duplicated from there originally.
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

#include <DallasTemperature.h>
#include <OneWire.h>
#include <SensorNode.h>
#include <SensorNodeBattery.h>

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
DallasTemperature dsSensors(&oneWire);

// The probe's 64-bit ROM address, discovered once at boot (see setup()) and used for every
// later read -- addressed by ROM code, not bus position/index, so a second probe added to the
// bus later can't silently swap which physical probe channel 0 means. probeFound stays false
// (and the temperature channel reads NAN every cycle, never a stale/garbage value) if nothing
// answered at boot.
DeviceAddress probeAddress;
bool probeFound = false;

// Channel 0: DS18B20 temperature, discovered by ROM address (not index) -- see probeAddress
// above. Channel 15: onboard battery state of charge (SensorNodeBattery::kSocChannel --
// reserved sitewide, see the library's README.md), same as BME280Node.ino. log() zips this
// positionally against a values list, so it has to stay temperature/SOC in this order.
const std::vector<SensorNodeChannel> kChannels = {
    {0, "DS18B20", "Temperature", "C", "Temp", 1},
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
// what it is. Independent of probeAddress/DallasTemperature above -- this talks to the raw
// OneWire bus directly, so it still works as a bench-testing aid even if probeFound is false.
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

  Wire.begin();
  if (!battery.begin()) {
    Serial.println("MAX17048 not detected -- battery channel will read NAN.");
  }

  dsSensors.begin();
  probeFound = dsSensors.getAddress(probeAddress, 0);
  if (probeFound) {
    Serial.printf("DS18B20 found at %s\n", formatAddress(probeAddress).c_str());
  } else {
    Serial.println("No DS18B20 found at startup -- temperature channel will read NAN.");
  }

  delay(60UL * 1000);
}

void loop() {
  float ch0 = NAN;
  if (probeFound) {
    dsSensors.requestTemperaturesByAddress(probeAddress);
    float reading = dsSensors.getTempC(probeAddress);
    // DEVICE_DISCONNECTED_C (-127) is DallasTemperature's own "read failed" sentinel -- e.g.
    // the probe found at boot got unplugged since -- treated the same as never-found: NAN,
    // which log() then skips rather than reporting a wildly wrong temperature.
    if (reading != DEVICE_DISCONNECTED_C) ch0 = reading;
  }
  float ch15 = battery.readSOC();  // battery state of charge %

  node.log(kChannels, {ch0, ch15});
  node.applyPendingCommand();  // handles open_portal/scan_i2c if either ever shows up here too

  // "scan_1wire" isn't something SensorNode knows about -- see the file header comment --
  // so it's checked and cleared right here instead.
  if (pendingCommand() == "scan_1wire") {
    Serial.printf("1-Wire scan (pin %d): %s\n", kOneWirePin, scanOneWireBus().c_str());
    clearPendingCommand();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
