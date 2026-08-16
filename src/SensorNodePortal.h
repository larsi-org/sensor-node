#pragma once

// Blocks forever: brings up an open Wi-Fi access point plus a captive
// config portal (network picker, password, device name, device id,
// write key, log frequency), saves whatever the user submits, then
// restarts the device. Whatever's already saved (if anything) is left
// on disk until the user submits -- this doesn't erase config itself,
// callers that want a full wipe first call resetConfig().
void runSensorNodeSetupPortal();
