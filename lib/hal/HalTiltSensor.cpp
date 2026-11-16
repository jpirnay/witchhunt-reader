#include "HalTiltSensor.h"

#include <HalCapabilities.h>
#include <HalI2cBus.h>
#include <Logging.h>

HalTiltSensor halTiltSensor;

bool HalTiltSensor::imuRead(Imu::Sample& out) {
  HalI2cBus::Lock i2cLock;
  return _imu.read(out);
}

bool HalTiltSensor::imuSleep() {
  HalI2cBus::Lock i2cLock;
  return _imu.sleep();
}

bool HalTiltSensor::imuWake() {
  HalI2cBus::Lock i2cLock;
  return _imu.wake();
}

void HalTiltSensor::begin() {
  _present = false;
  _available = false;
  if (!HalCapabilities::hasTiltSensor()) {
    return;
  }

  bool found = false;
  {
    HalI2cBus::Lock i2cLock;
    found = _imu.begin();
  }
  if (!found) {
    LOG_ERR("GYR", "IMU not found");
    return;
  }
  _present = true;
  // begin() leaves the sensors sampling; stand them by until a reader wakes them, so an IMU nobody
  // uses does not drain the battery.
  if (!imuSleep()) {
    LOG_ERR("GYR", "IMU standby failed");
  }
  // Tilt reads the gyro; an accelerometer-only part (SC7A20H) cannot drive it.
  _available = _imu.hasGyroscope();
  if (!_available) {
    LOG_INF("GYR", "IMU has no gyroscope");
    return;
  }
  _lastPollMs = millis();
  LOG_INF("GYR", "IMU initialized and put to sleep");
}

bool HalTiltSensor::readSample(Imu::Sample& out) {
  if (!_present) {
    return false;
  }
  if (_isAwake) {
    return imuRead(out);
  }
  if (!imuWake()) {
    return false;
  }
  delay(WAKE_STABILIZE_MS);
  const bool ok = imuRead(out);
  imuSleep();
  return ok;
}

bool HalTiltSensor::wake() {
  if (!_available) {
    return false;
  }
  if (!imuWake()) {
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }
  _lastPollMs = millis();
  _lastTiltMs = millis();
  _wakeMs = millis();
  LOG_INF("GYR", "IMU woke up");
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) {
    return false;
  }
  if (!imuSleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }
  clearPendingEvents();
  _inTilt = false;
  LOG_INF("GYR", "IMU entered sleep mode");
  return true;
}

void HalTiltSensor::update(const CrossPointTiltPageTurn::Value mode, const TiltRate::Axis axis,
                           const CrossPointOrientation::Value orientation, const bool inReader) {
  if (!_available) {
    return;
  }

  if ((mode != CrossPointTiltPageTurn::TILT_OFF && inReader) && !_isAwake) {
    _isAwake = wake();
    return;
  } else if ((mode == CrossPointTiltPageTurn::TILT_OFF || !inReader) && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  if ((mode == CrossPointTiltPageTurn::TILT_OFF) || !inReader) {
    return;
  }

  const unsigned long now = millis();
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  Imu::Sample sample;
  if (!imuRead(sample)) {
    return;
  }
  float tiltAxis = TiltRate::rate(sample.gx, sample.gy, orientation, axis);
  if (mode == CrossPointTiltPageTurn::TILT_INVERTED) {
    tiltAxis = -tiltAxis;
  }

  if (_inTilt) {
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) {
      _inTilt = false;
    }
  } else {
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > RATE_THRESHOLD_DPS) {
        _tiltForwardEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Forward Trigger=(%.1f) dps", tiltAxis);
      } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
        _tiltBackEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Backward Trigger=(%.1f) dps", tiltAxis);
      }
    }
  }
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
}
