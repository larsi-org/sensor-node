#include "SensorNodeStatus.h"

namespace {

SensorNodeStatusFn callback = nullptr;
SensorNodeStatus last;
bool haveLast = false;

uint32_t downBucket(uint32_t seconds) { return seconds < 60 ? seconds / 5 : 12 + seconds / 60; }

}  // namespace

void setSensorNodeStatusCallback(SensorNodeStatusFn fn) {
  callback = fn;
  haveLast = false;  // the next publish always reaches a newly registered callback
}

void publishSensorNodeStatus(const SensorNodeStatus &status) {
  if (!callback) return;
  if (haveLast && status.connected == last.connected && status.portal == last.portal &&
      status.ip == last.ip && status.level() == last.level() &&
      downBucket(status.downSeconds) == downBucket(last.downSeconds)) {
    return;
  }
  last = status;
  haveLast = true;
  callback(status);
}
