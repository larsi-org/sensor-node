#pragma once

// Blocks forever: brings up an open Wi-Fi access point plus a captive
// config portal (network picker, password, node name, device id, write
// key), saves whatever the user submits, then restarts the device.
void runSensorNodeSetupPortal();
