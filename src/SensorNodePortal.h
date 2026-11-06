#pragma once

#include <Arduino.h>

// Last 6 hex digits of WiFi.macAddress() (the per-device-unique tail, not the shared vendor
// OUI) -- exposed so sketches can build their own short unique identifiers (e.g. an OLED
// fallback title) the same way the AP name and default device name already do.
String macSuffix();

// Splits a server URL like "https://larsi.org/sensors/" into a bare hostname ("larsi.org")
// and a base path guaranteed to start and end with "/" ("/sensors/") -- SensorNode builds its
// API endpoints by appending "log"/"provision" directly onto basePath. The scheme (if any) is
// accepted but ignored: SensorNode always connects over TLS on port 443 regardless of what's
// typed, and there's no port-override support, so "http://", "https://", or no scheme at all
// just changes what gets discarded. Returns false (leaving host/basePath untouched) if no host
// is present at all, e.g. an empty or scheme-only URL -- used both by SensorNodePortal's
// handleSave() (reject before saving) and SensorNode::resolveServerIp() (parse before use).
bool parseServerUrl(const String &url, String &host, String &basePath);

// The portal in pieces, for running it in the background while the main loop goes on (see
// SensorNode::idle()): start brings up the access point (AP_STA, so station-side connection
// attempts can continue), DNS and web server and returns; service does one pass of both
// servers (and restarts the device a second after a successful save); stop tears it all down
// and returns to plain station mode. Safe to call start/stop repeatedly.
// sensorNodePortalBusy() is true while someone is evidently using it -- a phone on the AP
// that joined or sent a request within the last 5 minutes, or a save about to restart -- so
// the caller doesn't pull the portal out from under a half-filled form.
void startSensorNodePortal(uint32_t pendingFirmwareVersion = 0);
void serviceSensorNodePortal();
void stopSensorNodePortal();
bool sensorNodePortalActive();
bool sensorNodePortalBusy();

// Blocks forever: brings up an open Wi-Fi access point plus a captive
// config portal (network picker, password, device name, device id,
// API key, log frequency), saves whatever the user submits, then
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
