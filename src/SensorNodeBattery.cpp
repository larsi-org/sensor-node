#include "SensorNodeBattery.h"

bool SensorNodeBattery::begin(TwoWire &wirePort) {
  ready_ = gauge_.begin(wirePort);
  if (ready_) gauge_.quickStart();
  return ready_;
}

float SensorNodeBattery::readSOC() {
  if (!ready_) return NAN;
  // The MAX17048's own SOC estimate can overshoot past 100% (seen most
  // often while charging) -- a known quirk of its ModelGauge algorithm,
  // not a bug in this wrapper. Clamp so callers always get a real 0-100%.
  float soc = gauge_.getSOC();
  return soc > 100.0f ? 100.0f : soc;
}
