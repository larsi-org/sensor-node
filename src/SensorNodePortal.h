#pragma once

#include <Arduino.h>

// Last 6 hex digits of WiFi.macAddress() (the per-device-unique tail, not the shared vendor
// OUI) -- exposed so sketches can build their own short unique identifiers (e.g. an OLED
// fallback title) the same way the AP name and default device name already do.
String macSuffix();

// Blocks forever: brings up an open Wi-Fi access point plus a captive
// config portal (network picker, password, device name, device id,
// write key, log frequency), saves whatever the user submits, then
// restarts the device. Whatever's already saved (if anything) is left
// on disk until the user submits -- this doesn't erase config itself,
// callers that want a full wipe first call resetConfig().
//
// pendingFirmwareVersion, if non-zero, is only persisted as the last-seen firmware version
// once the user actually submits the form -- not the moment this function is entered. That
// way a reset or power loss while the portal is sitting open (unconfigured) doesn't silently
// consume a checkFirmwareVersion()-triggered visit; the next boot offers it again instead of
// assuming it already happened.
void runSensorNodeSetupPortal(uint32_t pendingFirmwareVersion = 0);
