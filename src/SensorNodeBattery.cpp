#include "SensorNodeBattery.h"

bool SensorNodeBattery::begin(TwoWire &wirePort) {
  ready_ = gauge_.begin(wirePort);
  if (ready_) gauge_.quickStart();
  return ready_;
}

float SensorNodeBattery::readSOC() {
  if (!ready_) return NAN;
  return gauge_.getSOC();
}
