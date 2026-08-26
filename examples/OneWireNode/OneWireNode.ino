// DS18B20 (1-Wire) temperature node -- 5 probes on one bus, from an old strand originally wired
// up for CCubes_DataLogger/batcave_temperature5.pde (see kKnownProbes below; same ROM codes,
// carried over so the probes keep their physical identity). No OLED for now (2026-08-25: pulled
// to free up the board while wiring the actual probes) -- see BME280Node.ino if it needs to come
// back, same treatment duplicated from there originally.
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

  Wire.begin();
  if (!battery.begin()) {
    Serial.println("MAX17048 not detected -- battery channel will read NAN.");
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

  // "scan_1wire" isn't something SensorNode knows about -- see the file header comment --
  // so it's checked and cleared right here instead.
  if (pendingCommand() == "scan_1wire") {
    Serial.printf("1-Wire scan (pin %d): %s\n", kOneWirePin, scanOneWireBus().c_str());
    clearPendingCommand();
  }

  delay(node.config().logIntervalMinutes * 60UL * 1000);
}
